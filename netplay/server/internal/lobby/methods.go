package lobby

import (
	"cmp"
	"encoding/hex"
	"encoding/json"
	"slices"
	"strconv"
	"strings"
	"time"
)

var rpcMethods map[string]rpcHandler

func init() {
	rpcMethods = map[string]rpcHandler{
		"session.create":    (*Server).rpcSessionCreate,
		"session.get":       (*Server).rpcSessionGet,
		"session.attrs":     (*Server).rpcSessionAttrs,
		"session.modify":    (*Server).rpcSessionModify,
		"session.join":      (*Server).rpcSessionJoin,
		"session.leave":     (*Server).rpcSessionLeave,
		"session.start":     (*Server).rpcSessionStart,
		"session.end":       (*Server).rpcSessionEnd,
		"session.delete":    (*Server).rpcSessionDelete,
		"session.search":    (*Server).rpcSessionSearch,
		"session.arbitrate": (*Server).rpcSessionArbitrate,
		"stats.write":       (*Server).rpcStatsWrite,
		"stats.read":        (*Server).rpcStatsRead,
		"stats.enumerate":   (*Server).rpcStatsEnumerate,
		"storage.read":      (*Server).rpcStorageRead,
		"whoami":            (*Server).rpcWhoami,
	}
}

// Request-size bounds beyond the generic ones.
const (
	maxSearchResults = 50
	maxStatsRows     = 100
	maxStatsXUIDs    = 100
	maxColumns       = 64
	maxJoinMembers   = 64
)

type empty struct{}

// --- helpers ---

func (s *Server) sessionFor(id string) (*Session, *rpcError) {
	v, ok := parseHex16(id)
	if !ok {
		return nil, errBadRequest("session_id must be 16 hex digits")
	}
	sess := s.sessions[v]
	if sess == nil {
		return nil, errNotFound("no such session")
	}
	return sess, nil
}

func (s *Server) hostSession(c *Client, id string) (*Session, *rpcError) {
	sess, err := s.sessionFor(id)
	if err != nil {
		return nil, err
	}
	if sess.Host != c.acct {
		return nil, errNotHost
	}
	return sess, nil
}

func checkTitle(t string) (string, *rpcError) {
	if !validTitle(t) {
		return "", errBadRequest("title must be 8 hex digits")
	}
	return normTitle(t), nil
}

func checkCompat(c *Client, compat string) (string, *rpcError) {
	if !validCompat(compat) {
		return "", errBadRequest("compat must be 32 hex digits")
	}
	compat = strings.ToLower(compat)
	if c.compat != "" && c.compat != compat {
		return "", errCompat
	}
	return compat, nil
}

func checkSlots(pub, priv uint32) *rpcError {
	if uint64(pub)+uint64(priv) > maxSlots {
		return errBadRequest("too many slots")
	}
	return nil
}

func parseXUIDs(in []string, limit int) ([]uint64, *rpcError) {
	if len(in) > limit {
		return nil, errBadRequest("too many xuids")
	}
	out := make([]uint64, 0, len(in))
	for _, x := range in {
		v, ok := parseHex16(x)
		if !ok {
			return nil, errBadRequest("xuid must be 16 hex digits")
		}
		out = append(out, v)
	}
	return out, nil
}

// machineID returns 0xFA00000000000000 | MAC48 with MAC = 00 22 48 + low 24 bits of the vIP.
func machineID(a *Account) uint64 {
	return 0xFA00000000000000 | 0x002248<<24 | uint64(a.VIP&0xFFFFFF)
}

// hostLive reports whether the session's hosting boot is the account's live registration.
func (s *Server) hostLive(sess *Session) bool {
	c := s.clients[sess.Host.PeerID]
	return c != nil && c.boot == sess.HostBoot
}

// --- session.create ---

type createArgs struct {
	Title        string    `json:"title"`
	Compat       string    `json:"compat"`
	Flags        uint32    `json:"flags"`
	PublicSlots  uint32    `json:"public_slots"`
	PrivateSlots uint32    `json:"private_slots"`
	Contexts     []Context `json:"contexts"`
	Properties   []Prop    `json:"properties"`
}

