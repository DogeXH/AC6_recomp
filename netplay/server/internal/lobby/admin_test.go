package lobby

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"ac6lobby/internal/client"
	"ac6lobby/internal/wire"
)

// helloFails checks that a HELLO with sk gets no answer.
func helloFails(t *testing.T, ts *testServer, sk [32]byte) {
	t.Helper()
	c, err := client.New(ts.addr, ts.PublicKey(), sk)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if _, err := c.Hello(client.HelloInfo{Gamertag: "Again", Title: "4E4D07D1", Compat: testCompat}, 700*time.Millisecond); err == nil {
		t.Fatal("banned account registered")
	}
}

func TestBanPersistsAndRefusesHello(t *testing.T) {
	dir := t.TempDir()
	key := client.NewKey()
	ts := startServer(t, withData(dir), withKey(key))
	sk := client.NewKey()
	a := ts.newClientKey("Grief", sk)
	b := ts.newClient("Bystander")
	host := createSession(t, a, 0x42F, 4, 0, [][]uint32{{0x800A, 1}, {0x800B, 1}})
	_ = host

	info, err := ts.Ban(Target{Gamertag: "grief"}, "team killing", 0) // case-insensitive
	if err != nil {
		t.Fatal(err)
	}
	if !info.WasOnline || info.Expires != "" || info.Gamertag != "Grief" || info.Reason != "team killing" {
		t.Fatalf("ban info %+v", info)
	}
	n, err := a.WaitNotice(2 * time.Second)
	if err != nil || !bytes.Contains(n, []byte(`"code":"banned"`)) || !bytes.Contains(n, []byte("team killing")) {
		t.Fatalf("ban notice %s %v", n, err)
	}
	if st := ts.Status(); st.Online != 1 || st.Sessions != 0 {
		t.Fatalf("after ban: %+v (the banned host's session must go)", st)
	}
	helloFails(t, ts, sk)
	if ts.Stats().BannedRefused == 0 {
		t.Fatal("refused HELLO not counted")
	}
	if got := ts.Bans(); len(got) != 1 || got[0].PeerID != info.PeerID {
		t.Fatalf("bans %+v", got)
	}
	_ = b

	// The ban is on disk at once and survives a restart.
	waitForFile(t, filepath.Join(dir, "bans.json"))
	st, _ := os.Stat(filepath.Join(dir, "bans.json"))
	if st.Mode().Perm() != 0o600 {
		t.Fatalf("bans.json mode %v", st.Mode())
	}
	ts.stop()
	portMu.Lock()
	nextPort = mustPort(ts.addr)
	portMu.Unlock()
	ts2 := startServer(t, withData(dir), withKey(key))
	helloFails(t, ts2, sk)

	if _, err := ts2.Unban(Target{PeerID: strings.ToUpper(info.PeerID)}); err != nil {
		t.Fatal(err)
	}
	if _, err := ts2.Unban(Target{PeerID: info.PeerID}); !errors.Is(err, ErrNotBanned) {
		t.Fatalf("second unban: %v", err)
	}
	c := ts2.newClientKey("Grief", sk)
	if c.Ack().PeerID != info.PeerID {
		t.Fatalf("identity after unban %s, want %s", c.Ack().PeerID, info.PeerID)
	}
	ts2.stop()
	var f bansFile
	data, _ := os.ReadFile(filepath.Join(dir, "bans.json"))
	if err := json.Unmarshal(data, &f); err != nil || len(f.Bans) != 0 {
		t.Fatalf("bans.json after unban: %s %v", data, err)
	}
}

func TestTemporaryBanExpires(t *testing.T) {
	clock := newFakeClock()
	ts := startServer(t, withClock(clock))
	sk := client.NewKey()
	ts.newClientKey("Hothead", sk)
	info, err := ts.Ban(Target{Gamertag: "Hothead"}, "", 90*time.Minute)
	if err != nil || info.Expires == "" {
		t.Fatalf("ban %+v %v", info, err)
	}
	helloFails(t, ts, sk)
	clock.Advance(91 * time.Minute)
	ts.sweepNow()
	if len(ts.Bans()) != 0 {
		t.Fatal("expired ban still listed")
	}
	ts.newClientKey("Hothead", sk) // registers again
}

