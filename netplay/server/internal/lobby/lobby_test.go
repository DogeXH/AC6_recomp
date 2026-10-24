package lobby

import (
	"bytes"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"math/rand/v2"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"ac6lobby/internal/client"
	"ac6lobby/internal/wire"
)

type whoami struct {
	PeerID   string `json:"peer_id"`
	XUID     string `json:"xuid"`
	VIP      string `json:"vip"`
	Gamertag string `json:"gamertag"`
	Public   []any  `json:"public"`
}

func TestHelloAndWhoami(t *testing.T) {
	ts := startServer(t)
	c := ts.newClient("Talisman")
	ack := c.Ack()
	if ack.V != 1 || len(ack.PeerID) != 16 || !strings.HasPrefix(ack.XUID, "0009") || len(ack.XUID) != 16 {
		t.Fatalf("bad ack ids: %+v", ack)
	}
	if ack.VIP != "10.64.0.1" || ack.Gamertag != "Talisman" {
		t.Fatalf("bad ack: %+v", ack)
	}
	if len(ack.Public) != 2 || ack.Public[0] != "127.0.0.1" || int(ack.Public[1].(float64)) != int(c.LocalAddr().Port()) {
		t.Fatalf("bad public address: %v", ack.Public)
	}
	var w whoami
	call(t, c, "whoami", nil, &w)
	if w.PeerID != ack.PeerID || w.XUID != ack.XUID || w.VIP != ack.VIP || w.Gamertag != ack.Gamertag {
		t.Fatalf("whoami %+v does not match ack %+v", w, ack)
	}
	// A repeated HELLO with the same boot is idempotent.
	ack2, err := c.Hello(client.HelloInfo{Gamertag: "Talisman", Title: "4E4D07D1", Compat: testCompat}, 3*time.Second)
	if err != nil || ack2.PeerID != ack.PeerID || ack2.VIP != ack.VIP {
		t.Fatalf("repeat hello: %+v %v", ack2, err)
	}
	if st := ts.Status(); st.Online != 1 || st.Accounts != 1 {
		t.Fatalf("status %+v", st)
	}
	// Unknown method and malformed args.
	callErr(t, c, "nope", nil, "bad_request")
	callErr(t, c, "session.get", map[string]any{"session_id": "zz"}, "bad_request")
	callErr(t, c, "session.get", map[string]any{"session_id": "8000000000000001"}, "not_found")
	var sr map[string]bool
	call(t, c, "storage.read", map[string]any{"path": "/title/4E4D07D1/x"}, &sr)
	if sr["found"] {
		t.Fatal("storage.read found something")
	}
}

func TestGamertagsAndAllocation(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("Talisman")
	b := ts.newClient("talisman")
	c := ts.newClient("ABCDEFGHIJKLMNOPQ")
	d := ts.newClient("ABCDEFGHIJKLMNO")
	e := ts.newClient("  <>!! ")
	want := []struct {
		c        *client.Client
		tag, vip string
	}{{a, "Talisman", "10.64.0.1"}, {b, "talisman2", "10.64.0.2"}, {c, "ABCDEFGHIJKLMNO", "10.64.0.3"},
		{d, "ABCDEFGHIJKLMN2", "10.64.0.4"}, {e, "Pilot", "10.64.0.5"}}
	for _, w := range want {
		if got := w.c.Ack(); got.Gamertag != w.tag || got.VIP != w.vip {
			t.Errorf("got %s %s, want %s %s", got.Gamertag, got.VIP, w.tag, w.vip)
		}
	}
	// Renaming through HELLO.
	ack, err := a.Hello(client.HelloInfo{Gamertag: "Bird of Prey", Title: "4E4D07D1", Compat: testCompat}, 3*time.Second)
	if err != nil || ack.Gamertag != "Bird of Prey" {
		t.Fatalf("rename: %+v %v", ack, err)
	}
	f := ts.newClient("Talisman") // the old name is free again
	if f.Ack().Gamertag != "Talisman" {
		t.Fatalf("freed name not reused: %s", f.Ack().Gamertag)
	}

	// VIP allocation skips .255 and .0 last octets.
	st := newAccountStore()
	st.nextVIP = 0x0A4000FE
	v1, _ := st.allocVIP()
	v2, _ := st.allocVIP()
	if ipString(v1) != "10.64.0.254" || ipString(v2) != "10.64.1.1" {
		t.Fatalf("vip skip: %s %s", ipString(v1), ipString(v2))
	}
	st.nextVIP = vipLast
	if _, err := st.allocVIP(); err != nil {
		t.Fatal(err)
	}
	if _, err := st.allocVIP(); err == nil {
		t.Fatal("allocation past 10.127.255.254 succeeded")
	}
	if got := sanitizeGamertag("a  b\tc"); got != "a bc" {
		t.Fatalf("sanitize: %q", got)
	}
}

func TestRPCFragmentationAndDedupe(t *testing.T) {
	ts := startServer(t)
	c := ts.newClient("Big")
	// A request over 1100 bytes and a response over 1100 bytes.
	props := []map[string]any{}
	for i := 0; i < 20; i++ {
		props = append(props, map[string]any{"id": 0x40000000 + i, "t": TypeBinary,
			"v": base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{byte(i)}, 100))})
	}
	props = append(props, map[string]any{"id": 0x20000001, "t": TypeInt64, "v": json.Number("9007199254740993")})
	var r createResult
	call(t, c, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat, "flags": 0x2F,
		"public_slots": 16, "private_slots": 0, "contexts": [][]uint32{{0x800A, 1}}, "properties": props}, &r)
	raw, err := c.CallRaw("session.get", map[string]any{"session_id": r.SessionID}, 6*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if len(raw) < 2*wire.MaxFragment {
		t.Fatalf("response is only %d bytes", len(raw))
	}
	var d docResult
	if err := json.Unmarshal(raw, &d); err != nil || len(d.Properties) != 21 {
		t.Fatalf("doc: %v, %d props", err, len(d.Properties))
	}
	if !strings.Contains(string(d.Properties[20]), "9007199254740993") {
		t.Fatalf("int64 did not round-trip: %s", d.Properties[20])
	}

	// The same request sent twice executes once.
	msg := []byte(`{"id":77,"m":"session.create","a":{"title":"4E4D07D1","compat":"` + testCompat +
		`","flags":8,"public_slots":4,"private_slots":0,"contexts":[],"properties":[]}}`)
	inner := wire.AppendRPCFrag(nil, wire.KindRPCReq, wire.RPCFrag{ReqID: 77, Idx: 0, Cnt: 1, Data: msg})
	before := len(ts.sessionsSnapshot())
	c.SendC2S(inner)
	c.SendC2S(inner)
	waitFor(t, "two responses", func() bool { return ts.Stats().RPCs >= 3 })
	time.Sleep(100 * time.Millisecond)
	if n := len(ts.sessionsSnapshot()); n != before+1 {
		t.Fatalf("duplicate request executed: %d sessions, want %d", n, before+1)
	}

	// A request over 32 KiB is dropped without an answer.
	big := bytes.Repeat([]byte(" "), 31*wire.MaxFragment)
	for i, f := range wire.SplitRPC(90, big) {
		c.SendC2S(wire.AppendRPCFrag(nil, wire.KindRPCReq, f))
		_ = i
	}
	rpcs := ts.Stats().RPCs
	time.Sleep(100 * time.Millisecond)
	if ts.Stats().RPCs != rpcs {
		t.Fatal("oversized request was dispatched")
	}
	// A JSON id that disagrees with the header req_id is rejected.
	bad := wire.AppendRPCFrag(nil, wire.KindRPCReq, wire.RPCFrag{ReqID: 91, Cnt: 1, Data: []byte(`{"id":5,"m":"whoami","a":{}}`)})
	c.SendC2S(bad)
	waitFor(t, "id mismatch answered", func() bool { return ts.Stats().RPCs > rpcs })
}

