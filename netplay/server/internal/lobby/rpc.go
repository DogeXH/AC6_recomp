package lobby

import (
	"bytes"
	"encoding/json"
	"slices"
	"strconv"
	"time"

	"ac6lobby/internal/wire"
)

// RPC limits (PROTOCOL.md section 3 plus DoS hygiene).
const (
	maxRequestBytes  = 32 * 1024
	maxAssemblies    = 8
	maxAssemblyBytes = 64 * 1024
	assemblyTimeout  = 10 * time.Second
	respCacheSize    = 64
	respCacheBytes   = 256 * 1024 // per client, so 64 large responses cannot pin megabytes
	respCacheTTL     = 60 * time.Second
	// maxResponseBytes keeps every response within MaxFragments fragments.
	maxResponseBytes = wire.MaxFragments * wire.MaxFragment
)

type asmKey struct {
	id  uint32
	cnt uint8
}

type assembly struct {
	parts   [][]byte
	have    int
	size    int
	started time.Time
}

type cachedResp struct {
	inners [][]byte // RPC_RESP inner messages, ready to seal
	size   int
	at     time.Time
}

type rpcState struct {
	asm        map[asmKey]*assembly
	asmBytes   int
	cache      map[uint32]*cachedResp
	order      []uint32 // cache insertion order
	cacheBytes int
}

func newRPCState() rpcState {
	return rpcState{asm: map[asmKey]*assembly{}, cache: map[uint32]*cachedResp{}}
}

// rpcError is an RPC failure with a PROTOCOL.md error code.
type rpcError struct {
	code, msg string
}

func errBadRequest(msg string) *rpcError { return &rpcError{"bad_request", msg} }
func errNotFound(msg string) *rpcError   { return &rpcError{"not_found", msg} }

var (
	errNotHost  = &rpcError{"not_host", "only the session host may do this"}
	errFull     = &rpcError{"full", "the session has no open slots"}
	errCompat   = &rpcError{"compat", "the compat key does not match"}
	errRate     = &rpcError{"rate", "rate limit hit"}
	errInternal = &rpcError{"internal", "server error"}
)

func (c *Client) expireRPC(now time.Time) {
	for k, a := range c.rpc.asm {
		if now.Sub(a.started) > assemblyTimeout {
			c.rpc.asmBytes -= a.size
			delete(c.rpc.asm, k)
		}
	}
	keep := c.rpc.order[:0]
	for _, id := range c.rpc.order {
		if e := c.rpc.cache[id]; e != nil && now.Sub(e.at) <= respCacheTTL {
			keep = append(keep, id)
		} else if e != nil {
			c.rpc.cacheBytes -= e.size
			delete(c.rpc.cache, id)
		}
	}
	c.rpc.order = keep
}

// handleRPCFrag reassembles a request, answers duplicates from the cache, and dispatches.
func (s *Server) handleRPCFrag(c *Client, body []byte, now time.Time) {
	f, err := wire.ParseRPCFrag(body)
	if err != nil {
		s.counters.Malformed++
		return
	}
	if e := c.rpc.cache[f.ReqID]; e != nil && now.Sub(e.at) <= respCacheTTL {
		if f.Idx == 0 { // one answer per retransmission round, not one per fragment
			for _, inner := range e.inners {
				s.sendS2C(c, inner)
			}
		}
		return
	}
	msg := f.Data
	if f.Cnt > 1 {
		if msg = c.rpc.addFragment(f, now); msg == nil {
			return
		}
	}
	s.counters.RPCs++
	var resp []byte
	if !c.rpcLimiter.allow(1, now) {
		resp = errorResponse(f.ReqID, errRate)
	} else {
		resp = s.dispatchRPC(c, f.ReqID, msg, now)
	}
	frags := wire.SplitRPC(f.ReqID, resp)
	if frags == nil {
		s.log.Error("rpc response too large", "bytes", len(resp))
		frags = wire.SplitRPC(f.ReqID, errorResponse(f.ReqID, errInternal))
	}
	e := &cachedResp{at: now}
	for _, fr := range frags {
		inner := wire.AppendRPCFrag(nil, wire.KindRPCResp, fr)
		e.inners = append(e.inners, inner)
		e.size += len(inner)
	}
	c.rpc.store(f.ReqID, e)
	for _, inner := range e.inners {
		s.sendS2C(c, inner)
	}
}

