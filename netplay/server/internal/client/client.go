// Package client is a small Go implementation of the ac6net client side of the protocol
// (HELLO, RPC, keepalive, introductions, relayed P2P). The server tests use it over real UDP;
// it is also a reference for the C++ overlay.
package client

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"sync"
	"time"

	"ac6lobby/internal/wire"
)

// HelloInfo is the content of the JSON hello.
type HelloInfo struct {
	Gamertag string
	Title    string
	Compat   string
	Local    [][]any // [["ip", port], ...]
	Privacy  bool
}

// HelloAck is the decoded HELLO_ACK.
type HelloAck struct {
	V            int    `json:"v"`
	PeerID       string `json:"peer_id"`
	XUID         string `json:"xuid"`
	VIP          string `json:"vip"`
	Gamertag     string `json:"gamertag"`
	Public       []any  `json:"public"`
	ServerTimeMS int64  `json:"server_time_ms"`
	MOTD         string `json:"motd"`
}

// PeerRecord is the JSON peer record of an INTRO.
type PeerRecord struct {
	PeerID   string  `json:"peer_id"`
	VIP      string  `json:"vip"`
	PK       string  `json:"pk"`
	Boot     string  `json:"boot"`
	XUID     string  `json:"xuid"`
	Gamertag string  `json:"gamertag"`
	Cands    [][]any `json:"cands"`
}

// Intro is a received INTRO.
type Intro struct {
	ReqID uint32
	Peer  PeerRecord
}

// IntroFail is a received INTRO_FAIL.
type IntroFail struct {
	ReqID  uint32
	Reason uint8
}

// Relayed is a decrypted P2P packet received through the relay.
type Relayed struct {
	Src   uint64
	Inner []byte // decrypted P2P inner message
}

// RPCError is an ok:false response.
type RPCError struct {
	Code, Msg string
}

func (e *RPCError) Error() string { return e.Code + ": " + e.Msg }

// ErrTimeout is returned when no answer arrived in time.
var ErrTimeout = errors.New("client: timeout")

type peerKeys struct {
	rec      PeerRecord
	id       uint64
	send     wire.Key
	recv     wire.Key
	sendCtr  uint64
	recvSeen wire.ReplayWindow
}

type rpcAsm struct {
	parts [][]byte
	have  int
}

// Client is one ac6net client instance with its own UDP socket and identity.
type Client struct {
	conn     *net.UDPConn
	server   netip.AddrPort
	serverPK wire.Key
	sk, pk   wire.Key
	boot     wire.Boot
	dh       wire.Key
	c2sKey   wire.Key

	// AutoRehello resends HELLO (at most once per second) when the server sends REHELLO.
	AutoRehello bool

	mu        sync.Mutex
	ctr       uint64
	hello     HelloInfo
	ack       HelloAck
	peerID    uint64
	haveS2C   bool
	sBoot     wire.Boot
	s2cKey    wire.Key
	s2cReplay wire.ReplayWindow
	nextReq   uint32
	calls     map[uint32]chan []byte
	asm       map[uint32]*rpcAsm
	peers     map[uint64]*peerKeys
	rehellos  int
	lastHello time.Time

	relayedDropped uint64 // relayed packets dropped because the queue was full

	rehelloPending bool

	acks       chan HelloAck
	intros     chan Intro
	introFails chan IntroFail
	kaAcks     chan []byte
	notices    chan []byte
	relayed    chan Relayed
	done       chan struct{}
}

// Options configure NewWithOptions.
type Options struct {
	// Local is the UDP address to bind, for example "0.0.0.0:0" to reach a remote server.
	// Empty = "127.0.0.1:0".
	Local string
	// QueueLen is the capacity of the relayed-packet queue (default 256). Packets that arrive
	// while it is full are dropped and counted (RelayedDropped).
	QueueLen int
}

// New opens a UDP socket on 127.0.0.1 and starts the receive loop.
func New(server string, serverPK, sk wire.Key) (*Client, error) {
	return NewWithOptions(server, serverPK, sk, Options{})
}