func TestKickAllowsReconnect(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("Loud")
	createSession(t, a, 0x42F, 4, 0, nil)
	p, err := ts.Kick(Target{Gamertag: "Loud"}, "spamming")
	if err != nil || p.Gamertag != "Loud" || p.Hosting != 1 {
		t.Fatalf("kick %+v %v", p, err)
	}
	n, err := a.WaitNotice(2 * time.Second)
	if err != nil || !bytes.Contains(n, []byte(`"code":"kick"`)) {
		t.Fatalf("kick notice %s %v", n, err)
	}
	if st := ts.Status(); st.Online != 0 || st.Sessions != 0 {
		t.Fatalf("after kick: %+v", st)
	}
	if _, err := ts.Kick(Target{Gamertag: "Loud"}, ""); !errors.Is(err, ErrNotOnline) {
		t.Fatalf("kick offline: %v", err)
	}
	// The kicked client's next packet gets REHELLO, and it registers again.
	a.AutoRehello = true
	var w whoami
	if err := a.Call("whoami", nil, &w); err != nil || w.Gamertag != "Loud" {
		t.Fatalf("whoami after kick: %+v %v", w, err)
	}
	if ts.Stats().Kicks != 1 {
		t.Fatalf("kicks %d", ts.Stats().Kicks)
	}
}

func TestAdminTargets(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("Alpha")
	for _, tc := range []struct {
		t    Target
		want error
	}{
		{Target{}, ErrBadTarget},
		{Target{PeerID: a.Ack().PeerID, Gamertag: "Alpha"}, ErrBadTarget},
		{Target{Gamertag: "Nobody"}, ErrNotFound},
		{Target{PeerID: "0123456789abcdef"}, ErrNotFound},
	} {
		if _, err := ts.Kick(tc.t, ""); !errors.Is(err, tc.want) {
			t.Errorf("kick %+v: %v, want %v", tc.t, err, tc.want)
		}
	}
	if _, err := ts.Ban(Target{PeerID: "xyz"}, "", 0); err == nil {
		t.Error("bad peer_id accepted")
	}
	if _, err := ts.Unban(Target{Gamertag: "Alpha"}); !errors.Is(err, ErrNotBanned) {
		t.Errorf("unban not banned: %v", err)
	}
	if _, err := ts.Ban(Target{PeerID: a.Ack().PeerID}, "", -time.Second); err == nil {
		t.Error("negative duration accepted")
	}
}

func TestAdminViewsAndPublicCounts(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("Talisman")
	b := ts.newClient("Shamrock")
	ranked := createSession(t, a, 0x53F, 8, 0, [][]uint32{{0x800A, 0}, {0x800B, 1}})
	call(t, a, "session.join", map[string]any{"session_id": ranked.SessionID,
		"members": []map[string]any{{"xuid": a.Ack().XUID}}}, nil)
	call(t, b, "session.join", map[string]any{"session_id": ranked.SessionID,
		"members": []map[string]any{{"xuid": b.Ack().XUID}}}, nil)
	coop := createSession(t, b, 0x42F, 4, 0, [][]uint32{{0x800A, 1}, {0x800B, 7}})
	call(t, b, "session.start", map[string]any{"session_id": coop.SessionID}, nil)
	createSession(t, b, 0x42F, 4, 0, nil) // no AC6 mode

	players := ts.Players(false)
	if len(players) != 2 || players[0].Gamertag != "Shamrock" || players[1].Gamertag != "Talisman" {
		t.Fatalf("players %+v", players)
	}
	if players[0].Addr != "" || players[0].Hosting != 2 || players[1].Hosting != 1 {
		t.Fatalf("players %+v", players)
	}
	if p := ts.Players(true); !strings.HasPrefix(p[0].Addr, "127.0.0.1:") {
		t.Fatalf("addr %q", p[0].Addr)
	}

	sess := ts.Sessions()
	if len(sess) != 3 {
		t.Fatalf("sessions %+v", sess)
	}
	byID := map[string]SessionInfo{}
	for _, s := range sess {
		byID[s.SessionID] = s
	}
	r := byID[ranked.SessionID]
	if r.ModeName != "ROYALE" || r.Type != "ranked" || r.Host != "Talisman" || !r.HostOnline ||
		strings.Join(r.Members, ",") != "Talisman,Shamrock" || r.State != "lobby" {
		t.Fatalf("ranked session %+v", r)
	}
	if c := byID[coop.SessionID]; c.ModeName != "COOP_DL" || c.Type != "standard" || c.State != "ingame" {
		t.Fatalf("coop session %+v", c)
	}

	counts := ts.SessionCounts()
	if counts.Total != 3 || counts.Lobby != 2 || counts.InGame != 1 || counts.Other != 1 || counts.Players != 2 ||
		counts.Modes[1].Ranked != 1 || counts.Modes[1].Players != 2 || counts.Modes[7].Standard != 1 || len(counts.Modes) != 8 {
		t.Fatalf("counts %+v", counts)
	}
	js, _ := json.Marshal(counts)
	for _, secret := range []string{"Talisman", "Shamrock", "127.0.0.1", a.Ack().XUID, ranked.SessionID, a.Ack().PeerID} {
		if bytes.Contains(js, []byte(secret)) {
			t.Fatalf("public counts leak %q: %s", secret, js)
		}
	}

	m := ts.Metrics()
	if m.Online != 2 || m.Sessions != 3 || m.Hellos < 2 || m.RPCs < 5 {
		t.Fatalf("metrics %+v", m)
	}
}