func (ts *testServer) sessionsSnapshot() map[uint64]*Session {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	m := map[uint64]*Session{}
	for k, v := range ts.sessions {
		m[k] = v
	}
	return m
}

func TestSessionLifecycle(t *testing.T) {
	ts := startServer(t)
	host := ts.newClient("Host")
	guest := ts.newClient("Guest")
	other := ts.newClient("Other")
	hx, gx := host.Ack().XUID, guest.Ack().XUID

	r := createSession(t, host, 0x2F, 2, 1, [][]uint32{{0x800A, 1}, {0x800B, 3}})
	if r.SessionID[:2] != "80" || len(r.SessionID) != 16 || len(r.Key) != 32 || len(r.Nonce) != 16 {
		t.Fatalf("create result %+v", r)
	}
	var d docResult
	call(t, guest, "session.get", map[string]any{"session_id": r.SessionID}, &d)
	if d.State != "lobby" || d.FilledPublic != 0 || d.Host["xuid"] != hx || d.Host["vip"] != "10.64.0.1" ||
		d.Nonce != r.Nonce || d.Key != r.Key || d.PublicSlots != 2 || d.PrivateSlots != 1 {
		t.Fatalf("doc %+v", d)
	}
	sid := map[string]any{"session_id": r.SessionID}

	// Host-only calls.
	callErr(t, guest, "session.start", sid, "not_host")
	callErr(t, guest, "session.modify", map[string]any{"session_id": r.SessionID, "flags": 0, "public_slots": 1, "private_slots": 0}, "not_host")
	callErr(t, guest, "session.attrs", map[string]any{"session_id": r.SessionID, "contexts": [][]uint32{}, "properties": []any{}}, "not_host")
	callErr(t, guest, "session.delete", sid, "not_host")

	// Joins: the host joins itself, the guest joins itself, nobody else may add others.
	var fill map[string]uint32
	call(t, host, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": hx, "private": false}}}, &fill)
	if fill["filled_public"] != 1 {
		t.Fatalf("fill %v", fill)
	}
	callErr(t, guest, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": other.Ack().XUID}}}, "not_host")
	call(t, guest, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": gx, "private": true}}}, &fill)
	if fill["filled_public"] != 1 || fill["filled_private"] != 1 {
		t.Fatalf("fill %v", fill)
	}
	call(t, guest, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": gx}}}, &fill)
	if fill["filled_public"] != 1 || fill["filled_private"] != 1 {
		t.Fatalf("repeat join changed counts: %v", fill)
	}
	// One public slot left; the host admits a remote private player (falls back to public)
	// and then the session is full.
	call(t, host, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": "0009000000000099", "private": true}}}, &fill)
	if fill["filled_public"] != 2 || fill["filled_private"] != 1 {
		t.Fatalf("fill %v", fill)
	}
	callErr(t, other, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": other.Ack().XUID}}}, "full")
	// Leave rules.
	callErr(t, guest, "session.leave", map[string]any{"session_id": r.SessionID, "xuids": []string{hx}}, "not_host")
	call(t, host, "session.leave", map[string]any{"session_id": r.SessionID, "xuids": []string{"0009000000000099"}}, nil)
	call(t, other, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": other.Ack().XUID}}}, &fill)
	call(t, other, "session.leave", map[string]any{"session_id": r.SessionID, "xuids": []string{other.Ack().XUID}}, nil)

	// Attributes and modify.
	call(t, host, "session.attrs", map[string]any{"session_id": r.SessionID, "contexts": [][]uint32{{0x800A, 0}},
		"properties": []any{map[string]any{"id": 0x10000001, "t": 1, "v": -5}}}, nil)
	call(t, host, "session.modify", map[string]any{"session_id": r.SessionID, "flags": 0x42F, "public_slots": 8, "private_slots": 0}, nil)
	call(t, guest, "session.get", sid, &d)
	if d.Flags != 0x42F || d.PublicSlots != 8 || len(d.Contexts) != 1 || d.Contexts[0][1] != 0 ||
		!strings.Contains(string(d.Properties[0]), `"v":-5`) || len(d.Members) != 2 {
		t.Fatalf("after attrs/modify: %+v", d)
	}

	// State machine.
	call(t, host, "session.start", sid, nil)
	callErr(t, host, "session.start", sid, "state")
	callErr(t, other, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": other.Ack().XUID}}}, "state")
	call(t, guest, "session.get", sid, &d)
	if d.State != "ingame" {
		t.Fatalf("state %s", d.State)
	}
	call(t, host, "session.end", sid, nil)
	call(t, host, "session.end", sid, nil) // lobby already: ok
	call(t, host, "session.delete", sid, nil)
	callErr(t, guest, "session.get", sid, "not_found")

	// At most 4 hosted sessions per account; compat must match the HELLO.
	for i := 0; i < maxSessionsPerAccount; i++ {
		createSession(t, host, 8, 4, 0, nil)
	}
	callErr(t, host, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat, "flags": 8, "public_slots": 4}, "rate")
	callErr(t, guest, "session.create", map[string]any{"title": "4E4D07D1", "compat": strings.Repeat("0", 32), "flags": 8, "public_slots": 4}, "compat")
	callErr(t, guest, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat, "public_slots": 40, "private_slots": 40}, "bad_request")
	callErr(t, guest, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat,
		"properties": []any{map[string]any{"id": 1, "t": 9, "v": 0}}}, "bad_request")
	callErr(t, guest, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat,
		"properties": []any{map[string]any{"id": 1, "t": 1, "v": 1e30}}}, "bad_request")
}

func TestSearchFilters(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("A")
	b := ts.newClient("B")
	searcher := ts.newClient("S")
	ranked := [][]uint32{{0x800A, 0}, {0x800B, 1}}

	s1 := createSession(t, a, 0x2F, 4, 0, ranked)                         // match
	s2 := createSession(t, b, 0x2F, 4, 0, ranked)                         // match, newer
	createSession(t, a, 0x27, 4, 0, ranked)                               // no matchmaking flag
	createSession(t, a, 0x2F, 4, 0, [][]uint32{{0x800A, 1}, {0x800B, 1}}) // context mismatch
	full := createSession(t, b, 0x2F, 1, 0, ranked)
	call(t, b, "session.join", map[string]any{"session_id": full.SessionID, "members": []any{map[string]any{"xuid": b.Ack().XUID}}}, nil)
	jip := createSession(t, b, 0x42F, 4, 0, ranked)
	call(t, b, "session.start", map[string]any{"session_id": jip.SessionID}, nil)
	open := createSession(t, b, 0x2F, 4, 0, ranked)
	call(t, b, "session.start", map[string]any{"session_id": open.SessionID}, nil) // in game, joins allowed

	search := func(c *client.Client, max int, ctxs [][]uint32) []docResult {
		var r struct {
			Results []docResult `json:"results"`
		}
		call(t, c, "session.search", map[string]any{"title": "4E4D07D1", "compat": testCompat, "proc": 0,
			"max_results": max, "num_users": 1, "contexts": ctxs, "properties": []any{}}, &r)
		return r.Results
	}
	res := search(searcher, 10, ranked)
	ids := []string{}
	for _, d := range res {
		ids = append(ids, d.SessionID)
	}
	want := []string{open.SessionID, s2.SessionID, s1.SessionID}
	if strings.Join(ids, ",") != strings.Join(want, ",") {
		t.Fatalf("search got %v want %v", ids, want)
	}
	if got := search(searcher, 1, ranked); len(got) != 1 || got[0].SessionID != open.SessionID {
		t.Fatalf("max_results 1: %v", got)
	}
	// The host's own sessions are excluded.
	for _, d := range search(a, 10, ranked) {
		if d.Host["xuid"] == a.Ack().XUID {
			t.Fatal("own session returned")
		}
	}
	// A different compat key finds nothing.
	var r struct {
		Results []docResult `json:"results"`
	}
	call(t, searcher, "session.search", map[string]any{"title": "4E4D07D1", "compat": strings.Repeat("1", 32),
		"max_results": 10, "num_users": 1, "contexts": ranked}, &r)
	if len(r.Results) != 0 {
		t.Fatalf("compat mismatch matched %d sessions", len(r.Results))
	}
	// num_users larger than the open slots.
	call(t, searcher, "session.search", map[string]any{"title": "4E4D07D1", "compat": testCompat,
		"max_results": 10, "num_users": 5, "contexts": ranked}, &r)
	if len(r.Results) != 0 {
		t.Fatalf("num_users 5 matched %d", len(r.Results))
	}
	// A host that went offline: its sessions disappear from results.
	b.Bye()
	waitFor(t, "b offline", func() bool { return ts.Status().Online == 2 })
	if got := search(searcher, 10, ranked); len(got) != 1 || got[0].SessionID != s1.SessionID {
		t.Fatalf("after b left: %v", got)
	}
}

func TestSearchTruncatesToFit(t *testing.T) {
	ts := startServer(t)
	searcher := ts.newClient("S")
	big := []any{}
	for i := 0; i < 6; i++ {
		big = append(big, map[string]any{"id": 0x60000000 + i, "t": TypeBinary,
			"v": base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{1}, 1000))})
	}
	for i := 0; i < 12; i++ {
		h := ts.newClient("H")
		call(t, h, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat, "flags": 8,
			"public_slots": 4, "properties": big}, nil)
	}
	// proc 7 has no XLast query, so results keep their (large) properties.
	raw, err := searcher.CallRaw("session.search", map[string]any{"title": "4E4D07D1", "compat": testCompat,
		"proc": 7, "max_results": 50, "num_users": 1}, 8*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	var r struct {
		Results []json.RawMessage `json:"results"`
	}
	json.Unmarshal(raw, &r)
	if len(r.Results) == 0 || len(r.Results) >= 12 || len(raw) > maxResponseBytes {
		t.Fatalf("got %d results in %d bytes", len(r.Results), len(raw))
	}
}

func TestArbitrationGrouping(t *testing.T) {
	ts := startServer(t)
	host := ts.newClient("Host")
	g1 := ts.newClient("G1")
	g2 := ts.newClient("G2")
	r := createSession(t, host, 0x3F, 8, 0, nil)
	join := func(c *client.Client, xuids ...string) {
		ms := []any{}
		for _, x := range xuids {
			ms = append(ms, map[string]any{"xuid": x})
		}
		call(t, c, "session.join", map[string]any{"session_id": r.SessionID, "members": ms}, nil)
	}
	join(host, host.Ack().XUID)
	join(g2, g2.Ack().XUID)
	join(host, "0009000000000abc") // no account: attributed to the host machine
	join(g1, g1.Ack().XUID)

	outsider := ts.newClient("Out")
	callErr(t, outsider, "session.arbitrate", map[string]any{"session_id": r.SessionID, "nonce": r.Nonce}, "not_host")

	var res struct {
		Registrants []struct {
			MachineID string   `json:"machine_id"`
			Trust     int      `json:"trust"`
			XUIDs     []string `json:"xuids"`
		} `json:"registrants"`
	}
	call(t, g1, "session.arbitrate", map[string]any{"session_id": r.SessionID, "nonce": r.Nonce}, &res)
	if len(res.Registrants) != 3 {
		t.Fatalf("registrants %+v", res.Registrants)
	}
	want := []struct {
		vip   string
		xuids []string
	}{{host.Ack().VIP, []string{host.Ack().XUID, "0009000000000abc"}}, {g2.Ack().VIP, []string{g2.Ack().XUID}},
		{g1.Ack().VIP, []string{g1.Ack().XUID}}}
	for i, w := range want {
		reg := res.Registrants[i]
		v, _ := parseVIP(w.vip)
		if wantID := hex16(0xFA00000000000000 | 0x002248<<24 | uint64(v&0xFFFFFF)); reg.MachineID != wantID {
			t.Errorf("registrant %d machine %s want %s", i, reg.MachineID, wantID)
		}
		if reg.Trust != 1 || strings.Join(reg.XUIDs, ",") != strings.Join(w.xuids, ",") {
			t.Errorf("registrant %d: %+v", i, reg)
		}
	}
	if res.Registrants[0].MachineID != "fa00002248400001" { // MAC 00:22:48:40:00:01 for 10.64.0.1
		t.Errorf("machine id format %s", res.Registrants[0].MachineID)
	}
}

func TestStats(t *testing.T) {
	dir := t.TempDir()
	key := client.NewKey()
	ts := startServer(t, withData(dir), withKey(key))
	host := ts.newClient("Host")
	g := ts.newClient("Guest")
	r := createSession(t, host, 0x1F, 4, 0, nil)
	call(t, g, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": g.Ack().XUID}}}, nil)
	write := func(c *client.Client, xuid string, score, kills int64) {
		call(t, c, "stats.write", map[string]any{"session_id": r.SessionID, "title": "4E4D07D1", "xuid": xuid,
			"views": []any{map[string]any{"view_id": 3, "props": []any{
				map[string]any{"id": 0x20000001, "t": TypeInt64, "v": score},
				map[string]any{"id": 0x10000002, "t": TypeInt32, "v": kills}}}}}, nil)
	}
	write(host, host.Ack().XUID, 500, 7)
	write(g, g.Ack().XUID, 900, 2)
	callErr(t, g, "stats.write", map[string]any{"session_id": r.SessionID, "title": "4E4D07D1", "xuid": host.Ack().XUID,
		"views": []any{}}, "not_host")
	// The host may write for anyone; a later write replaces the column.
	write(host, g.Ack().XUID, 1000, 3)

	type statsView struct {
		ViewID    uint32 `json:"view_id"`
		TotalRows int    `json:"total_rows"`
		Rows      []struct {
			XUID     string            `json:"xuid"`
			Gamertag string            `json:"gamertag"`
			Rank     int               `json:"rank"`
			Rating   int64             `json:"rating"`
			Columns  []json.RawMessage `json:"columns"`
		} `json:"rows"`
	}
	var rd struct {
		Views []statsView `json:"views"`
	}
	call(t, host, "stats.read", map[string]any{"title": "4E4D07D1", "xuids": []string{host.Ack().XUID, g.Ack().XUID, "0009000000000001"},
		"specs": []any{map[string]any{"view_id": 3, "columns": []int{1, 2, 9}}, map[string]any{"view_id": 99, "columns": []int{1}}}}, &rd)
	if len(rd.Views) != 2 || rd.Views[0].TotalRows != 2 || len(rd.Views[0].Rows) != 2 || len(rd.Views[1].Rows) != 0 {
		t.Fatalf("read: %+v", rd)
	}
	hr, gr := rd.Views[0].Rows[0], rd.Views[0].Rows[1]
	if hr.Rank != 2 || hr.Rating != 500 || gr.Rank != 1 || gr.Rating != 1000 || gr.Gamertag != "Guest" {
		t.Fatalf("ranks: %+v %+v", hr, gr)
	}
	if string(gr.Columns[0]) != `{"id":1,"t":2,"v":1000}` || string(gr.Columns[1]) != `{"id":2,"t":1,"v":3}` ||
		string(gr.Columns[2]) != `{"id":9,"t":255,"v":0}` {
		t.Fatalf("columns: %s", gr.Columns)
	}

	// Enumerate by rank, xuid and rating.
	for i := 0; i < 5; i++ {
		c := ts.newClient("P")
		createSessionAndWrite(t, c, int64(100*i))
	}
	enum := func(pivot string, value any, n int) statsView {
		var e struct {
			Views []statsView `json:"views"`
		}
		call(t, g, "stats.enumerate", map[string]any{"title": "4E4D07D1", "view_id": 3, "columns": []int{1},
			"pivot": pivot, "value": value, "num_rows": n}, &e)
		if len(e.Views) != 1 {
			t.Fatalf("enumerate views %d", len(e.Views))
		}
		return e.Views[0]
	}
	v := enum("rank", 1, 3)
	if v.TotalRows != 7 || len(v.Rows) != 3 || v.Rows[0].Rating != 1000 || v.Rows[1].Rating != 500 || v.Rows[2].Rating != 400 {
		t.Fatalf("by rank: %+v", v)
	}
	v = enum("rank", 6, 5)
	if len(v.Rows) != 2 || v.Rows[0].Rank != 6 || v.Rows[1].Rank != 7 {
		t.Fatalf("by rank 6: %+v", v)
	}
	v = enum("xuid", host.Ack().XUID, 3)
	if len(v.Rows) != 3 || v.Rows[1].XUID != host.Ack().XUID || v.Rows[1].Rank != 2 {
		t.Fatalf("by xuid: %+v", v)
	}
	if v = enum("xuid", "0009000000000001", 3); len(v.Rows) != 0 || v.TotalRows != 7 {
		t.Fatalf("by unknown xuid: %+v", v)
	}
	v = enum("rating", 250, 2)
	if len(v.Rows) != 2 || v.Rows[0].Rating != 300 || v.Rows[1].Rating != 200 {
		t.Fatalf("by rating: %+v", v)
	}
	callErr(t, g, "stats.enumerate", map[string]any{"title": "4E4D07D1", "view_id": 3, "pivot": "bogus", "value": 1, "num_rows": 1}, "bad_request")

	// Persistence: stop, restart on the same data, read again.
	ts.stop()
	waitForFile(t, filepath.Join(dir, "stats.json"))
	info, err := os.Stat(filepath.Join(dir, "stats.json"))
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("stats.json mode: %v %v", info.Mode(), err)
	}
	ts2 := startServer(t, withData(dir), withKey(key))
	c2 := ts2.newClient("Reader")
	call(t, c2, "stats.read", map[string]any{"title": "4E4D07D1", "xuids": []string{g.Ack().XUID},
		"specs": []any{map[string]any{"view_id": 3, "columns": []int{1}}}}, &rd)
	if len(rd.Views[0].Rows) != 1 || rd.Views[0].Rows[0].Rating != 1000 || rd.Views[0].TotalRows != 7 {
		t.Fatalf("after reload: %+v", rd)
	}
}

func createSessionAndWrite(t *testing.T, c *client.Client, score int64) {
	t.Helper()
	r := createSession(t, c, 0x1F, 4, 0, nil)
	call(t, c, "stats.write", map[string]any{"session_id": r.SessionID, "title": "4E4D07D1", "xuid": c.Ack().XUID,
		"views": []any{map[string]any{"view_id": 3, "props": []any{map[string]any{"id": 0x20000001, "t": TypeInt64, "v": score}}}}}, nil)
}

func waitForFile(t *testing.T, path string) {
	t.Helper()
	waitFor(t, path, func() bool { _, err := os.Stat(path); return err == nil })
}

// bskOf returns the secret key a test client was created with through ts.keys.
func bskOf(t *testing.T, ts *testServer, c *client.Client) wire.Key {
	k, ok := ts.keys[c]
	if !ok {
		t.Fatal("no recorded key")
	}
	return k
}

func vipValue(t *testing.T, s string) uint64 {
	v, ok := parseVIP(s)
	if !ok {
		t.Fatalf("bad vip %s", s)
	}
	return uint64(v)
}

func TestIntroAndRelay(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("A")
	b := ts.newClient("B")
	if err := a.IntroReq(42, wire.IntroByVIP, vipValue(t, b.Ack().VIP)); err != nil {
		t.Fatal(err)
	}
	ia, err := a.WaitIntro(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	ib, err := b.WaitIntro(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if ia.ReqID != 42 || ia.Peer.PeerID != b.Ack().PeerID || ia.Peer.VIP != b.Ack().VIP || ia.Peer.Gamertag != "B" {
		t.Fatalf("intro to A: %+v", ia)
	}
	if ib.ReqID != 0 || ib.Peer.PeerID != a.Ack().PeerID || ib.Peer.XUID != a.Ack().XUID {
		t.Fatalf("intro to B: %+v", ib)
	}
	bpk := b.PublicKey()
	bboot := b.Boot()
	if ia.Peer.PK != hexString(bpk[:]) || ia.Peer.Boot != hexString(bboot[:]) {
		t.Fatal("intro pk/boot mismatch")
	}
	// Both on loopback: lan candidates including loopback; srflx equals the loopback
	// candidate here, so it is deduplicated.
	types := []string{}
	for _, c := range ia.Peer.Cands {
		types = append(types, c[2].(string))
	}
	if strings.Join(types, ",") != "lan,lan" || ia.Peer.Cands[1][0] != "127.0.0.1" {
		t.Fatalf("cands %v", ia.Peer.Cands)
	}

	// Relay both ways.
	bid, _ := a.AddPeer(ia.Peer)
	aid, _ := b.AddPeer(ib.Peer)
	payload := append([]byte{wire.P2PData, 0x01}, []byte("hello over relay")...)
	if err := a.SendRelay(bid, payload); err != nil {
		t.Fatal(err)
	}
	got, err := b.WaitRelayed(2 * time.Second)
	if err != nil || got.Src != aid || !bytes.Equal(got.Inner, payload) {
		t.Fatalf("relayed to B: %+v %v", got, err)
	}
	if err := b.SendRelay(aid, []byte{wire.P2PKeepalive}); err != nil {
		t.Fatal(err)
	}
	if got, err := a.WaitRelayed(2 * time.Second); err != nil || got.Inner[0] != wire.P2PKeepalive {
		t.Fatalf("relayed to A: %+v %v", got, err)
	}

	// Introduction by peer id works too.
	if err := b.IntroReq(43, wire.IntroByPeerID, aid); err != nil {
		t.Fatal(err)
	}
	if i, err := b.WaitIntro(2 * time.Second); err != nil || i.ReqID != 43 {
		t.Fatalf("intro by peer id: %+v %v", i, err)
	}
}

func hexString(b []byte) string { return hex.EncodeToString(b) }

func TestIntroFailAndPrivacy(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("A")
	b := ts.newClient("B")

	a.IntroReq(1, wire.IntroByVIP, vipValue(t, "10.64.9.9"))
	if f, err := a.WaitIntroFail(2 * time.Second); err != nil || f.ReqID != 1 || f.Reason != wire.IntroFailUnknown {
		t.Fatalf("unknown: %+v %v", f, err)
	}
	a.IntroReq(2, wire.IntroByVIP, vipValue(t, a.Ack().VIP))
	if f, err := a.WaitIntroFail(2 * time.Second); err != nil || f.Reason != wire.IntroFailRefused {
		t.Fatalf("self: %+v %v", f, err)
	}
	bid, _ := parseHex16(b.Ack().PeerID)
	b.Bye()
	waitFor(t, "b offline", func() bool { return ts.Status().Online == 1 })
	a.IntroReq(3, wire.IntroByPeerID, bid)
	if f, err := a.WaitIntroFail(2 * time.Second); err != nil || f.ReqID != 3 || f.Reason != wire.IntroFailOffline {
		t.Fatalf("offline: %+v %v", f, err)
	}

	// Privacy: records about a privacy client carry no candidates.
	p, err := client.New(ts.addr, ts.PublicKey(), client.NewKey())
	if err != nil {
		t.Fatal(err)
	}
	defer p.Close()
	if _, err := p.Hello(client.HelloInfo{Gamertag: "Ghost", Title: "4E4D07D1", Compat: testCompat, Privacy: true,
		Local: [][]any{{"192.168.1.9", 5000}}}, 3*time.Second); err != nil {
		t.Fatal(err)
	}
	a.IntroReq(4, wire.IntroByVIP, vipValue(t, p.Ack().VIP))
	ia, err := a.WaitIntro(2 * time.Second)
	if err != nil || len(ia.Peer.Cands) != 0 {
		t.Fatalf("privacy record: %+v %v", ia, err)
	}
	ip, err := p.WaitIntro(2 * time.Second)
	if err != nil || len(ip.Peer.Cands) == 0 {
		t.Fatalf("record sent to the privacy client should still list A's candidates: %+v %v", ip, err)
	}
}

func TestCandidateRules(t *testing.T) {
	mk := func(pub string, privacy bool, locals ...string) *Client {
		c := &Client{addr: netip.MustParseAddrPort(pub), privacy: privacy}
		for _, l := range locals {
			c.local = append(c.local, netip.MustParseAddrPort(l))
		}
		return c
	}
	str := func(cs [][]any) string {
		parts := []string{}
		for _, c := range cs {
			parts = append(parts, c[2].(string)+":"+netip.AddrPortFrom(netip.MustParseAddr(c[0].(string)), c[1].(uint16)).String())
		}
		return strings.Join(parts, " ")
	}
	a := mk("203.0.113.9:40000", false, "192.168.1.20:5000", "[2001:db8::5]:5000", "127.0.0.1:5000", "[fd00::1]:5000")
	sameNAT := mk("203.0.113.9:40001", false)
	other := mk("198.51.100.7:1234", false)
	if got := str(candidates(a, sameNAT)); got != "lan:192.168.1.20:5000 lan:[fd00::1]:5000 v6:[2001:db8::5]:5000 srflx:203.0.113.9:40000" {
		t.Errorf("same public: %s", got)
	}
	if got := str(candidates(a, other)); got != "srflx:203.0.113.9:40000 v6:[2001:db8::5]:5000" {
		t.Errorf("different public: %s", got)
	}
	a.privacy = true
	if got := candidates(a, other); len(got) != 0 {
		t.Errorf("privacy: %v", got)
	}
	// Parsing drops junk.
	raw := []json.RawMessage{json.RawMessage(`["10.0.0.1",1]`), json.RawMessage(`["fe80::1",2]`),
		json.RawMessage(`["0.0.0.0",3]`), json.RawMessage(`["224.0.0.1",4]`), json.RawMessage(`["x",5]`),
		json.RawMessage(`["10.0.0.2",0]`), json.RawMessage(`["10.0.0.3",70000]`), json.RawMessage(`[1,2]`), json.RawMessage(`{}`)}
	if got := parseCandidates(raw); len(got) != 1 || got[0].String() != "10.0.0.1:1" {
		t.Errorf("parseCandidates: %v", got)
	}
}

func TestRelayAuthorization(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("A")
	bsk := client.NewKey()
	b := ts.newClientKey("B", bsk)
	ts.keys[b] = bsk
	c := ts.newClient("C")
	// A and B are introduced; C is not introduced to anyone.
	a.IntroReq(1, wire.IntroByVIP, vipValue(t, b.Ack().VIP))
	ia, _ := a.WaitIntro(2 * time.Second)
	ib, _ := b.WaitIntro(2 * time.Second)
	bid, _ := a.AddPeer(ia.Peer)
	aid, _ := b.AddPeer(ib.Peer)
	cid, _ := parseHex16(c.Ack().PeerID)

	// C -> A was never introduced: dropped. C forges A's record to get keys for the attempt.
	c.AddPeer(client.PeerRecord{PeerID: a.Ack().PeerID, PK: hexString(func() []byte { k := a.PublicKey(); return k[:] }()),
		Boot: hexString(func() []byte { k := a.Boot(); return k[:] }())})
	c.SendRelay(aid, []byte{wire.P2PData, 1, 'x'})
	// A packet claiming to come from A but sent from C's socket: dropped.
	spoof, _ := a.BuildRelay(bid, []byte{wire.P2PData, 1, 'y'})
	c.SendRaw(spoof)
	// Unknown destination and malformed inner packets: dropped.
	junk := wire.BuildRelay(cid, 0x1234, make([]byte, 40))
	c.SendRaw(junk)
	waitFor(t, "drops counted", func() bool { return ts.Stats().RelayDropped >= 2 })
	if _, err := a.WaitRelayed(200 * time.Millisecond); err == nil {
		t.Fatal("A received an unauthorized relay")
	}
	if _, err := b.WaitRelayed(50 * time.Millisecond); err == nil {
		t.Fatal("B received a spoofed relay")
	}
	// The authorized pair still works.
	a.SendRelay(bid, []byte{wire.P2PData, 1, 'z'})
	if r, err := b.WaitRelayed(2 * time.Second); err != nil || r.Inner[2] != 'z' {
		t.Fatalf("authorized relay: %+v %v", r, err)
	}
	// After B restarts (same key, new boot), the old introduction no longer authorizes
	// relaying: P2P keys bind both boots, so A needs a new INTRO.
	b2 := ts.newClientKey("B", bskOf(t, ts, b))
	dropped := ts.Stats().RelayDropped
	a.SendRelay(bid, []byte{wire.P2PData, 1, 'w'})
	waitFor(t, "relay to restarted peer dropped", func() bool { return ts.Stats().RelayDropped > dropped })
	a.IntroReq(2, wire.IntroByVIP, vipValue(t, b2.Ack().VIP))
	ia2, err := a.WaitIntro(2 * time.Second)
	if err != nil || ia2.Peer.Boot == ia.Peer.Boot {
		t.Fatalf("re-intro: %+v %v", ia2, err)
	}
	ib2, _ := b2.WaitIntro(2 * time.Second)
	a.AddPeer(ia2.Peer)
	b2.AddPeer(ib2.Peer)
	a.SendRelay(bid, []byte{wire.P2PData, 1, 'v'})
	if r, err := b2.WaitRelayed(2 * time.Second); err != nil || r.Inner[2] != 'v' {
		t.Fatalf("relay after re-intro: %+v %v", r, err)
	}
}

func TestRelayRateLimit(t *testing.T) {
	clock := newFakeClock()
	ts := startServer(t, withClock(clock))
	a := ts.newClient("A")
	b := ts.newClient("B")
	a.IntroReq(1, wire.IntroByVIP, vipValue(t, b.Ack().VIP))
	ia, _ := a.WaitIntro(2 * time.Second)
	b.WaitIntro(2 * time.Second)
	bid, _ := a.AddPeer(ia.Peer)
	// The clock is frozen, so the buckets never refill: 2000 packets pass, the rest drop.
	small := []byte{wire.P2PKeepalive}
	for i := 0; i < 2100; i++ {
		a.SendRelay(bid, small)
		if i%200 == 199 {
			time.Sleep(2 * time.Millisecond) // keep the loopback socket buffer from overflowing
		}
	}
	waitFor(t, "2100 relay packets handled", func() bool { s := ts.Stats(); return s.Relayed+s.RelayDropped >= 2100 })
	if s := ts.Stats(); s.Relayed != 2000 || s.RelayDropped != 100 {
		t.Fatalf("packet limit: relayed %d dropped %d", s.Relayed, s.RelayDropped)
	}
	// Byte limit: after a second, 4 Mbit/s = 500000 bytes of full-size packets.
	clock.Advance(1100 * time.Millisecond)
	before := ts.Stats()
	large := make([]byte, wire.MaxDatagram-wire.RelayHeader-wire.P2PHeader-wire.TagSize)
	large[0] = wire.P2PData
	for i := 0; i < 400; i++ {
		a.SendRelay(bid, large)
		if i%50 == 49 {
			time.Sleep(2 * time.Millisecond)
		}
	}
	waitFor(t, "400 large packets handled", func() bool {
		s := ts.Stats()
		return s.Relayed+s.RelayDropped >= before.Relayed+before.RelayDropped+400
	})
	passed := ts.Stats().Relayed - before.Relayed
	if want := uint64(500000 / wire.MaxDatagram); passed != want {
		t.Fatalf("byte limit: %d large packets passed, want %d", passed, want)
	}
}

func TestIntroResend(t *testing.T) {
	clock := newFakeClock()
	ts := startServer(t, withClock(clock))
	a := ts.newClient("A")
	b := ts.newClient("B")
	a.IntroReq(1, wire.IntroByVIP, vipValue(t, b.Ack().VIP))
	if _, err := a.WaitIntro(2 * time.Second); err != nil {
		t.Fatal(err)
	}
	if _, err := b.WaitIntro(2 * time.Second); err != nil {
		t.Fatal(err)
	}
	// A confirms at once (any later packet); B stays silent.
	if _, err := a.Keepalive(nil, 2*time.Second); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		clock.Advance(600 * time.Millisecond)
		ts.sweepNow()
	}
	time.Sleep(100 * time.Millisecond)
	if n := b.IntroCount(); n != introMaxResends {
		t.Fatalf("B got %d resends, want %d", n, introMaxResends)
	}
	if n := a.IntroCount(); n != 0 {
		t.Fatalf("A got %d resends after confirming", n)
	}
}

func TestLiveness(t *testing.T) {
	clock := newFakeClock()
	ts := startServer(t, withClock(clock))
	host := ts.newClient("Host")
	member := ts.newClient("Member")
	other := ts.newClient("Other")
	r1 := createSession(t, host, 8, 4, 0, nil)
	r2 := createSession(t, other, 8, 4, 0, nil)
	call(t, member, "session.join", map[string]any{"session_id": r2.SessionID, "members": []any{map[string]any{"xuid": member.Ack().XUID}}}, nil)
	call(t, member, "session.join", map[string]any{"session_id": r1.SessionID, "members": []any{map[string]any{"xuid": member.Ack().XUID}}}, nil)

	clock.Advance(20 * time.Second)
	if _, err := other.Keepalive(nil, 2*time.Second); err != nil {
		t.Fatal(err)
	}
	clock.Advance(11 * time.Second) // host and member: 31 s silent; other: 11 s
	ts.sweepNow()
	if st := ts.Status(); st.Online != 1 || st.Sessions != 1 {
		t.Fatalf("after timeout: %+v", st)
	}
	var d docResult
	call(t, other, "session.get", map[string]any{"session_id": r2.SessionID}, &d)
	if len(d.Members) != 0 {
		t.Fatalf("offline member still listed: %+v", d.Members)
	}
	// The timed-out client gets REHELLO and can register again.
	member.AutoRehello = true
	if err := member.Call("whoami", nil, nil); err != nil {
		t.Fatalf("after rehello: %v", err)
	}
	if member.Rehellos() == 0 {
		t.Fatal("no REHELLO seen")
	}
	// Session idle timeout.
	clock.Advance(sessionIdleTimeout + time.Second)
	other.Keepalive(nil, 2*time.Second)
	member.Keepalive(nil, 2*time.Second)
	ts.sweepNow()
	if st := ts.Status(); st.Sessions != 0 || st.Online != 2 {
		t.Fatalf("after idle timeout: %+v", st)
	}
}

func TestClientRestartReplacesBoot(t *testing.T) {
	clock := newFakeClock()
	ts := startServer(t, withClock(clock))
	sk := client.NewKey()
	old := ts.newClientKey("Talisman", sk)
	r := createSession(t, old, 8, 4, 0, nil)
	clock.Advance(5 * time.Second)
	fresh := ts.newClientKey("Talisman", sk)
	if fresh.Ack().PeerID != old.Ack().PeerID || fresh.Ack().VIP != old.Ack().VIP || fresh.Ack().Gamertag != "Talisman" {
		t.Fatalf("restart changed identity: %+v vs %+v", fresh.Ack(), old.Ack())
	}
	// The old boot is no longer registered.
	if err := old.SendC2S([]byte{wire.KindKeepalive}); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "rehello to old boot", func() bool { return old.Rehellos() > 0 })
	// Its session lives until 30 s after the old boot's last packet, then goes.
	var d docResult
	call(t, fresh, "session.get", map[string]any{"session_id": r.SessionID}, &d)
	clock.Advance(26 * time.Second)
	fresh.Keepalive(nil, 2*time.Second)
	ts.sweepNow()
	callErr(t, fresh, "session.get", map[string]any{"session_id": r.SessionID}, "not_found")
	if st := ts.Status(); st.Online != 1 {
		t.Fatalf("status %+v", st)
	}
}

func TestStaleHelloReplay(t *testing.T) {
	ts := startServer(t)
	sk := client.NewKey()
	old := ts.newClientKey("A", sk)
	// An attacker records a HELLO of the old boot (here: one built with its keys).
	oldHello := buildHelloFor(t, ts, sk, old.Boot(), 1000)
	if err := old.SendRaw(oldHello); err != nil {
		t.Fatal(err)
	}
	if _, err := old.WaitAck(2 * time.Second); err != nil {
		t.Fatal(err)
	}
	fresh := ts.newClientKey("A", sk)
	// Replaying the old boot's HELLO must not take the registration back.
	fresh.SendRaw(oldHello)
	time.Sleep(100 * time.Millisecond)
	if err := fresh.Call("whoami", nil, nil); err != nil {
		t.Fatalf("fresh boot lost its registration: %v", err)
	}
	if fresh.Rehellos() != 0 {
		t.Fatal("fresh boot was told to REHELLO")
	}
}

// buildHelloFor builds a HELLO for (sk, boot) with the given counter.
func buildHelloFor(t *testing.T, ts *testServer, sk wire.Key, boot wire.Boot, ctr uint64) []byte {
	t.Helper()
	pk := wire.PublicKey(sk)
	dh, err := wire.DH(sk, ts.PublicKey())
	if err != nil {
		t.Fatal(err)
	}
	js := []byte(`{"v":1,"client":"t","gamertag":"A","title":"4E4D07D1","compat":"` + testCompat + `","local":[],"privacy":false}`)
	return wire.BuildHello(wire.HelloFields{ClientPK: pk, Boot: boot, Ctr: ctr}, wire.C2SKey(dh, pk, ts.PublicKey(), boot), js)
}

func TestRehelloAfterServerRestart(t *testing.T) {
	dir := t.TempDir()
	key := client.NewKey()
	ts := startServer(t, withData(dir), withKey(key))
	c := ts.newClient("Survivor")
	before := c.Ack()
	b := ts.newClient("Second")
	ts.stop()
	waitForFile(t, filepath.Join(dir, "accounts.json"))
	info, _ := os.Stat(filepath.Join(dir, "accounts.json"))
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("accounts.json mode %v", info.Mode())
	}

	// Same port, same data, same key, new boot.
	portMu.Lock()
	nextPort = mustPort(ts.addr)
	portMu.Unlock()
	ts2 := startServer(t, withData(dir), withKey(key))
	if ts2.addr != ts.addr {
		t.Fatalf("restart on %s, want %s", ts2.addr, ts.addr)
	}
	if st := ts2.Status(); st.Accounts != 2 || st.Online != 0 {
		t.Fatalf("reloaded status %+v", st)
	}
	c.AutoRehello = true
	var w whoami
	if err := c.Call("whoami", nil, &w); err != nil {
		t.Fatalf("whoami after restart: %v", err)
	}
	if c.Rehellos() == 0 {
		t.Fatal("no REHELLO after the server restart")
	}
	if w.PeerID != before.PeerID || w.XUID != before.XUID || w.VIP != before.VIP || w.Gamertag != before.Gamertag {
		t.Fatalf("identity changed across restart: %+v vs %+v", w, before)
	}
	// New accounts continue the VIP sequence.
	n := ts2.newClient("Third")
	if n.Ack().VIP != "10.64.0.3" {
		t.Fatalf("next vip %s", n.Ack().VIP)
	}
	_ = b
}

func mustPort(addr string) int {
	ap := netip.MustParseAddrPort(addr)
	return int(ap.Port())
}

func TestAccountRateLimit(t *testing.T) {
	ts := startServer(t, withLimits(Limits{HelloPerSecond: 1000, HelloBurst: 1000}))
	for i := 0; i < 20; i++ {
		ts.newClient("P")
	}
	c, err := client.New(ts.addr, ts.PublicKey(), client.NewKey())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if _, err := c.Hello(client.HelloInfo{Gamertag: "TooMany", Title: "4E4D07D1", Compat: testCompat}, 700*time.Millisecond); err == nil {
		t.Fatal("21st account from one IP was created")
	}
	if st := ts.Status(); st.Accounts != 20 {
		t.Fatalf("accounts %d", st.Accounts)
	}
}

func TestHelloRateLimit(t *testing.T) {
	ts := startServer(t, withLimits(Limits{HelloPerSecond: 0.001, HelloBurst: 3, AccountsPerIPPerHour: 100}))
	for i := 0; i < 3; i++ {
		ts.newClient("P")
	}
	c, err := client.New(ts.addr, ts.PublicKey(), client.NewKey())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if _, err := c.Hello(client.HelloInfo{Gamertag: "X", Title: "4E4D07D1", Compat: testCompat}, 500*time.Millisecond); err == nil {
		t.Fatal("hello over the per-IP rate was answered")
	}
}

func TestReplayAndMalformed(t *testing.T) {
	ts := startServer(t)
	c := ts.newClient("A")
	// A replayed C2S packet is processed once.
	pkt := c.BuildC2S([]byte{wire.KindKeepalive})
	c.SendRaw(pkt)
	if _, err := c.WaitKeepaliveAck(2 * time.Second); err != nil {
		t.Fatal(err)
	}
	c.SendRaw(pkt)
	c.SendRaw(pkt)
	if _, err := c.WaitKeepaliveAck(200 * time.Millisecond); err == nil {
		t.Fatal("replayed packet answered")
	}
	r := rand.New(rand.NewPCG(9, 9))
	for i := 0; i < 3000; i++ {
		n := r.IntN(1500)
		b := make([]byte, n)
		for j := range b {
			b[j] = byte(r.IntN(256))
		}
		if n > 1 && r.IntN(2) == 0 {
			b[0] = wire.Magic
			b[1] = []byte{1, 3, 6, 0x10, 0x55}[r.IntN(5)]
		}
		c.SendRaw(b)
	}
	// Truncated and bit-flipped real packets.
	good := c.BuildC2S(append([]byte{wire.KindRPCReq}, make([]byte, 6)...))
	for n := 0; n < len(good); n++ {
		c.SendRaw(good[:n])
	}
	flipped := append([]byte{}, good...)
	flipped[len(flipped)-1] ^= 1
	c.SendRaw(flipped)
	time.Sleep(200 * time.Millisecond)
	var w whoami
	call(t, c, "whoami", nil, &w)
	if ts.Stats().Malformed == 0 {
		t.Fatal("no malformed packets counted")
	}
}

func TestKeepaliveUpdatesCandidates(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("A")
	b := ts.newClient("B")
	body, err := a.Keepalive([]byte(`{"local":[["10.1.2.3",4000]]}`), 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	var ka struct {
		Public []any `json:"public"`
	}
	if json.Unmarshal(body, &ka) != nil || len(ka.Public) != 2 || ka.Public[0] != "127.0.0.1" {
		t.Fatalf("keepalive ack %s", body)
	}
	b.IntroReq(5, wire.IntroByVIP, vipValue(t, a.Ack().VIP))
	ib, err := b.WaitIntro(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if ib.Peer.Cands[0][0] != "10.1.2.3" {
		t.Fatalf("cands after keepalive: %v", ib.Peer.Cands)
	}
}

func TestShutdownNotice(t *testing.T) {
	ts := startServer(t)
	c := ts.newClient("A")
	ts.stop()
	n, err := c.WaitNotice(2 * time.Second)
	if err != nil || !bytes.Contains(n, []byte(`"code":"shutdown"`)) {
		t.Fatalf("notice %s %v", n, err)
	}
}

func TestPropRoundTrip(t *testing.T) {
	cases := []string{
		`{"id":1,"t":1,"v":-2147483648}`, `{"id":2,"t":2,"v":-9223372036854775808}`, `{"id":3,"t":3,"v":1.5}`,
		`{"id":4,"t":4,"v":"AEEAAA=="}`, `{"id":5,"t":5,"v":0.25}`, `{"id":6,"t":6,"v":"AQID"}`,
		`{"id":7,"t":7,"v":133000000000000000}`, `{"id":4294967295,"t":0,"v":7}`,
	}
	for _, c := range cases {
		var p Prop
		if err := json.Unmarshal([]byte(c), &p); err != nil {
			t.Fatalf("%s: %v", c, err)
		}
		out, _ := json.Marshal(p)
		if string(out) != c {
			t.Errorf("round trip %s -> %s", c, out)
		}
	}
	var p Prop
	if err := json.Unmarshal([]byte(`{"id":1,"t":1,"v":4294967295}`), &p); err != nil || p.I != -1 {
		t.Errorf("u32 into int32: %+v %v", p, err)
	}
	for _, bad := range []string{`{"id":-1,"t":1,"v":0}`, `{"id":1,"t":8,"v":0}`, `{"id":1,"t":1}`,
		`{"id":1,"t":4,"v":"%%%"}`, `{"id":1,"t":1,"v":"x"}`, `{"id":1,"t":2,"v":1e400}`} {
		if err := json.Unmarshal([]byte(bad), &p); err == nil {
			t.Errorf("accepted %s", bad)
		}
	}
	var c Context
	if err := json.Unmarshal([]byte(`[32778,1]`), &c); err != nil || c.ID != 32778 || c.Value != 1 {
		t.Errorf("context: %+v %v", c, err)
	}
	if err := json.Unmarshal([]byte(`[1,2,3]`), &c); err == nil {
		t.Error("3-element context accepted")
	}
	big := `{"id":1,"t":6,"v":"` + base64.StdEncoding.EncodeToString(make([]byte, 1025)) + `"}`
	if err := json.Unmarshal([]byte(big), &p); err == nil {
		t.Error("1025-byte binary accepted")
	}
}

func TestAC6SearchQueries(t *testing.T) {
	ts := startServer(t)
	h1 := ts.newClient("H1")
	h2 := ts.newClient("H2")
	searcher := ts.newClient("S")
	i32 := func(id uint32, v int64) map[string]any { return map[string]any{"id": id, "t": TypeInt32, "v": v} }
	mk := func(h *client.Client, ctxs [][]uint32, props []any) string {
		var r createResult
		call(t, h, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat, "flags": 0x2F,
			"public_slots": 8, "contexts": ctxs, "properties": props}, &r)
		return r.SessionID
	}
	s1 := mk(h1, [][]uint32{{0x8001, 4}, {0x800B, 2}, {0x800A, 1}}, []any{i32(0x1000000C, 5), i32(0x1000000D, 0)})
	s2 := mk(h1, [][]uint32{{0x800A, 1}, {0x800B, 2}}, []any{i32(0x1000000C, 7), i32(0x1000000D, 0)})
	s3 := mk(h1, [][]uint32{{0x800A, 1}, {0x800B, 2}}, []any{i32(0x1000000C, 5), i32(0x1000000D, 1)})
	s4 := mk(h2, [][]uint32{{0x800A, 1}}, []any{})
	mk(h2, [][]uint32{{0x800A, 0}, {0x800B, 2}}, []any{i32(0x1000000C, 5)})

	search := func(proc int, props []any) ([]string, []docResult) {
		var r struct {
			Results []docResult `json:"results"`
		}
		call(t, searcher, "session.search", map[string]any{"title": "4E4D07D1", "compat": testCompat, "proc": proc,
			"max_results": 50, "num_users": 1, "contexts": [][]uint32{{0x800A, 1}, {0x800B, 2}}, "properties": props}, &r)
		ids := map[string]bool{}
		for _, d := range r.Results {
			ids[d.SessionID] = true
		}
		out := []string{}
		for _, id := range []string{s1, s2, s3, s4} {
			if ids[id] {
				out = append(out, id)
			}
		}
		if len(out) != len(r.Results) {
			t.Fatalf("unexpected sessions in results: %d results", len(r.Results))
		}
		return out, r.Results
	}
	check := func(name string, got []string, want ...string) {
		t.Helper()
		if strings.Join(got, ",") != strings.Join(want, ",") {
			t.Errorf("%s: got %v want %v", name, got, want)
		}
	}
	got, docs := search(1, []any{i32(0x1000000C, 5)})
	check("proc 1, mission 5", got, s1, s4)
	for _, d := range docs {
		if d.SessionID == s1 && (len(d.Contexts) != 2 || d.Contexts[0][0] != 0x800A || d.Contexts[1][0] != 0x800B ||
			len(d.Properties) != 0) {
			t.Errorf("result attributes %v %v", d.Contexts, d.Properties)
		}
	}
	got, _ = search(1, []any{})
	check("proc 1, any mission", got, s1, s2, s4)
	got, _ = search(0, []any{i32(0x1000000C, 5)})
	check("proc 0, mission 5", got, s1, s3, s4)
}

func TestAC6StatsSchema(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("Alpha")
	b := ts.newClient("Bravo")
	r := createSession(t, a, 0x1F, 4, 0, nil)
	call(t, b, "session.join", map[string]any{"session_id": r.SessionID, "members": []any{map[string]any{"xuid": b.Ack().XUID}}}, nil)
	i64 := func(id uint32, v int64) map[string]any { return map[string]any{"id": id, "t": TypeInt64, "v": v} }
	i32 := func(id uint32, v int64) map[string]any { return map[string]any{"id": id, "t": TypeInt32, "v": v} }
	write := func(c *client.Client, props ...any) {
		call(t, c, "stats.write", map[string]any{"session_id": r.SessionID, "title": "4E4D07D1", "xuid": c.Ack().XUID,
			"views": []any{map[string]any{"view_id": 86, "props": props}}}, nil)
	}
	write(a, i64(0x20000008, 100), i32(0x10000009, 3), i64(0x20000006, 50), i64(0x2000000B, 77), i32(0x10008001, 9))
	write(a, i64(0x20000008, 80), i32(0x10000009, 4), i64(0x20000006, 60))
	write(b, i64(0x20000008, 100), i64(0x20000006, 10)) // ties A on rating, later
	var rd struct {
		Views []struct {
			TotalRows int `json:"total_rows"`
			Rows      []struct {
				XUID    string            `json:"xuid"`
				Rank    int               `json:"rank"`
				Rating  int64             `json:"rating"`
				Columns []json.RawMessage `json:"columns"`
			} `json:"rows"`
		} `json:"views"`
	}
	call(t, a, "stats.read", map[string]any{"title": "4E4D07D1", "xuids": []string{a.Ack().XUID, b.Ack().XUID},
		"specs": []any{map[string]any{"view_id": 86, "columns": []int{2, 3, 1, 65534, 65535, 65533}}}}, &rd)
	rows := rd.Views[0].Rows
	if len(rows) != 2 || rows[0].Rank != 1 || rows[1].Rank != 2 || rows[0].Rating != 100 || rows[1].Rating != 100 {
		t.Fatalf("rows %+v", rows)
	}
	cols := []string{}
	for _, c := range rows[0].Columns {
		cols = append(cols, string(c))
	}
	want := []string{`{"id":2,"t":1,"v":4}`, `{"id":3,"t":2,"v":60}`, `{"id":1,"t":2,"v":77}`,
		`{"id":65534,"t":2,"v":100}`, `{"id":65535,"t":1,"v":1}`,
		`{"id":65533,"t":4,"v":"` + base64.StdEncoding.EncodeToString(utf16BE("Alpha")) + `"}`}
	if strings.Join(cols, " ") != strings.Join(want, " ") {
		t.Fatalf("columns\n got %v\nwant %v", cols, want)
	}
	if string(rows[1].Columns[0]) != `{"id":2,"t":255,"v":0}` {
		t.Fatalf("missing column: %s", rows[1].Columns[0])
	}
}

// A boot that times out and registers again keeps the same s2c key, so the server must move
// its counter forward, and packets from before the timeout must not replay.
func TestReRegistrationSameBoot(t *testing.T) {
	clock := newFakeClock()
	ts := startServer(t, withClock(clock))
	c := ts.newClient("A")
	for i := 0; i < 5; i++ {
		if _, err := c.Keepalive(nil, 2*time.Second); err != nil {
			t.Fatal(err)
		}
	}
	before := c.S2CHighest()
	captured := c.BuildC2S([]byte{wire.KindKeepalive})
	clock.Advance(livenessTimeout + time.Second)
	ts.sweepNow()
	if ts.Status().Online != 0 {
		t.Fatal("client did not time out")
	}
	// One HELLO must be enough: its HELLO_ACK has to carry a counter above everything sent
	// before with this key.
	if err := c.SendRaw(c.HelloPacket()); err != nil {
		t.Fatal(err)
	}
	if _, err := c.WaitAck(time.Second); err != nil {
		t.Fatalf("HELLO_ACK after re-registration not accepted (counter reuse?): %v", err)
	}
	if c.S2CHighest() <= before {
		t.Fatalf("s2c counter did not move forward: %d <= %d", c.S2CHighest(), before)
	}
	// The packet built before the timeout is older than the HELLO: rejected.
	c.SendRaw(captured)
	if _, err := c.WaitKeepaliveAck(200 * time.Millisecond); err == nil {
		t.Fatal("pre-timeout packet replayed into the new registration")
	}
	if _, err := c.Keepalive(nil, time.Second); err != nil {
		t.Fatal(err)
	}
}

// A new account is on disk before its first HELLO_ACK, so a crash right after cannot give the
// same key a different identity.
func TestNewAccountDurableBeforeAck(t *testing.T) {
	dir := t.TempDir()
	ts := startServer(t, withData(dir))
	for i := 0; i < 3; i++ {
		c := ts.newClient("Durable")
		data, err := os.ReadFile(filepath.Join(dir, "accounts.json"))
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Contains(data, []byte(c.Ack().PeerID)) {
			t.Fatalf("account %s not on disk when its HELLO_ACK arrived", c.Ack().PeerID)
		}
	}
}

func TestStatsWithoutServerSession(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("Solo")
	b := ts.newClient("Other")
	w := func(c *client.Client, xuid string, view int) error {
		return c.Call("stats.write", map[string]any{"session_id": "0000000000000000", "title": "4E4D07D1", "xuid": xuid,
			"views": []any{map[string]any{"view_id": view, "props": []any{map[string]any{"id": 0x20000006, "t": TypeInt64, "v": 42}}}}}, nil)
	}
	if err := w(a, a.Ack().XUID, 12); err != nil { // campaign board: allowed
		t.Fatal(err)
	}
	if err, ok := w(a, b.Ack().XUID, 12).(*client.RPCError); !ok || err.Code != "not_host" {
		t.Fatalf("writing another player's row: %v", err)
	}
	if err, ok := w(a, a.Ack().XUID, 86).(*client.RPCError); !ok || err.Code != "state" {
		t.Fatalf("ranked view without a session: %v", err)
	}
	var rd struct {
		Views []struct {
			Rows []struct {
				Columns []json.RawMessage `json:"columns"`
			} `json:"rows"`
		} `json:"views"`
	}
	call(t, b, "stats.read", map[string]any{"title": "4E4D07D1", "xuids": []string{a.Ack().XUID},
		"specs": []any{map[string]any{"view_id": 12, "columns": []int{3}}}}, &rd)
	if len(rd.Views[0].Rows) != 1 || string(rd.Views[0].Rows[0].Columns[0]) != `{"id":3,"t":2,"v":42}` {
		t.Fatalf("read back: %+v", rd)
	}
}