// NewWithOptions is New with a configurable local address and queue size. server must be an
// IP:port (resolve names first).
func NewWithOptions(server string, serverPK, sk wire.Key, o Options) (*Client, error) {
	sa, err := netip.ParseAddrPort(server)
	if err != nil {
		return nil, err
	}
	sa = netip.AddrPortFrom(sa.Addr().Unmap(), sa.Port())
	local := netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), 0)
	if o.Local != "" {
		if local, err = netip.ParseAddrPort(o.Local); err != nil {
			return nil, err
		}
	}
	if o.QueueLen <= 0 {
		o.QueueLen = 256
	}
	conn, err := net.ListenUDP("udp", net.UDPAddrFromAddrPort(local))
	if err != nil {
		return nil, err
	}
	c := &Client{
		conn: conn, server: sa, serverPK: serverPK, sk: sk, pk: wire.PublicKey(sk),
		calls: map[uint32]chan []byte{}, asm: map[uint32]*rpcAsm{}, peers: map[uint64]*peerKeys{},
		acks: make(chan HelloAck, 16), intros: make(chan Intro, 64), introFails: make(chan IntroFail, 64),
		kaAcks: make(chan []byte, 16), notices: make(chan []byte, 16), relayed: make(chan Relayed, o.QueueLen),
		done: make(chan struct{}),
	}
	if _, err := rand.Read(c.boot[:]); err != nil {
		conn.Close()
		return nil, err
	}
	if c.dh, err = wire.DH(sk, serverPK); err != nil {
		conn.Close()
		return nil, err
	}
	c.c2sKey = wire.C2SKey(c.dh, c.pk, serverPK, c.boot)
	go c.readLoop()
	return c, nil
}

// NewKey returns a random secret key.
func NewKey() wire.Key {
	var k wire.Key
	if _, err := rand.Read(k[:]); err != nil {
		panic(err)
	}
	return k
}

// Close stops the client.
func (c *Client) Close() {
	c.conn.Close()
	<-c.done
}

// LocalAddr is the client's UDP address.
func (c *Client) LocalAddr() netip.AddrPort { return c.conn.LocalAddr().(*net.UDPAddr).AddrPort() }

// PublicKey, Boot and PeerID describe this client.
func (c *Client) PublicKey() wire.Key { return c.pk }
func (c *Client) Boot() wire.Boot     { return c.boot }
func (c *Client) PeerID() uint64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.peerID
}

// Ack returns the latest HELLO_ACK.
func (c *Client) Ack() HelloAck {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.ack
}

// Rehellos counts REHELLO packets received.
func (c *Client) Rehellos() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.rehellos
}

// SendRaw sends b to the server unchanged.
func (c *Client) SendRaw(b []byte) error {
	_, err := c.conn.WriteToUDPAddrPort(b, c.server)
	return err
}

func (c *Client) nextCtrLocked() uint64 {
	c.ctr++
	return c.ctr
}

func (c *Client) helloPacketLocked() []byte {
	h := map[string]any{"v": 1, "client": "ac6lobby-testclient/0.1", "gamertag": c.hello.Gamertag,
		"title": c.hello.Title, "compat": c.hello.Compat, "local": c.hello.Local, "privacy": c.hello.Privacy}
	if c.hello.Local == nil {
		h["local"] = [][]any{}
	}
	js, _ := json.Marshal(h)
	c.lastHello = time.Now()
	return wire.BuildHello(wire.HelloFields{ClientPK: c.pk, Boot: c.boot, Ctr: c.nextCtrLocked()}, c.c2sKey, js)
}

// Hello registers with the server, resending every 300 ms until HELLO_ACK or timeout.
func (c *Client) Hello(info HelloInfo, timeout time.Duration) (HelloAck, error) {
	c.mu.Lock()
	c.hello = info
	c.mu.Unlock()
	deadline := time.After(timeout)
	for {
		c.mu.Lock()
		pkt := c.helloPacketLocked()
		c.mu.Unlock()
		if err := c.SendRaw(pkt); err != nil {
			return HelloAck{}, err
		}
		select {
		case a := <-c.acks:
			return a, nil
		case <-time.After(300 * time.Millisecond):
		case <-deadline:
			return HelloAck{}, ErrTimeout
		}
	}
}

// HelloPacket builds the next HELLO (with the last HelloInfo) without sending it.
func (c *Client) HelloPacket() []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.helloPacketLocked()
}

// S2CHighest is the highest S2C/HELLO_ACK counter accepted from the current server boot.
func (c *Client) S2CHighest() uint64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.s2cReplay.Highest()
}

// WaitAck waits for the next HELLO_ACK (for example after an automatic re-HELLO).
func (c *Client) WaitAck(timeout time.Duration) (HelloAck, error) {
	select {
	case a := <-c.acks:
		return a, nil
	case <-time.After(timeout):
		return HelloAck{}, ErrTimeout
	}
}