func (s *Server) rpcSessionCreate(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a createArgs
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	title, err := checkTitle(a.Title)
	if err != nil {
		return nil, err
	}
	compat, err := checkCompat(c, a.Compat)
	if err != nil {
		return nil, err
	}
	if err := checkSlots(a.PublicSlots, a.PrivateSlots); err != nil {
		return nil, err
	}
	if validateContexts(a.Contexts) != nil || validateProps(a.Properties) != nil {
		return nil, errBadRequest("too many contexts or properties")
	}
	hosted := 0
	for _, sess := range s.sessions {
		if sess.Host == c.acct {
			hosted++
		}
	}
	if hosted >= maxSessionsPerAccount || len(s.sessions) >= maxSessionsTotal {
		return nil, errRate
	}
	s.seq++
	sess := &Session{
		ID: newSessionID(func(id uint64) bool { return s.sessions[id] != nil }), Key: randomKey16(), Nonce: random64(),
		Title: title, Compat: compat, Flags: a.Flags, PublicSlots: a.PublicSlots, PrivateSlots: a.PrivateSlots,
		Contexts: a.Contexts, Properties: a.Properties, State: stateLobby,
		Host: c.acct, HostBoot: c.boot, Arbitrated: map[uint64]bool{}, seq: s.seq, Created: now, Updated: now,
	}
	s.sessions[sess.ID] = sess
	s.log.Info("session created", "session", hex16(sess.ID), "host", hex16(c.acct.PeerID), "flags", a.Flags,
		"slots", a.PublicSlots+a.PrivateSlots)
	return map[string]string{"session_id": hex16(sess.ID), "key": hex.EncodeToString(sess.Key[:]),
		"nonce": hex16(sess.Nonce)}, nil
}

// --- session.get / attrs / modify ---

type sessionIDArgs struct {
	SessionID string `json:"session_id"`
}

func (s *Server) rpcSessionGet(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a sessionIDArgs
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	sess, err := s.sessionFor(a.SessionID)
	if err != nil {
		return nil, err
	}
	return sess.doc(), nil
}

func (s *Server) rpcSessionAttrs(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		SessionID  string    `json:"session_id"`
		Contexts   []Context `json:"contexts"`
		Properties []Prop    `json:"properties"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	if validateContexts(a.Contexts) != nil || validateProps(a.Properties) != nil {
		return nil, errBadRequest("too many contexts or properties")
	}
	sess, err := s.hostSession(c, a.SessionID)
	if err != nil {
		return nil, err
	}
	sess.Contexts, sess.Properties, sess.Updated = a.Contexts, a.Properties, now
	return empty{}, nil
}

func (s *Server) rpcSessionModify(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		SessionID    string `json:"session_id"`
		Flags        uint32 `json:"flags"`
		PublicSlots  uint32 `json:"public_slots"`
		PrivateSlots uint32 `json:"private_slots"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	if err := checkSlots(a.PublicSlots, a.PrivateSlots); err != nil {
		return nil, err
	}
	sess, err := s.hostSession(c, a.SessionID)
	if err != nil {
		return nil, err
	}
	sess.Flags, sess.PublicSlots, sess.PrivateSlots, sess.Updated = a.Flags, a.PublicSlots, a.PrivateSlots, now
	return empty{}, nil
}

// --- session.join / leave ---

func (s *Server) rpcSessionJoin(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		SessionID string `json:"session_id"`
		Members   []struct {
			XUID    string `json:"xuid"`
			Private bool   `json:"private"`
		} `json:"members"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	if len(a.Members) > maxJoinMembers {
		return nil, errBadRequest("too many members")
	}
	sess, err := s.sessionFor(a.SessionID)
	if err != nil {
		return nil, err
	}
	isHost := sess.Host == c.acct
	if !isHost && c.compat != "" && c.compat != sess.Compat {
		return nil, errCompat
	}
	adds := make([]member, 0, len(a.Members))
	for _, m := range a.Members {
		x, ok := parseHex16(m.XUID)
		if !ok {
			return nil, errBadRequest("xuid must be 16 hex digits")
		}
		if !isHost && x != c.acct.XUID {
			return nil, errNotHost
		}
		if sess.memberIndex(x) >= 0 || slices.ContainsFunc(adds, func(o member) bool { return o.XUID == x }) {
			continue
		}
		adds = append(adds, member{XUID: x, Private: m.Private})
	}
	if len(adds) > 0 && sess.State == stateInGame && sess.Flags&flagJoinInProgressOff != 0 {
		return nil, &rpcError{"state", "the session is in progress and does not allow joins"}
	}
	pub, priv := sess.filled()
	for i := range adds {
		switch {
		case adds[i].Private && priv < sess.PrivateSlots:
			priv++
		case pub < sess.PublicSlots:
			adds[i].Private = false
			pub++
		default:
			return nil, errFull
		}
	}
	sess.Members = append(sess.Members, adds...)
	sess.Updated = now
	return map[string]uint32{"filled_public": pub, "filled_private": priv}, nil
}

func (s *Server) rpcSessionLeave(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		SessionID string   `json:"session_id"`
		XUIDs     []string `json:"xuids"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	xuids, err := parseXUIDs(a.XUIDs, maxJoinMembers)
	if err != nil {
		return nil, err
	}
	sess, err := s.sessionFor(a.SessionID)
	if err != nil {
		return nil, err
	}
	isHost := sess.Host == c.acct
	for _, x := range xuids {
		if !isHost && x != c.acct.XUID {
			return nil, errNotHost
		}
	}
	for _, x := range xuids {
		if i := sess.memberIndex(x); i >= 0 {
			sess.Members = append(sess.Members[:i], sess.Members[i+1:]...)
		}
	}
	sess.Updated = now
	return empty{}, nil
}

