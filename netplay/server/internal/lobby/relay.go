package lobby

import (
	"net/netip"

	"ac6lobby/internal/wire"
)

// handleRelay forwards RELAY as RELAYED when the PROTOCOL.md 2.6 conditions hold; otherwise
// the packet is dropped silently. The inner packet is never decrypted.
func (s *Server) handleRelay(b []byte, from netip.AddrPort) {
	src, dst, inner, err := wire.ParseRelay(b)
	if err != nil {
		s.counters.Malformed++
		return
	}
	sc := s.clients[src]
	if sc == nil || sc.addr != from {
		s.counters.RelayDropped++
		return
	}
	dc := s.clients[dst]
	if dc == nil {
		s.counters.RelayDropped++
		return
	}
	if boot, ok := sc.introduced[dst]; !ok || boot != dc.boot {
		s.counters.RelayDropped++
		return
	}
	now := s.now()
	size := float64(len(b))
	if !sc.relayPkts.has(1, now) || !sc.relayBytes.has(size, now) {
		s.counters.RelayDropped++
		return
	}
	sc.relayPkts.allow(1, now)
	sc.relayBytes.allow(size, now)
	s.counters.Relayed++
	s.counters.RelayedBytes += uint64(len(b))
	sc.relayedPkts++
	sc.relayedBytes += uint64(len(b))
	s.send(wire.BuildRelayed(src, inner), dc.addr)
}