// SendC2S seals inner as a C2S packet and sends it.
func (c *Client) SendC2S(inner []byte) error {
	c.mu.Lock()
	pkt := wire.BuildC2S(wire.C2SFields{PeerID: c.peerID, Boot: c.boot, Ctr: c.nextCtrLocked()}, c.c2sKey, inner)
	c.mu.Unlock()
	return c.SendRaw(pkt)
}

// BuildC2S returns a sealed C2S packet without sending it (for replay tests).
func (c *Client) BuildC2S(inner []byte) []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	return wire.BuildC2S(wire.C2SFields{PeerID: c.peerID, Boot: c.boot, Ctr: c.nextCtrLocked()}, c.c2sKey, inner)
}

// CallRaw performs an RPC and returns the raw "r" object, retransmitting every 300 ms.
func (c *Client) CallRaw(method string, args any, timeout time.Duration) (json.RawMessage, error) {
	if args == nil {
		args = map[string]any{}
	}
	c.mu.Lock()
	c.nextReq++
	id := c.nextReq
	ch := make(chan []byte, 1)
	c.calls[id] = ch
	c.mu.Unlock()
	defer func() {
		c.mu.Lock()
		delete(c.calls, id)
		c.mu.Unlock()
	}()
	msg, err := json.Marshal(map[string]any{"id": id, "m": method, "a": args})
	if err != nil {
		return nil, err
	}
	frags := wire.SplitRPC(id, msg)
	if frags == nil {
		return nil, errors.New("client: request too large")
	}
	deadline := time.After(timeout)
	for {
		for _, f := range frags {
			if err := c.SendC2S(wire.AppendRPCFrag(nil, wire.KindRPCReq, f)); err != nil {
				return nil, err
			}
		}
		select {
		case resp := <-ch:
			var r struct {
				ID  uint32          `json:"id"`
				OK  bool            `json:"ok"`
				R   json.RawMessage `json:"r"`
				Err string          `json:"err"`
				Msg string          `json:"msg"`
			}
			if err := json.Unmarshal(resp, &r); err != nil {
				return nil, fmt.Errorf("client: bad response: %w", err)
			}
			if r.ID != id {
				return nil, fmt.Errorf("client: response id %d for request %d", r.ID, id)
			}
			if !r.OK {
				return nil, &RPCError{r.Err, r.Msg}
			}
			return r.R, nil
		case <-time.After(300 * time.Millisecond):
		case <-deadline:
			return nil, ErrTimeout
		}
	}
}

// Call performs an RPC and decodes "r" into out (if non-nil).
func (c *Client) Call(method string, args, out any) error {
	r, err := c.CallRaw(method, args, 6*time.Second)
	if err != nil {
		return err
	}
	if out != nil {
		return json.Unmarshal(r, out)
	}
	return nil
}

// Keepalive sends KEEPALIVE with an optional JSON body and waits for KEEPALIVE_ACK.
func (c *Client) Keepalive(body []byte, timeout time.Duration) ([]byte, error) {
	if err := c.SendC2S(append([]byte{wire.KindKeepalive}, body...)); err != nil {
		return nil, err
	}
	select {
	case b := <-c.kaAcks:
		return b, nil
	case <-time.After(timeout):
		return nil, ErrTimeout
	}
}

// WaitKeepaliveAck waits for a KEEPALIVE_ACK without sending anything.
func (c *Client) WaitKeepaliveAck(timeout time.Duration) ([]byte, error) {
	select {
	case b := <-c.kaAcks:
		return b, nil
	case <-time.After(timeout):
		return nil, ErrTimeout
	}
}

// Bye tells the server this client is going away.
func (c *Client) Bye() error { return c.SendC2S([]byte{wire.KindBye}) }

// IntroReq sends INTRO_REQ by vip (host-order u32) or by peer ID.
func (c *Client) IntroReq(reqID uint32, by uint8, value uint64) error {
	return c.SendC2S(wire.AppendIntroReq(nil, reqID, by, value))
}

// WaitIntro waits for the next INTRO.
func (c *Client) WaitIntro(timeout time.Duration) (Intro, error) {
	select {
	case i := <-c.intros:
		return i, nil
	case <-time.After(timeout):
		return Intro{}, ErrTimeout
	}
}

// WaitIntroFail waits for the next INTRO_FAIL.
func (c *Client) WaitIntroFail(timeout time.Duration) (IntroFail, error) {
	select {
	case f := <-c.introFails:
		return f, nil
	case <-time.After(timeout):
		return IntroFail{}, ErrTimeout
	}
}

