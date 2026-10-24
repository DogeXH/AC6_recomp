package lobby

import (
	"encoding/hex"
	"net/netip"
	"time"

	"ac6lobby/internal/wire"
)

// pendingIntro is an INTRO awaiting confirmation: the receiver sending any packet with a
// counter above baseCtr.
type pendingIntro struct {
	inner   []byte
	baseCtr uint64
	resends int
	next    time.Time
}

type peerRecord struct {
	PeerID   string  `json:"peer_id"`
	VIP      string  `json:"vip"`
	PK       string  `json:"pk"`
	Boot     string  `json:"boot"`
	XUID     string  `json:"xuid"`
	Gamertag string  `json:"gamertag"`
	Cands    [][]any `json:"cands"`
}

func (s *Server) handleIntroReq(c *Client, body []byte, now time.Time) {
	reqID, by, value, err := wire.ParseIntroReq(body)
	if err != nil {
		s.counters.Malformed++
		return
	}
	fail := func(reason uint8) {
		s.sendS2C(c, wire.AppendIntroFail(nil, reqID, reason))
	}
	if !c.introLimiter.allow(1, now) {
		fail(wire.IntroFailRefused)
		return
	}
	var target *Account
	if by == wire.IntroByVIP {
		target = s.accounts.byVIP[uint32(value)]
	} else {
		target = s.accounts.byPeer[value]
	}
	switch {
	case target == nil:
		fail(wire.IntroFailUnknown)
		return
	case target == c.acct:
		fail(wire.IntroFailRefused)
		return
	}
	t := s.clients[target.PeerID]
	if t == nil {
		fail(wire.IntroFailOffline)
		return
	}
	_, already := c.introduced[t.acct.PeerID]
	if !already && (len(c.introduced) >= maxIntroduced || len(t.introduced) >= maxIntroduced) {
		fail(wire.IntroFailRefused)
		return
	}
	c.introduced[t.acct.PeerID] = t.boot
	t.introduced[c.acct.PeerID] = c.boot
	s.counters.Intros++
	s.log.Debug("introduced", "a", hex16(c.acct.PeerID), "b", hex16(t.acct.PeerID))
	s.sendIntro(c, reqID, t, now)
	s.sendIntro(t, 0, c, now)
}

// sendIntro sends INTRO(about) to to, and schedules up to 3 resends.
func (s *Server) sendIntro(to *Client, reqID uint32, about *Client, now time.Time) {
	inner := wire.AppendIntro(nil, reqID, mustJSON(s.peerRecord(about, to)))
	s.sendS2C(to, inner)
	if len(to.pendingIntros) >= maxPendingIntros {
		to.pendingIntros = append(to.pendingIntros[:0], to.pendingIntros[1:]...)
	}
	to.pendingIntros = append(to.pendingIntros, &pendingIntro{inner: inner, baseCtr: to.replay.Highest(),
		next: now.Add(introResendEvery)})
}

func (s *Server) resendIntros(now time.Time) {
	for _, c := range s.clients {
		if len(c.pendingIntros) == 0 {
			continue
		}
		keep := c.pendingIntros[:0]
		for _, p := range c.pendingIntros {
			if c.replay.Highest() > p.baseCtr || p.resends >= introMaxResends {
				continue // confirmed, or out of resends
			}
			if !now.Before(p.next) {
				s.sendS2C(c, p.inner)
				p.resends++
				p.next = now.Add(introResendEvery)
			}
			keep = append(keep, p)
		}
		clear(c.pendingIntros[len(keep):])
		c.pendingIntros = keep
	}
}

// Candidate types.
const (
	candLAN   = "lan"
	candSrflx = "srflx"
	candV6    = "v6"
)

// peerRecord describes about for the receiver to, applying the candidate rules.
func (s *Server) peerRecord(about, to *Client) peerRecord {
	return peerRecord{
		PeerID: hex16(about.acct.PeerID), VIP: ipString(about.acct.VIP), PK: hex.EncodeToString(about.acct.PK[:]),
		Boot: hex.EncodeToString(about.boot[:]), XUID: hex16(about.acct.XUID), Gamertag: about.acct.Gamertag,
		Cands: candidates(about, to),
	}
}

// candidates implements the PROTOCOL.md 2.4 candidate rules for addresses of about sent to to.
func candidates(about, to *Client) [][]any {
	out := [][]any{}
	if about.privacy {
		return out
	}
	aPub, tPub := about.addr.Addr(), to.addr.Addr()
	bothLoopback := aPub.IsLoopback() && tPub.IsLoopback()
	samePublic := bothLoopback || aPub.Is4() && aPub == tPub
	add := func(a netip.AddrPort, typ string) {
		for _, e := range out {
			if e[0] == a.Addr().String() && e[1] == a.Port() {
				return
			}
		}
		out = append(out, []any{a.Addr().String(), a.Port(), typ})
	}
	var lan, v6 []netip.AddrPort
	for _, l := range about.local {
		ip := l.Addr()
		switch {
		case ip.IsLoopback():
			if bothLoopback {
				lan = append(lan, l)
			}
		case ip.Is4(), ip.IsPrivate():
			lan = append(lan, l)
		case ip.IsGlobalUnicast():
			v6 = append(v6, l)
		}
	}
	if samePublic {
		for _, l := range lan {
			add(l, candLAN)
		}
		for _, l := range v6 {
			add(l, candV6)
		}
		add(about.addr, candSrflx) // hairpin fallback; also the loopback socket itself
		return out
	}
	if !aPub.IsLoopback() {
		add(about.addr, candSrflx)
	}
	for _, l := range v6 {
		add(l, candV6)
	}
	return out
}