// --- session.start / end / delete ---

func (s *Server) rpcSessionStart(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a sessionIDArgs
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	sess, err := s.hostSession(c, a.SessionID)
	if err != nil {
		return nil, err
	}
	if sess.State != stateLobby {
		return nil, &rpcError{"state", "the session is already in progress"}
	}
	sess.State, sess.Updated = stateInGame, now
	return empty{}, nil
}

func (s *Server) rpcSessionEnd(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a sessionIDArgs
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	sess, err := s.hostSession(c, a.SessionID)
	if err != nil {
		return nil, err
	}
	sess.State, sess.Updated = stateLobby, now
	return empty{}, nil
}

func (s *Server) rpcSessionDelete(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a sessionIDArgs
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	sess, err := s.hostSession(c, a.SessionID)
	if err != nil {
		return nil, err
	}
	delete(s.sessions, sess.ID)
	s.log.Info("session deleted", "session", hex16(sess.ID))
	return empty{}, nil
}

// --- session.search ---

type searchArgs struct {
	Title      string    `json:"title"`
	Compat     string    `json:"compat"`
	Proc       uint32    `json:"proc"`
	MaxResults uint32    `json:"max_results"`
	NumUsers   uint32    `json:"num_users"`
	Contexts   []Context `json:"contexts"`
	Properties []Prop    `json:"properties"` // ignored in v1
}

func (s *Server) rpcSessionSearch(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a searchArgs
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	title, err := checkTitle(a.Title)
	if err != nil {
		return nil, err
	}
	if !validCompat(a.Compat) {
		return nil, errBadRequest("compat must be 32 hex digits")
	}
	if validateContexts(a.Contexts) != nil || validateProps(a.Properties) != nil {
		return nil, errBadRequest("too many contexts or properties")
	}
	compat := strings.ToLower(a.Compat)
	limit := int(a.MaxResults)
	if limit == 0 || limit > maxSearchResults {
		limit = maxSearchResults
	}
	matches := make([]*Session, 0, 16)
	for _, sess := range s.sessions {
		if sessionMatches(sess, c, title, compat, &a) && s.hostLive(sess) {
			matches = append(matches, sess)
		}
	}
	slices.SortFunc(matches, func(x, y *Session) int { return cmp.Compare(y.seq, x.seq) })
	results := make([]json.RawMessage, 0, min(limit, len(matches)))
	budget := maxResponseBytes - 256
	for _, sess := range matches {
		if len(results) >= limit {
			break
		}
		d := sess.doc()
		if qs := titleQueries[title]; qs != nil {
			if _, ok := qs.procs[a.Proc]; ok {
				// The XLast query's return attributes: these contexts, no properties.
				d.Contexts, d.Properties = returnContexts(sess, qs.returns), []Prop{}
			}
		}
		js := mustJSON(d)
		if budget -= len(js) + 1; budget < 0 {
			break
		}
		results = append(results, js)
	}
	return map[string]any{"results": results}, nil
}

// matchQuery is one XLast matchmaking query: contexts and properties that must equal the
// search parameter of the same id, and session properties that must equal a constant.
type matchQuery struct {
	contexts  []uint32
	props     []uint32
	constants map[uint32]int64
}