// WaitNotice waits for the next NOTICE body.
func (c *Client) WaitNotice(timeout time.Duration) ([]byte, error) {
	select {
	case n := <-c.notices:
		return n, nil
	case <-time.After(timeout):
		return nil, ErrTimeout
	}
}

// IntroCount drains pending INTRO events and returns how many there were.
func (c *Client) IntroCount() int {
	n := 0
	for {
		select {
		case <-c.intros:
			n++
		default:
			return n
		}
	}
}

// AddPeer derives P2P keys for a peer record from an INTRO.
func (c *Client) AddPeer(rec PeerRecord) (uint64, error) {
	var pk wire.Key
	var boot wire.Boot
	id, err := parseHex64(rec.PeerID)
	if err != nil {
		return 0, err
	}
	if b, err := hex.DecodeString(rec.PK); err != nil || len(b) != 32 {
		return 0, errors.New("client: bad pk")
	} else {
		copy(pk[:], b)
	}
	if b, err := hex.DecodeString(rec.Boot); err != nil || len(b) != 8 {
		return 0, errors.New("client: bad boot")
	} else {
		copy(boot[:], b)
	}
	dh, err := wire.DH(c.sk, pk)
	if err != nil {
		return 0, err
	}
	c.mu.Lock()
	c.peers[id] = &peerKeys{rec: rec, id: id,
		send: wire.P2PKey(dh, c.pk, pk, c.boot, boot),
		recv: wire.P2PKey(dh, pk, c.pk, boot, c.boot)}
	c.mu.Unlock()
	return id, nil
}

// BuildRelay returns a RELAY datagram carrying a P2P packet with inner for peer dst.
func (c *Client) BuildRelay(dst uint64, inner []byte) ([]byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	p := c.peers[dst]
	if p == nil {
		return nil, errors.New("client: unknown peer")
	}
	p.sendCtr++
	return wire.BuildRelay(c.peerID, dst, wire.BuildP2P(c.peerID, p.sendCtr, p.send, inner)), nil
}

// SendRelay sends inner to peer dst through the server relay.
func (c *Client) SendRelay(dst uint64, inner []byte) error {
	pkt, err := c.BuildRelay(dst, inner)
	if err != nil {
		return err
	}
	return c.SendRaw(pkt)
}

// WaitRelayed waits for the next relayed P2P packet.
func (c *Client) WaitRelayed(timeout time.Duration) (Relayed, error) {
	select {
	case r := <-c.relayed:
		return r, nil
	case <-time.After(timeout):
		return Relayed{}, ErrTimeout
	}
}

// --- receive path ---

func (c *Client) readLoop() {
	defer close(c.done)
	buf := make([]byte, 2048)
	for {
		n, from, err := c.conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			return
		}
		if netip.AddrPortFrom(from.Addr().Unmap(), from.Port()) != c.server || n < 2 || buf[0] != wire.Magic {
			continue
		}
		b := append([]byte{}, buf[:n]...)
		switch b[1] {
		case wire.TypeHelloAck, wire.TypeS2C:
			c.handleServerPacket(b)
		case wire.TypeRehello:
			c.mu.Lock()
			c.rehellos++
			if c.AutoRehello && !c.rehelloPending {
				// At most one HELLO per second: send now or when the second is up.
				c.rehelloPending = true
				wait := max(0, time.Second-time.Since(c.lastHello))
				time.AfterFunc(wait, c.sendRehello)
			}
			c.mu.Unlock()
		case wire.TypeRelayed:
			c.handleRelayed(b)
		}
	}
}

func (c *Client) sendRehello() {
	c.mu.Lock()
	c.rehelloPending = false
	pkt := c.helloPacketLocked()
	c.mu.Unlock()
	c.SendRaw(pkt)
}

