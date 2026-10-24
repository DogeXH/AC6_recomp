package lobby

import (
	"encoding/json"
	"net/netip"
	"strings"
	"time"

	"ac6lobby/internal/wire"
)

// Per-client limits.
const (
	maxLocalCands    = 8
	maxIntroduced    = 512
	maxPendingIntros = 32
)

// Client is one live registration: an account plus the boot that registered it.
type Client struct {
	acct    *Account
	boot    wire.Boot
	c2sKey  wire.Key
	s2cKey  wire.Key
	addr    netip.AddrPort // registered address (follows the newest authenticated packet)
	replay  wire.ReplayWindow
	sendCtr uint64

	since     time.Time // registration time
	lastSeen  time.Time
	local     []netip.AddrPort
	privacy   bool
	compat    string // lower-case hex, or "" if the HELLO had none
	title     string
	clientVer string

	rpc rpcState

	ackPending bool // the account is new and not yet on disk: hold the HELLO_ACK
	oldKey     bool // registered under a previous server key (rotation grace)

	introduced    map[uint64]wire.Boot // peer_id -> that peer's boot at introduction
	pendingIntros []*pendingIntro

	rpcLimiter, introLimiter, keepaliveLimiter *bucket
	relayPkts, relayBytes                      *bucket
	relayedPkts, relayedBytes                  uint64 // forwarded from this client
}

func newClient(acct *Account, boot wire.Boot, c2s, s2c wire.Key, addr netip.AddrPort, now time.Time) *Client {
	return &Client{
		acct: acct, boot: boot, c2sKey: c2s, s2cKey: s2c, addr: addr, since: now, lastSeen: now,
		rpc:              newRPCState(),
		introduced:       map[uint64]wire.Boot{},
		rpcLimiter:       newBucket(50, 100, now),
		introLimiter:     newBucket(20, 40, now),
		keepaliveLimiter: newBucket(5, 10, now),
		relayPkts:        newBucket(2000, 2000, now),
		relayBytes:       newBucket(4_000_000/8, 4_000_000/8, now),
	}
}

type helloMsg struct {
	V        int               `json:"v"`
	Client   string            `json:"client"`
	Gamertag string            `json:"gamertag"`
	Title    string            `json:"title"`
	Compat   string            `json:"compat"`
	Local    []json.RawMessage `json:"local"`
	Privacy  bool              `json:"privacy"`
}

type helloAckMsg struct {
	V            int    `json:"v"`
	PeerID       string `json:"peer_id"`
	XUID         string `json:"xuid"`
	VIP          string `json:"vip"`
	Gamertag     string `json:"gamertag"`
	Public       []any  `json:"public"`
	ServerTimeMS int64  `json:"server_time_ms"`
	MOTD         string `json:"motd"`
}

func addrPair(a netip.AddrPort) []any { return []any{a.Addr().String(), a.Port()} }

// parseCandidates reads [["ip", port], ...]; invalid entries are skipped.
func parseCandidates(raw []json.RawMessage) []netip.AddrPort {
	out := make([]netip.AddrPort, 0, min(len(raw), maxLocalCands))
	for _, r := range raw {
		if len(out) >= maxLocalCands {
			break
		}
		var pair []json.RawMessage
		if json.Unmarshal(r, &pair) != nil || len(pair) < 2 {
			continue
		}
		var ip string
		var port uint16
		if json.Unmarshal(pair[0], &ip) != nil || json.Unmarshal(pair[1], &port) != nil || port == 0 {
			continue
		}
		a, err := netip.ParseAddr(ip)
		if err != nil || a.Zone() != "" {
			continue
		}
		a = a.Unmap()
		if a.IsUnspecified() || a.IsMulticast() || a.IsLinkLocalUnicast() && a.Is6() || a.IsInterfaceLocalMulticast() {
			continue
		}
		out = append(out, netip.AddrPortFrom(a, port))
	}
	return out
}

func (c *Client) applyHello(h *helloMsg) {
	c.local = parseCandidates(h.Local)
	c.privacy = h.Privacy
	c.compat = ""
	if validCompat(h.Compat) {
		c.compat = strings.ToLower(h.Compat)
	}
	c.title = ""
	if validTitle(h.Title) {
		c.title = normTitle(h.Title)
	}
	c.clientVer = printableASCII(h.Client, 64)
}