type titleQuerySet struct {
	procs   map[uint32]matchQuery
	returns []uint32 // contexts returned in search results, in order (no properties)
}

// titleQueries holds the matchmaking queries from each title's XLast (AC6: see
// notes/ac6_spa_summary.json). Titles or procs without an entry use the generic v1 rule:
// every search context must exist in the session with an equal value.
var titleQueries = map[string]*titleQuerySet{
	"4E4D07D1": {
		procs: map[uint32]matchQuery{
			0: {contexts: []uint32{0x800A, 0x800B}, props: []uint32{0x1000000C}}, // Find Matches
			1: {contexts: []uint32{0x800A, 0x800B}, props: []uint32{0x1000000C}, // Find Matches 2
				constants: map[uint32]int64{0x1000000D: 0}}, // EnableJoin == ENABLE_JOIN
		},
		returns: []uint32{0x800A, 0x800B},
	},
}

// sessionMatches applies the PROTOCOL.md 3.2 search filters plus the title's query.
func sessionMatches(sess *Session, c *Client, title, compat string, a *searchArgs) bool {
	if sess.Host == c.acct || sess.Title != title || sess.Compat != compat {
		return false
	}
	if sess.Flags&flagMatchmaking == 0 {
		return false
	}
	if sess.State != stateLobby && sess.Flags&flagJoinInProgressOff != 0 {
		return false
	}
	if sess.openPublic() < a.NumUsers {
		return false
	}
	if qs := titleQueries[title]; qs != nil {
		if q, ok := qs.procs[a.Proc]; ok {
			return q.matches(sess, a)
		}
	}
	for _, want := range a.Contexts {
		if !sess.hasContext(want) {
			return false
		}
	}
	return true
}

// matches evaluates the query with Xenia's unresolvable-operand rule: a filter whose
// parameter the search did not supply, or whose attribute the session lacks, is true.
func (q matchQuery) matches(sess *Session, a *searchArgs) bool {
	for _, id := range q.contexts {
		want, ok1 := findContext(a.Contexts, id)
		have, ok2 := findContext(sess.Contexts, id)
		if ok1 && ok2 && want != have {
			return false
		}
	}
	for _, id := range q.props {
		want, ok1 := findProp(a.Properties, id)
		have, ok2 := findProp(sess.Properties, id)
		if ok1 && ok2 && !sameValue(want, have) {
			return false
		}
	}
	for id, v := range q.constants {
		if have, ok := findProp(sess.Properties, id); ok && !sameValue(have, Prop{T: TypeInt64, I: v}) {
			return false
		}
	}
	return true
}

func findContext(cs []Context, id uint32) (uint32, bool) {
	for _, c := range cs {
		if c.ID == id {
			return c.Value, true
		}
	}
	return 0, false
}

func findProp(ps []Prop, id uint32) (Prop, bool) {
	for _, p := range ps {
		if p.ID == id {
			return p, true
		}
	}
	return Prop{}, false
}

// sameValue compares two property values: numbers by value across integer types, doubles
// exactly, strings and blobs bytewise.
func sameValue(a, b Prop) bool {
	isBytes := func(p Prop) bool { return p.T == TypeUnicode || p.T == TypeBinary }
	isFloat := func(p Prop) bool { return p.T == TypeDouble || p.T == TypeFloat }
	switch {
	case isBytes(a) || isBytes(b):
		return a.T == b.T && string(a.B) == string(b.B)
	case isFloat(a) || isFloat(b):
		fa, fb := a.F, b.F
		if !isFloat(a) {
			fa = float64(a.I)
		}
		if !isFloat(b) {
			fb = float64(b.I)
		}
		return fa == fb
	}
	return a.I == b.I
}

// returnContexts lists the session's contexts with the given ids, in that order.
func returnContexts(sess *Session, ids []uint32) []Context {
	out := make([]Context, 0, len(ids))
	for _, id := range ids {
		if v, ok := findContext(sess.Contexts, id); ok {
			out = append(out, Context{ID: id, Value: v})
		}
	}
	return out
}

// --- session.arbitrate ---

type registrantDoc struct {
	MachineID string   `json:"machine_id"`
	Trust     int      `json:"trust"`
	XUIDs     []string `json:"xuids"`
}