func (c *Client) handleServerPacket(b []byte) {
	typ, sBoot, ctr, sealed, err := wire.ParseServerPacket(b)
	if err != nil {
		return
	}
	c.mu.Lock()
	key, replay := c.s2cKey, &c.s2cReplay
	fresh := false
	if !c.haveS2C || sBoot != c.sBoot {
		if typ != wire.TypeHelloAck {
			c.mu.Unlock()
			return // S2C from an unknown server boot
		}
		key, replay, fresh = wire.S2CKey(c.dh, c.pk, c.serverPK, c.boot, sBoot), &wire.ReplayWindow{}, true
	}
	if !replay.Check(ctr) {
		c.mu.Unlock()
		return
	}
	pt, err := wire.Open(key, ctr, b[:wire.S2CHeader], sealed)
	if err != nil {
		c.mu.Unlock()
		return
	}
	if fresh {
		c.haveS2C, c.sBoot, c.s2cKey, c.s2cReplay = true, sBoot, key, *replay
		replay = &c.s2cReplay
	}
	replay.Accept(ctr)
	if typ == wire.TypeHelloAck {
		var a HelloAck
		if json.Unmarshal(pt, &a) == nil {
			c.ack = a
			if id, err := parseHex64(a.PeerID); err == nil {
				c.peerID = id
			}
			c.mu.Unlock()
			select {
			case c.acks <- a:
			default:
			}
			return
		}
		c.mu.Unlock()
		return
	}
	c.mu.Unlock()
	c.handleInner(pt)
}

func (c *Client) handleInner(pt []byte) {
	if len(pt) == 0 {
		return
	}
	body := pt[1:]
	switch pt[0] {
	case wire.KindRPCResp:
		f, err := wire.ParseRPCFrag(body)
		if err != nil {
			return
		}
		c.mu.Lock()
		ch := c.calls[f.ReqID]
		var full []byte
		if ch != nil {
			if f.Cnt == 1 {
				full = f.Data
			} else {
				a := c.asm[f.ReqID]
				if a == nil || len(a.parts) != int(f.Cnt) {
					a = &rpcAsm{parts: make([][]byte, f.Cnt)}
					c.asm[f.ReqID] = a
				}
				if a.parts[f.Idx] == nil {
					a.parts[f.Idx] = append([]byte{}, f.Data...)
					a.have++
				}
				if a.have == len(a.parts) {
					for _, p := range a.parts {
						full = append(full, p...)
					}
					delete(c.asm, f.ReqID)
				}
			}
		}
		c.mu.Unlock()
		if full != nil {
			select {
			case ch <- full:
			default:
			}
		}
	case wire.KindKeepaliveAck:
		select {
		case c.kaAcks <- append([]byte{}, body...):
		default:
		}
	case wire.KindIntro:
		if len(body) < 4 {
			return
		}
		var rec PeerRecord
		if json.Unmarshal(body[4:], &rec) != nil {
			return
		}
		select {
		case c.intros <- Intro{ReqID: binary.LittleEndian.Uint32(body[:4]), Peer: rec}:
		default:
		}
	case wire.KindIntroFail:
		if len(body) != 5 {
			return
		}
		select {
		case c.introFails <- IntroFail{ReqID: binary.LittleEndian.Uint32(body[:4]), Reason: body[4]}:
		default:
		}
	case wire.KindNotice:
		select {
		case c.notices <- append([]byte{}, body...):
		default:
		}
	}
}

func (c *Client) handleRelayed(b []byte) {
	src, inner, err := wire.ParseRelayed(b)
	if err != nil {
		return
	}
	psrc, ctr, sealed, err := wire.ParseP2P(inner)
	if err != nil || psrc != src {
		return
	}
	c.mu.Lock()
	p := c.peers[src]
	if p == nil || !p.recvSeen.Check(ctr) {
		c.mu.Unlock()
		return
	}
	pt, err := wire.Open(p.recv, ctr, inner[:wire.P2PHeader], sealed)
	if err == nil {
		p.recvSeen.Accept(ctr)
	}
	c.mu.Unlock()
	if err != nil {
		return
	}
	select {
	case c.relayed <- Relayed{Src: src, Inner: pt}:
	default:
		c.mu.Lock()
		c.relayedDropped++
		c.mu.Unlock()
	}
}

// RelayedDropped counts relayed packets dropped because nobody drained WaitRelayed in time.
func (c *Client) RelayedDropped() uint64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.relayedDropped
}

// RelayedChan returns the queue of decrypted relayed packets (the channel WaitRelayed reads).
func (c *Client) RelayedChan() <-chan Relayed { return c.relayed }

// HasPeer reports whether AddPeer was called for the peer ID.
func (c *Client) HasPeer(id uint64) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.peers[id] != nil
}

func parseHex64(s string) (uint64, error) {
	b, err := hex.DecodeString(s)
	if err != nil || len(b) != 8 {
		return 0, fmt.Errorf("client: bad 16-hex id %q", s)
	}
	return binary.BigEndian.Uint64(b), nil
}