func TestBroadcast(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("One")
	b := ts.newClient("Two")
	if n := ts.Broadcast("restart in 5 minutes"); n != 2 {
		t.Fatalf("broadcast reached %d", n)
	}
	for _, c := range []*client.Client{a, b} {
		n, err := c.WaitNotice(2 * time.Second)
		if err != nil || !bytes.Contains(n, []byte(`"code":"info"`)) || !bytes.Contains(n, []byte("restart in 5 minutes")) {
			t.Fatalf("notice %s %v", n, err)
		}
	}
}

func TestRelayCountersPerClient(t *testing.T) {
	ts := startServer(t)
	a := ts.newClient("A")
	b := ts.newClient("B")
	if err := a.IntroReq(1, 2, b.PeerID()); err != nil {
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
	if _, err := a.AddPeer(ia.Peer); err != nil {
		t.Fatal(err)
	}
	if _, err := b.AddPeer(ib.Peer); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 5; i++ {
		if err := a.SendRelay(b.PeerID(), []byte{0x03}); err != nil {
			t.Fatal(err)
		}
		if _, err := b.WaitRelayed(2 * time.Second); err != nil {
			t.Fatal(err)
		}
	}
	m := ts.Metrics()
	if m.Relayed != 5 || m.RelayedBytes == 0 {
		t.Fatalf("metrics %+v", m)
	}
	for _, p := range ts.Players(false) {
		if p.Gamertag == "A" && (p.RelayPkts != 5 || p.RelayBytes != m.RelayedBytes || p.Peers != 1) {
			t.Fatalf("player A %+v", p)
		}
	}
}

func TestForgetDeletesAccountAndStats(t *testing.T) {
	dir := t.TempDir()
	ts := startServer(t, withData(dir))
	sk := client.NewKey()
	a := ts.newClientKey("Erase", sk)
	b := ts.newClient("Keep")
	for _, c := range []*client.Client{a, b} {
		call(t, c, "stats.write", map[string]any{"session_id": "0000000000000000", "title": "4E4D07D1",
			"xuid": c.Ack().XUID, "views": []map[string]any{{"view_id": 1,
				"props": []map[string]any{{"id": 0x20000008, "t": 2, "v": 500}}}}}, nil)
	}
	other := createSession(t, b, 0x42F, 4, 0, nil)
	call(t, a, "session.join", map[string]any{"session_id": other.SessionID,
		"members": []map[string]any{{"xuid": a.Ack().XUID}}}, nil)
	createSession(t, a, 0x42F, 4, 0, nil)

	f, err := ts.Forget(Target{Gamertag: "Erase"})
	if err != nil || !f.WasOnline || f.StatsRows != 1 || f.XUID != a.Ack().XUID || f.Banned {
		t.Fatalf("forget %+v %v", f, err)
	}
	if st := ts.Status(); st.Accounts != 1 || st.Online != 1 || st.Sessions != 1 {
		t.Fatalf("after forget: %+v", st)
	}
	if s := ts.Sessions(); len(s) != 1 || len(s[0].Members) != 0 {
		t.Fatalf("sessions %+v", s)
	}
	if _, err := ts.Forget(Target{Gamertag: "Erase"}); !errors.Is(err, ErrNotFound) {
		t.Fatalf("second forget: %v", err)
	}
	ts.stop()
	for _, name := range []string{"accounts.json", "stats.json"} {
		data, _ := os.ReadFile(filepath.Join(dir, name))
		if bytes.Contains(data, []byte(a.Ack().XUID)) || bytes.Contains(data, []byte("Erase")) {
			t.Fatalf("%s still has the account: %s", name, data)
		}
		if !bytes.Contains(data, []byte(b.Ack().XUID)) {
			t.Fatalf("%s lost the other account", name)
		}
	}
	// The same key gets a new account afterwards.
	portMu.Lock()
	nextPort = mustPort(ts.addr)
	portMu.Unlock()
	ts2 := startServer(t, withData(dir), withKey(ts.sk))
	c := ts2.newClientKey("Erase", sk)
	if c.Ack().XUID == a.Ack().XUID || c.Ack().PeerID == a.Ack().PeerID {
		t.Fatal("the forgotten identity came back")
	}
}

func TestForgetKeepsBan(t *testing.T) {
	ts := startServer(t)
	sk := client.NewKey()
	ts.newClientKey("Banned", sk)
	if _, err := ts.Ban(Target{Gamertag: "Banned"}, "", 0); err != nil {
		t.Fatal(err)
	}
	f, err := ts.Forget(Target{Gamertag: "Banned"})
	if err != nil || !f.Banned {
		t.Fatalf("forget %+v %v", f, err)
	}
	helloFails(t, ts, sk)
	if _, err := ts.Unban(Target{Gamertag: "Banned"}); err != nil { // found through the ban record
		t.Fatal(err)
	}
	ts.newClientKey("Banned", sk)
}

func TestPreviousKeyAcceptedDuringRotation(t *testing.T) {
	oldKey, newKey := client.NewKey(), client.NewKey()
	ts := startServer(t, withKey(newKey), func(c *Config) { c.PrevSecretKeys = []wire.Key{oldKey} })
	oldPub := wire.PublicKey(oldKey)
	if got := ts.PrevPublicKeys(); len(got) != 1 || got[0] != oldPub {
		t.Fatalf("prev keys %x", got)
	}
	register := func(tag string, pin wire.Key) (*client.Client, error) {
		c, err := client.New(ts.addr, pin, client.NewKey())
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(c.Close)
		_, err = c.Hello(client.HelloInfo{Gamertag: tag, Title: "4E4D07D1", Compat: testCompat}, 1500*time.Millisecond)
		return c, err
	}
	stale, err := register("OldPin", oldPub)
	if err != nil {
		t.Fatalf("client on the previous key: %v", err)
	}
	fresh, err := register("NewPin", ts.PublicKey())
	if err != nil {
		t.Fatalf("client on the new key: %v", err)
	}
	if _, err := register("Stranger", wire.PublicKey(client.NewKey())); err == nil {
		t.Fatal("a client pinning an unknown key registered")
	}
	var w whoami
	if err := stale.Call("whoami", nil, &w); err != nil || w.Gamertag != "OldPin" {
		t.Fatalf("rpc on the previous key: %+v %v", w, err)
	}
	if err := stale.IntroReq(1, 2, fresh.PeerID()); err != nil {
		t.Fatal(err)
	}
	ia, err := stale.WaitIntro(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	ib, err := fresh.WaitIntro(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	stale.AddPeer(ia.Peer)
	fresh.AddPeer(ib.Peer)
	if err := stale.SendRelay(fresh.PeerID(), []byte{0x03}); err != nil {
		t.Fatal(err)
	}
	if _, err := fresh.WaitRelayed(2 * time.Second); err != nil {
		t.Fatalf("relay between old-key and new-key clients: %v", err)
	}
	if m := ts.Metrics(); m.OnlineOldKey != 1 || m.Online != 2 {
		t.Fatalf("metrics %+v", m)
	}
	for _, p := range ts.Players(false) {
		if p.OldKey != (p.Gamertag == "OldPin") {
			t.Fatalf("player %+v", p)
		}
	}
}

func TestClientStringSanitized(t *testing.T) {
	if got := printableASCII("ac6\x1b[31mred\x07\n/0.1é", 64); got != "ac6[31mred/0.1" {
		t.Fatalf("got %q", got)
	}
	if got := printableASCII(strings.Repeat("x", 100), 64); len(got) != 64 {
		t.Fatalf("len %d", len(got))
	}
}
