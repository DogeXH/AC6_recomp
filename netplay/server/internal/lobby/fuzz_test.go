package lobby

import (
	"net/netip"
	"testing"

	"ac6lobby/internal/client"
	"ac6lobby/internal/wire"
)

// recordConn collects datagrams instead of sending them.
type recordConn struct{ n int }

func (r *recordConn) WriteToUDPAddrPort(b []byte, _ netip.AddrPort) (int, error) {
	if len(b) > wire.MaxDatagram {
		panic("datagram over 1400 bytes")
	}
	r.n++
	return len(b), nil
}

// fuzzServer returns a server with two registered clients and one session, driven directly
// (no socket).
func fuzzServer(t testing.TB) (*Server, *Client, *Client) {
	clock := newFakeClock()
	srv, err := New(Config{SecretKey: client.NewKey(), Logger: testLogger(), Now: clock.Now})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(srv.Close)
	// The timer loop runs from New; every access below and in the fuzz bodies holds mu.
	srv.mu.Lock()
	defer srv.mu.Unlock()
	srv.conn = &recordConn{}
	now := clock.Now()
	mk := func(tag string, port uint16) *Client {
		pk := wire.PublicKey(client.NewKey())
		a, err := srv.accounts.create(pk, wire.Key{1}, tag, now)
		if err != nil {
			t.Fatal(err)
		}
		c := newClient(a, wire.Boot{byte(port)}, wire.Key{2}, wire.Key{3}, netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), port), now)
		c.compat = testCompat
		srv.clients[a.PeerID] = c
		return c
	}
	a, b := mk("A", 1000), mk("B", 1001)
	srv.dispatchRPC(a, 1, []byte(`{"id":1,"m":"session.create","a":{"title":"4E4D07D1","compat":"`+testCompat+
		`","flags":31,"public_slots":4,"private_slots":1,"contexts":[[32778,1]],"properties":[{"id":268435468,"t":1,"v":5}]}}`), now)
	return srv, a, b
}

var rpcSeeds = []string{
	`{"id":2,"m":"whoami","a":{}}`,
	`{"id":3,"m":"session.search","a":{"title":"4E4D07D1","compat":"` + testCompat + `","proc":1,"max_results":5,"num_users":1,"contexts":[[32778,1],[32779,0]],"properties":[{"id":268435468,"t":1,"v":5}]}}`,
	`{"id":4,"m":"session.join","a":{"session_id":"8000000000000001","members":[{"xuid":"0009000000000001","private":true}]}}`,
	`{"id":5,"m":"stats.write","a":{"session_id":"8000000000000001","title":"4E4D07D1","xuid":"0009000000000001","views":[{"view_id":86,"props":[{"id":536870920,"t":2,"v":10}]}]}}`,
	`{"id":6,"m":"stats.enumerate","a":{"title":"4E4D07D1","view_id":86,"columns":[2,3,1],"pivot":"rating","value":5,"num_rows":10}}`,
	`{"id":7,"m":"stats.read","a":{"title":"4E4D07D1","xuids":["0009000000000001"],"specs":[{"view_id":86,"columns":[65535,65533]}]}}`,
	`{"id":8,"m":"session.arbitrate","a":{"session_id":"8000000000000001","nonce":"0000000000000001"}}`,
	`{"id":9,"m":"session.attrs","a":{"session_id":"8000000000000001","contexts":[[1,2]],"properties":[{"id":1,"t":4,"v":"AEEAAA=="}]}}`,
}

func FuzzRPC(f *testing.F) {
	for _, s := range rpcSeeds {
		f.Add([]byte(s))
	}
	srv, a, b := fuzzServer(f)
	f.Fuzz(func(t *testing.T, data []byte) {
		srv.mu.Lock()
		defer srv.mu.Unlock()
		now := srv.now()
		srv.dispatchRPC(a, 2, data, now)
		srv.dispatchRPC(b, 3, data, now)
	})
}

func FuzzInner(f *testing.F) {
	f.Add([]byte{wire.KindRPCReq, 1, 0, 0, 0, 0, 1, '{', '}'})
	f.Add([]byte{wire.KindRPCReq, 1, 0, 0, 0, 0, 2, '{'})
	f.Add([]byte{wire.KindKeepalive, '{', '"', 'l', 'o', 'c', 'a', 'l', '"', ':', '[', '[', '"', '1', '.', '2', '.', '3', '.', '4', '"', ',', '5', ']', ']', '}'})
	f.Add([]byte{wire.KindIntroReq, 1, 0, 0, 0, 1, 10, 64, 0, 2})
	f.Add([]byte{wire.KindIntroReq, 1, 0, 0, 0, 2, 1, 2, 3, 4, 5, 6, 7, 8})
	f.Add([]byte{wire.KindBye})
	f.Fuzz(func(t *testing.T, inner []byte) {
		srv, a, _ := fuzzServer(t)
		srv.mu.Lock()
		defer srv.mu.Unlock()
		srv.handleInner(a, inner, srv.now())
		srv.tick()
	})
}

func FuzzPacket(f *testing.F) {
	srv, a, b := fuzzServer(f)
	f.Add(wire.BuildRelay(a.acct.PeerID, b.acct.PeerID, wire.BuildP2P(a.acct.PeerID, 1, wire.Key{}, []byte{3})))
	f.Add(wire.BuildC2S(wire.C2SFields{PeerID: a.acct.PeerID, Boot: a.boot, Ctr: 9}, a.c2sKey, []byte{wire.KindKeepalive}))
	f.Add(wire.BuildHello(wire.HelloFields{Ctr: 1}, wire.Key{}, []byte(`{"v":1}`)))
	from := netip.MustParseAddrPort("127.0.0.1:1000")
	f.Fuzz(func(t *testing.T, data []byte) {
		srv.mu.Lock()
		srv.handlePacket(data, from)
		srv.mu.Unlock()
	})
}

// The relay checks with a client registered directly (complements the UDP tests).
func TestRelayChecksDirect(t *testing.T) {
	srv, a, b := fuzzServer(t)
	srv.mu.Lock()
	defer srv.mu.Unlock()
	rc := srv.conn.(*recordConn)
	pkt := wire.BuildRelay(a.acct.PeerID, b.acct.PeerID, wire.BuildP2P(a.acct.PeerID, 1, wire.Key{}, []byte{3}))
	srv.handlePacket(pkt, a.addr)
	if rc.n != 0 {
		t.Fatal("relay between a pair that was never introduced")
	}
	a.introduced[b.acct.PeerID] = b.boot
	b.introduced[a.acct.PeerID] = a.boot
	srv.handlePacket(pkt, b.addr) // wrong source address
	if rc.n != 0 {
		t.Fatal("relay from an address that is not the source's")
	}
	srv.handlePacket(pkt, a.addr)
	if rc.n != 1 {
		t.Fatal("authorized relay not forwarded")
	}
	b.boot[0] ^= 0xFF // b restarted
	srv.handlePacket(pkt, a.addr)
	if rc.n != 1 {
		t.Fatal("relay to a peer's new boot without a new introduction")
	}
}