// printableASCII keeps bytes 0x20-0x7E (no control or escape sequences reach logs or the admin
// terminal), at most max of them.
func printableASCII(s string, max int) string {
	b := make([]byte, 0, min(len(s), max))
	for i := 0; i < len(s) && len(b) < max; i++ {
		if c := s[i]; c >= 0x20 && c <= 0x7E {
			b = append(b, c)
		}
	}
	return string(b)
}

// --- HELLO ---

func (s *Server) handleHello(b []byte, from netip.AddrPort) {
	now := s.now()
	f, sealed, err := wire.ParseHello(b)
	if err != nil || f.Ctr == 0 {
		s.counters.Malformed++
		return
	}
	if !s.helloLimiter.allow(from.Addr(), 1, now) || !s.helloGlobal.allow(1, now) {
		s.log.Debug("hello rate limited", "from", from)
		return
	}
	if s.bannedHello(f.ClientPK, now) {
		return // banned accounts get no answer, like any refused HELLO
	}
	acct := s.accounts.byPK[f.ClientPK]
	var dh wire.Key
	if acct != nil && acct.dhOK {
		dh = acct.dh
	} else if dh, err = s.dhFor(f.ClientPK); err != nil {
		s.counters.Malformed++
		return
	}
	var existing *Client
	if acct != nil {
		existing = s.clients[acct.PeerID]
	}
	same := existing != nil && existing.boot == f.Boot
	if same && !existing.replay.Check(f.Ctr) || !same && acct != nil && acct.staleHello(f.Boot, f.Ctr) {
		return
	}
	c2s := wire.C2SKey(dh, f.ClientPK, s.pk, f.Boot)
	sdh, spk, oldKey := dh, s.pk, false // the DH and server key the HELLO authenticated under
	pt, err := wire.Open(c2s, f.Ctr, b[:wire.HelloHeader], sealed)
	if err != nil {
		var ok bool
		if pt, c2s, sdh, spk, ok = s.openWithPrevKeys(f, b[:wire.HelloHeader], sealed); !ok {
			s.counters.Malformed++
			return
		}
		oldKey = true
	}
	var h helloMsg
	if err := json.Unmarshal(pt, &h); err != nil || h.V != 1 {
		s.counters.Malformed++
		return
	}
	s.counters.Hellos++
	if acct != nil && !acct.dhOK {
		acct.dh, acct.dhOK = dh, true // cache only after the HELLO authenticated
	}

	if same && existing.oldKey != oldKey {
		return // the same boot cannot switch server keys; a new boot can
	}
	if same {
		if existing.replay.Accept(f.Ctr) {
			existing.addr = from
		}
		existing.lastSeen = now
		existing.applyHello(&h)
		s.accounts.rename(acct, h.Gamertag)
		if !existing.ackPending {
			s.sendHelloAck(existing)
		}
		return
	}

	if existing == nil && len(s.clients) >= maxClients {
		s.log.Warn("client limit reached; hello dropped")
		return
	}
	if acct == nil {
		if !s.accountLimiter.allow(from.Addr(), 1, now) {
			s.accountLimitHits++
			if now.Sub(s.accountLimitLogged) >= time.Minute {
				attrs := []any{"hits", s.accountLimitHits}
				if s.cfg.LogAddresses {
					attrs = append(attrs, "from", from.Addr())
				}
				s.log.Warn("new-account limit hit; hellos dropped", attrs...)
				s.accountLimitLogged, s.accountLimitHits = now, 0
			}
			s.log.Debug("new-account limit hit", "from", from.Addr())
			return
		}
		if acct, err = s.accounts.create(f.ClientPK, dh, h.Gamertag, now); err != nil {
			s.log.Error("account create failed", "err", err)
			return
		}
		s.log.Info("account created", "peer", hex16(acct.PeerID), "vip", ipString(acct.VIP), "gamertag", acct.Gamertag)
	} else {
		s.accounts.rename(acct, h.Gamertag)
	}

	s2c := wire.S2CKey(sdh, f.ClientPK, spk, f.Boot, s.boot)
	c := newClient(acct, f.Boot, c2s, s2c, from, now)
	c.oldKey = oldKey
	// Nothing older than this HELLO is accepted, so packets captured before a timeout or a
	// BYE cannot be replayed into the new registration.
	c.replay.Reset(f.Ctr)
	// The s2c key depends only on the two boots, so a boot that registers again in this
	// server process reuses it: start the counter above anything an earlier registration
	// used (remembered per boot; the clock-based base covers a forgotten boot).
	c.sendCtr = max(s.counterBase(), acct.lastSendCtr(f.Boot))
	c.applyHello(&h)
	for r := range s.retired {
		if r.acct == acct && r.boot == f.Boot {
			delete(s.retired, r) // the boot is back; its sessions belong to c again
		}
	}
	s.clients[acct.PeerID] = c
	if existing != nil {
		s.log.Info("client restarted; old registration replaced", "peer", hex16(acct.PeerID))
		existing.pendingIntros = nil
		acct.rememberBoot(existing.boot, existing.replay.Highest(), existing.sendCtr)
		if s.hostsSessions(existing) {
			s.retired[existing] = true // its sessions live until its liveness timeout
		} else {
			s.goOffline(existing)
		}
	}
	s.log.Info("client online", "peer", hex16(acct.PeerID), "gamertag", acct.Gamertag, "client", c.clientVer,
		"old_server_key", oldKey)
	s.log.Debug("client address", "peer", hex16(acct.PeerID), "addr", from)
	if s.accountsW != nil && acct.gen > s.accountsWritten {
		c.ackPending = true
		s.pendingAcks = append(s.pendingAcks, c)
		s.persist(false) // submits at once while acks are pending
		return
	}
	s.sendHelloAck(c)
}