// addFragment stores one fragment and returns the whole request once complete.
func (r *rpcState) addFragment(f wire.RPCFrag, now time.Time) []byte {
	k := asmKey{f.ReqID, f.Cnt}
	a := r.asm[k]
	if a == nil {
		if len(r.asm) >= maxAssemblies {
			return nil
		}
		a = &assembly{parts: make([][]byte, f.Cnt), started: now}
		r.asm[k] = a
	}
	if a.parts[f.Idx] != nil {
		return nil // duplicate fragment
	}
	if a.size+len(f.Data) > maxRequestBytes || r.asmBytes+len(f.Data) > maxAssemblyBytes {
		r.asmBytes -= a.size
		delete(r.asm, k)
		return nil
	}
	a.parts[f.Idx] = append([]byte{}, f.Data...) // non-nil even when empty
	a.have++
	a.size += len(f.Data)
	r.asmBytes += len(f.Data)
	if a.have < len(a.parts) {
		return nil
	}
	r.asmBytes -= a.size
	delete(r.asm, k)
	return bytes.Join(a.parts, nil)
}

func (r *rpcState) store(id uint32, e *cachedResp) {
	if old := r.cache[id]; old != nil {
		r.cacheBytes -= old.size
		r.order = slices.DeleteFunc(r.order, func(x uint32) bool { return x == id })
	}
	for len(r.order) > 0 && (len(r.order) >= respCacheSize || r.cacheBytes+e.size > respCacheBytes) {
		oldest := r.order[0]
		r.order = r.order[1:]
		if o := r.cache[oldest]; o != nil {
			r.cacheBytes -= o.size
			delete(r.cache, oldest)
		}
	}
	r.order = append(r.order, id)
	r.cache[id] = e
	r.cacheBytes += e.size
}

type rpcRequest struct {
	ID *json.Number    `json:"id"`
	M  string          `json:"m"`
	A  json.RawMessage `json:"a"`
}

type rpcHandler func(s *Server, c *Client, args json.RawMessage, now time.Time) (any, *rpcError)

func (s *Server) dispatchRPC(c *Client, reqID uint32, msg []byte, now time.Time) []byte {
	var req rpcRequest
	if err := decodeStrict(msg, &req); err != nil {
		return errorResponse(reqID, errBadRequest("malformed request"))
	}
	if req.ID != nil && req.ID.String() != strconv.FormatUint(uint64(reqID), 10) {
		return errorResponse(reqID, errBadRequest("id does not match req_id"))
	}
	h := rpcMethods[req.M]
	if h == nil {
		return errorResponse(reqID, errBadRequest("unknown method"))
	}
	args := req.A
	if len(args) == 0 || string(args) == "null" {
		args = json.RawMessage("{}")
	}
	result, rerr := h(s, c, args, now)
	if rerr != nil {
		s.log.Debug("rpc failed", "peer", hex16(c.acct.PeerID), "m", req.M, "err", rerr.code, "msg", rerr.msg)
		return errorResponse(reqID, rerr)
	}
	s.log.Debug("rpc", "peer", hex16(c.acct.PeerID), "m", req.M)
	out, err := json.Marshal(struct {
		ID uint32 `json:"id"`
		OK bool   `json:"ok"`
		R  any    `json:"r"`
	}{reqID, true, result})
	if err != nil {
		s.log.Error("rpc marshal", "m", req.M, "err", err)
		return errorResponse(reqID, errInternal)
	}
	return out
}

func errorResponse(reqID uint32, e *rpcError) []byte {
	return mustJSON(struct {
		ID  uint32 `json:"id"`
		OK  bool   `json:"ok"`
		Err string `json:"err"`
		Msg string `json:"msg"`
	}{reqID, false, e.code, e.msg})
}

// decodeArgs unmarshals RPC arguments; any error is a bad_request.
func decodeArgs(args json.RawMessage, v any) *rpcError {
	if err := json.Unmarshal(args, v); err != nil {
		return errBadRequest("bad arguments: " + trimErr(err.Error()))
	}
	return nil
}

func trimErr(s string) string {
	if len(s) > 120 {
		return s[:120]
	}
	return s
}