func (s *Server) rpcSessionArbitrate(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		SessionID string `json:"session_id"`
		Nonce     string `json:"nonce"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	sess, err := s.sessionFor(a.SessionID)
	if err != nil {
		return nil, err
	}
	if sess.Host != c.acct && sess.memberIndex(c.acct.XUID) < 0 {
		return nil, errNotHost
	}
	if n, ok := parseHex16(a.Nonce); !ok || n != sess.Nonce {
		s.log.Debug("arbitrate with a different nonce", "session", hex16(sess.ID))
	}
	sess.Arbitrated[machineID(c.acct)] = true
	sess.Updated = now

	regs := make([]registrantDoc, 0, len(sess.Members))
	index := map[uint64]int{}
	for _, m := range sess.Members {
		owner := s.accounts.byXUID[m.XUID]
		if owner == nil {
			owner = sess.Host // XUIDs without an account are attributed to the host machine
		}
		mid := machineID(owner)
		i, ok := index[mid]
		if !ok {
			i = len(regs)
			index[mid] = i
			regs = append(regs, registrantDoc{MachineID: hex16(mid), Trust: 1})
		}
		regs[i].XUIDs = append(regs[i].XUIDs, hex16(m.XUID))
	}
	return map[string]any{"registrants": regs}, nil
}

// --- stats ---

type statsWriteArgs struct {
	SessionID string `json:"session_id"`
	Title     string `json:"title"`
	XUID      string `json:"xuid"`
	Views     []struct {
		ViewID uint32 `json:"view_id"`
		Props  []Prop `json:"props"`
	} `json:"views"`
}

func (s *Server) rpcStatsWrite(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a statsWriteArgs
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	title, err := checkTitle(a.Title)
	if err != nil {
		return nil, err
	}
	xuid, ok := parseHex16(a.XUID)
	if !ok {
		return nil, errBadRequest("xuid must be 16 hex digits")
	}
	if len(a.Views) > maxStatsViews {
		return nil, errBadRequest("too many views")
	}
	for _, v := range a.Views {
		if validateProps(v.Props) != nil {
			return nil, errBadRequest("too many properties")
		}
	}
	var sess *Session
	if a.SessionID == "" || a.SessionID == "0000000000000000" {
		// A stats-only session that exists only on the client (single-player boards): the
		// caller may write its own rows, but not to ranked (arbitrated) views.
		if xuid != c.acct.XUID {
			return nil, errNotHost
		}
		for _, v := range a.Views {
			if rankedView(title, v.ViewID) {
				return nil, &rpcError{"state", "ranked views need a server session"}
			}
		}
	} else {
		if sess, err = s.sessionFor(a.SessionID); err != nil {
			return nil, err
		}
		if sess.Title != title {
			return nil, errBadRequest("title does not match the session")
		}
		if sess.Host != c.acct && xuid != c.acct.XUID {
			return nil, errNotHost
		}
	}
	if s.accounts.byXUID[xuid] == nil {
		return nil, errNotFound("no account with that xuid")
	}
	for _, v := range a.Views {
		if err := s.stats.write(title, v.ViewID, xuid, v.Props, now); err != nil {
			return nil, errRate
		}
	}
	if sess != nil {
		sess.Updated = now
	}
	return empty{}, nil
}

// rankedView reports whether a title's view is arbitrated (AC6: views 86-163, from its SPA).
func rankedView(title string, view uint32) bool {
	return title == "4E4D07D1" && view >= 86 && view <= 163
}

type statsSpec struct {
	ViewID  uint32   `json:"view_id"`
	Columns []uint16 `json:"columns"`
}

func (s *Server) gamertagOf(xuid uint64) string {
	if a := s.accounts.byXUID[xuid]; a != nil {
		return a.Gamertag
	}
	return ""
}

// appendRow marshals a row into the view if the response budget allows.
func appendRow(v *statsViewDoc, row statsRowDoc, budget *int) bool {
	d := mustJSON(row)
	if *budget -= len(d) + 1; *budget < 0 {
		return false
	}
	v.Rows = append(v.Rows, d)
	return true
}

func (s *Server) rpcStatsRead(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		Title string      `json:"title"`
		XUIDs []string    `json:"xuids"`
		Specs []statsSpec `json:"specs"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	title, err := checkTitle(a.Title)
	if err != nil {
		return nil, err
	}
	xuids, err := parseXUIDs(a.XUIDs, maxStatsXUIDs)
	if err != nil {
		return nil, err
	}
	if len(a.Specs) > maxStatsViews {
		return nil, errBadRequest("too many specs")
	}
	budget := maxResponseBytes - 256
	views := make([]statsViewDoc, 0, len(a.Specs))
	for _, spec := range a.Specs {
		if len(spec.Columns) > maxColumns {
			return nil, errBadRequest("too many columns")
		}
		v := statsViewDoc{ViewID: spec.ViewID, Rows: []json.RawMessage{}}
		budget -= 64
		if b := s.stats.boards[boardKey{title, spec.ViewID}]; b != nil {
			rows := b.ranked()
			rank := make(map[uint64]int, len(rows))
			for i, r := range rows {
				rank[r.XUID] = i + 1
			}
			v.TotalRows = len(rows)
			for _, x := range xuids {
				r := b.rows[x]
				if r == nil {
					continue
				}
				if !appendRow(&v, rowDoc(r, rank[x], s.gamertagOf(x), spec.Columns), &budget) {
					break
				}
			}
		}
		views = append(views, v)
	}
	return map[string]any{"views": views}, nil
}