func (s *Server) sendHelloAck(c *Client) {
	msg := helloAckMsg{V: 1, PeerID: hex16(c.acct.PeerID), XUID: hex16(c.acct.XUID), VIP: ipString(c.acct.VIP),
		Gamertag: c.acct.Gamertag, Public: addrPair(c.addr), ServerTimeMS: s.now().UnixMilli(), MOTD: s.cfg.MOTD}
	c.sendCtr++
	s.send(wire.BuildServerPacket(wire.TypeHelloAck, s.boot, c.sendCtr, c.s2cKey, mustJSON(msg)), c.addr)
}

// --- C2S ---

func (s *Server) handleC2S(b []byte, from netip.AddrPort) {
	now := s.now()
	f, sealed, err := wire.ParseC2S(b)
	if err != nil {
		s.counters.Malformed++
		return
	}
	c := s.clients[f.PeerID]
	if c == nil || c.boot != f.Boot {
		if s.rehelloLimiter.allow(from, 1, now) {
			s.counters.Rehellos++
			s.send(wire.Rehello, from)
		}
		return
	}
	if !c.replay.Check(f.Ctr) {
		return
	}
	inner, err := wire.Open(c.c2sKey, f.Ctr, b[:wire.C2SHeader], sealed)
	if err != nil {
		s.counters.Malformed++
		return
	}
	if c.replay.Accept(f.Ctr) && from != c.addr {
		s.log.Debug("client address changed", "peer", hex16(c.acct.PeerID), "addr", from)
		c.addr = from
	}
	c.lastSeen = now
	s.handleInner(c, inner, now)
}

// handleInner dispatches one decrypted C2S inner message.
func (s *Server) handleInner(c *Client, inner []byte, now time.Time) {
	if len(inner) == 0 {
		return
	}
	body := inner[1:]
	switch inner[0] {
	case wire.KindRPCReq:
		s.handleRPCFrag(c, body, now)
	case wire.KindKeepalive:
		s.handleKeepalive(c, body, now)
	case wire.KindIntroReq:
		s.handleIntroReq(c, body, now)
	case wire.KindBye:
		s.log.Info("client offline", "peer", hex16(c.acct.PeerID), "reason", "bye")
		s.goOffline(c)
	default:
		s.counters.Malformed++
	}
}

func (s *Server) handleKeepalive(c *Client, body []byte, now time.Time) {
	if len(body) > 0 {
		var ka struct {
			Local []json.RawMessage `json:"local"`
		}
		if json.Unmarshal(body, &ka) == nil && ka.Local != nil {
			c.local = parseCandidates(ka.Local)
		}
	}
	if !c.keepaliveLimiter.allow(1, now) {
		return
	}
	ack := map[string]any{"public": addrPair(c.addr)}
	s.sendS2C(c, append([]byte{wire.KindKeepaliveAck}, mustJSON(ack)...))
}

func mustJSON(v any) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		panic(err) // only server-built values are marshaled here
	}
	return b
}