func (s *Server) rpcStatsEnumerate(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		Title   string          `json:"title"`
		ViewID  uint32          `json:"view_id"`
		Columns []uint16        `json:"columns"`
		Pivot   string          `json:"pivot"`
		Value   json.RawMessage `json:"value"`
		NumRows uint32          `json:"num_rows"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	title, err := checkTitle(a.Title)
	if err != nil {
		return nil, err
	}
	if len(a.Columns) > maxColumns {
		return nil, errBadRequest("too many columns")
	}
	n := int(min(a.NumRows, maxStatsRows))
	v := statsViewDoc{ViewID: a.ViewID, Rows: []json.RawMessage{}}
	b := s.stats.boards[boardKey{title, a.ViewID}]
	var rows []*statsRow
	if b != nil {
		rows = b.ranked()
		v.TotalRows = len(rows)
	}
	start := 0
	switch a.Pivot {
	case "rank":
		var rank uint64
		if decodeStrict(a.Value, &rank) != nil {
			return nil, errBadRequest("value must be a rank number")
		}
		start = int(min(max(rank, 1)-1, uint64(len(rows))))
	case "xuid":
		var xs string
		if json.Unmarshal(a.Value, &xs) != nil {
			return nil, errBadRequest("value must be a 16-hex xuid")
		}
		x, ok := parseHex16(xs)
		if !ok {
			return nil, errBadRequest("value must be a 16-hex xuid")
		}
		idx := slices.IndexFunc(rows, func(r *statsRow) bool { return r.XUID == x })
		if idx < 0 {
			return map[string]any{"views": []statsViewDoc{v}}, nil
		}
		start = max(0, idx-n/2)
	case "rating":
		var num json.Number
		if decodeStrict(a.Value, &num) != nil {
			return nil, errBadRequest("value must be a rating number")
		}
		rating, perr := parseInt(num, 64)
		if perr != nil {
			f, ferr := strconv.ParseFloat(num.String(), 64)
			if ferr != nil {
				return nil, errBadRequest("value must be a rating number")
			}
			rating = Prop{T: TypeDouble, F: f}.Number()
		}
		idx := slices.IndexFunc(rows, func(r *statsRow) bool { return r.rating <= rating })
		if idx < 0 {
			idx = len(rows)
		}
		start = max(0, idx-n/2)
	default:
		return nil, errBadRequest(`pivot must be "xuid", "rank" or "rating"`)
	}
	budget := maxResponseBytes - 256
	for i := start; i < len(rows) && i < start+n; i++ {
		if !appendRow(&v, rowDoc(rows[i], i+1, s.gamertagOf(rows[i].XUID), a.Columns), &budget) {
			break
		}
	}
	return map[string]any{"views": []statsViewDoc{v}}, nil
}

// --- storage.read / whoami ---

func (s *Server) rpcStorageRead(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	var a struct {
		Path string `json:"path"`
	}
	if err := decodeArgs(args, &a); err != nil {
		return nil, err
	}
	if len(a.Path) > 512 {
		return nil, errBadRequest("path too long")
	}
	return map[string]bool{"found": false}, nil
}

func (s *Server) rpcWhoami(c *Client, args json.RawMessage, now time.Time) (any, *rpcError) {
	return map[string]any{"peer_id": hex16(c.acct.PeerID), "xuid": hex16(c.acct.XUID),
		"vip": ipString(c.acct.VIP), "gamertag": c.acct.Gamertag, "public": addrPair(c.addr)}, nil
}
