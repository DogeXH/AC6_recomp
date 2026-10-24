package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"ac6lobby/internal/client"
	"ac6lobby/internal/lobby"
)

// mainTestPort is the last port of AC6LOBBY_TEST_PORTS (default 47829); the lobby package
// tests leave it free.
func mainTestPort() int {
	last := 47829
	if v := os.Getenv("AC6LOBBY_TEST_PORTS"); v != "" {
		var a, b int
		if n, _ := fmt.Sscanf(v, "%d-%d", &a, &b); n == 2 && b >= a {
			last = b
		}
	}
	return last
}

func quietLog() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

// udpServer runs a lobby server on the main test port.
func udpServer(t *testing.T) (*lobby.Server, string) {
	t.Helper()
	sk, err := lobby.LoadOrCreateKey(t.TempDir(), quietLog())
	if err != nil {
		t.Fatal(err)
	}
	srv, err := lobby.New(lobby.Config{SecretKey: sk, Logger: quietLog()})
	if err != nil {
		t.Fatal(err)
	}
	addr := fmt.Sprintf("127.0.0.1:%d", mainTestPort())
	conn, err := net.ListenUDP("udp", net.UDPAddrFromAddrPort(netip.MustParseAddrPort(addr)))
	if err != nil {
		srv.Close()
		t.Fatalf("listen %s: %v", addr, err)
	}
	done := make(chan struct{})
	go func() { srv.Serve(conn); close(done) }()
	t.Cleanup(func() { srv.Close(); conn.Close(); <-done })
	return srv, addr
}

func register(t *testing.T, srv *lobby.Server, addr, tag string) *client.Client {
	t.Helper()
	c, err := client.New(addr, srv.PublicKey(), client.NewKey())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(c.Close)
	if _, err := c.Hello(client.HelloInfo{Gamertag: tag, Title: "4E4D07D1",
		Compat: "a51a535851e632111a59c1dcd86a42ec"}, 3*time.Second); err != nil {
		t.Fatal(err)
	}
	return c
}

func TestAdminHTTP(t *testing.T) {
	srv, addr := udpServer(t)
	c := register(t, srv, addr, "Pixy")
	token := []byte(strings.Repeat("ab", 32))
	h := adminServer(srv, token, nil, quietLog()).Handler

	do := func(method, path, tok, ctype, body string) (int, map[string]any) {
		req := httptest.NewRequest(method, "http://127.0.0.1:47802"+path, strings.NewReader(body))
		if tok != "" {
			req.Header.Set("Authorization", "Bearer "+tok)
		}
		if ctype != "" {
			req.Header.Set("Content-Type", ctype)
		}
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		var doc map[string]any
		json.Unmarshal(rec.Body.Bytes(), &doc)
		return rec.Code, doc
	}
	good := string(token)
	if code, _ := do("GET", "/admin/v1/players", "", "", ""); code != 401 {
		t.Fatalf("no token: %d", code)
	}
	if code, _ := do("GET", "/admin/v1/players", good[:63]+"x", "", ""); code != 401 {
		t.Fatalf("wrong token: %d", code)
	}
	code, doc := do("GET", "/admin/v1/players", good, "", "")
	if code != 200 || !strings.Contains(fmt.Sprint(doc), "Pixy") || strings.Contains(fmt.Sprint(doc), "127.0.0.1") {
		t.Fatalf("players: %d %v", code, doc)
	}
	if code, doc := do("GET", "/admin/v1/players?addr=1", good, "", ""); code != 200 || !strings.Contains(fmt.Sprint(doc), "127.0.0.1:") {
		t.Fatalf("players with addr: %d %v", code, doc)
	}
	for _, p := range []string{"sessions", "bans", "metrics"} {
		if code, _ := do("GET", "/admin/v1/"+p, good, "", ""); code != 200 {
			t.Fatalf("%s: %d", p, code)
		}
	}
	if code, _ := do("POST", "/admin/v1/ban", good, "text/plain", `{"gamertag":"Pixy"}`); code != 415 {
		t.Fatalf("ban without json: %d", code)
	}
	if code, _ := do("POST", "/admin/v1/ban", good, "application/json", `{"gamertag":"Nobody"}`); code != 404 {
		t.Fatalf("ban unknown: %d", code)
	}
	if code, _ := do("POST", "/admin/v1/ban", good, "application/json", `{"gamertag":"Pixy","duration":"soon"}`); code != 400 {
		t.Fatalf("ban bad duration: %d", code)
	}
	if code, _ := do("POST", "/admin/v1/ban", good, "application/json", `{"gamertag":"Pixy","extra":1}`); code != 400 {
		t.Fatalf("ban unknown field: %d", code)
	}
	if code, _ := do("GET", "/admin/v1/ban", good, "", ""); code != 405 {
		t.Fatalf("GET ban: %d", code)
	}
	code, doc = do("POST", "/admin/v1/ban", good, "application/json", `{"gamertag":"Pixy","reason":"test","duration":"7d"}`)
	if code != 200 || !strings.Contains(fmt.Sprint(doc), "was_online:true") {
		t.Fatalf("ban: %d %v", code, doc)
	}
	if n, err := c.WaitNotice(2 * time.Second); err != nil || !bytes.Contains(n, []byte("banned")) {
		t.Fatalf("notice %s %v", n, err)
	}
	if code, doc := do("GET", "/admin/v1/bans", good, "", ""); code != 200 || !strings.Contains(fmt.Sprint(doc), "Pixy") {
		t.Fatalf("bans: %d %v", code, doc)
	}
	if code, _ := do("POST", "/admin/v1/kick", good, "application/json", `{"gamertag":"Pixy"}`); code != 409 {
		t.Fatalf("kick offline: %d", code)
	}
	if code, _ := do("POST", "/admin/v1/unban", good, "application/json", `{"peer_id":"`+c.Ack().PeerID+`"}`); code != 200 {
		t.Fatalf("unban: %d", code)
	}
	if code, _ := do("POST", "/admin/v1/unban", good, "application/json", `{"peer_id":"`+c.Ack().PeerID+`"}`); code != 404 {
		t.Fatalf("unban again: %d", code)
	}
	if code, doc := do("POST", "/admin/v1/broadcast", good, "application/json", `{"msg":"hi"}`); code != 200 || doc["sent"] != 0.0 {
		t.Fatalf("broadcast: %d %v", code, doc)
	}
	if code, _ := do("POST", "/admin/v1/forget", good, "application/json", `{"gamertag":"Nobody"}`); code != 404 {
		t.Fatalf("forget unknown: %d", code)
	}
	if code, doc := do("POST", "/admin/v1/forget", good, "application/json", `{"gamertag":"Pixy"}`); code != 200 ||
		!strings.Contains(fmt.Sprint(doc), c.Ack().XUID) {
		t.Fatalf("forget: %d %v", code, doc)
	}
	if st := srv.Status(); st.Accounts != 0 {
		t.Fatalf("accounts after forget: %+v", st)
	}
}

func TestStatusPagesCountsOnly(t *testing.T) {
	srv, addr := udpServer(t)
	a := register(t, srv, addr, "Garuda")
	var cr struct {
		SessionID string `json:"session_id"`
	}
	if err := a.Call("session.create", map[string]any{"title": "4E4D07D1", "compat": "a51a535851e632111a59c1dcd86a42ec",
		"flags": 0x42F, "public_slots": 8, "private_slots": 0, "contexts": [][]uint32{{0x800A, 1}, {0x800B, 1}},
		"properties": []any{}}, &cr); err != nil {
		t.Fatal(err)
	}
	if err := a.Call("session.join", map[string]any{"session_id": cr.SessionID,
		"members": []map[string]any{{"xuid": a.Ack().XUID}}}, nil); err != nil {
		t.Fatal(err)
	}
	h := statusServer("127.0.0.1:0", srv).Handler
	get := func(path string) (int, string, string) {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
		return rec.Code, rec.Header().Get("Content-Type"), rec.Body.String()
	}
	code, ctype, body := get("/status/sessions")
	if code != 200 || !strings.HasPrefix(ctype, "application/json") {
		t.Fatalf("/status/sessions %d %s", code, ctype)
	}
	var counts lobby.SessionCounts
	if err := json.Unmarshal([]byte(body), &counts); err != nil || counts.Total != 1 || counts.Modes[1].Standard != 1 ||
		counts.Modes[1].Players != 1 {
		t.Fatalf("counts %s %v", body, err)
	}
	code, ctype, page := get("/")
	if code != 200 || !strings.HasPrefix(ctype, "text/html") || !strings.Contains(page, "Battle Royale") {
		t.Fatalf("/ %d %s", code, ctype)
	}
	for _, b := range []string{body, page} {
		for _, secret := range []string{"Garuda", "127.0.0.1", a.Ack().XUID, a.Ack().PeerID, cr.SessionID, "10.64."} {
			if strings.Contains(b, secret) {
				t.Fatalf("status page leaks %q", secret)
			}
		}
	}
	if code, _, _ := get("/admin/v1/players"); code != 404 {
		t.Fatalf("admin path on the status server: %d", code)
	}
}

func TestLoopbackAddr(t *testing.T) {
	for in, want := range map[string]string{"127.0.0.1:47802": "127.0.0.1:47802", "localhost:1": "127.0.0.1:1",
		"[::1]:9": "[::1]:9", "127.1.2.3:5": "127.1.2.3:5"} {
		if got, err := loopbackAddr(in); err != nil || got != want {
			t.Errorf("%s: %s %v", in, got, err)
		}
	}
	for _, in := range []string{"0.0.0.0:47802", ":47802", "192.168.1.2:1", "example.com:1", "[::]:1", "127.0.0.1"} {
		if _, err := loopbackAddr(in); err == nil {
			t.Errorf("%s accepted", in)
		}
	}
}

func TestParseDuration(t *testing.T) {
	for in, want := range map[string]time.Duration{"7d": 168 * time.Hour, "1d12h": 36 * time.Hour,
		"30m": 30 * time.Minute, "2h30m": 150 * time.Minute} {
		if got, err := parseDuration(in); err != nil || got != want {
			t.Errorf("%s: %v %v", in, got, err)
		}
	}
	for _, in := range []string{"", "soon", "d", "1.5d", "-1d", "7dd"} {
		if _, err := parseDuration(in); err == nil {
			t.Errorf("%q accepted", in)
		}
	}
}

func TestAdminToken(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "admin.token")
	tok, err := loadOrCreateToken(p, quietLog())
	if err != nil || len(tok) != 64 {
		t.Fatalf("token %q %v", tok, err)
	}
	if st, _ := os.Stat(p); st.Mode().Perm() != 0o600 {
		t.Fatalf("mode %v", st.Mode())
	}
	again, err := loadOrCreateToken(p, quietLog())
	if err != nil || string(again) != string(tok) {
		t.Fatal("token not reloaded")
	}
	os.WriteFile(p, []byte("short\n"), 0o600)
	if _, err := loadOrCreateToken(p, quietLog()); err == nil {
		t.Fatal("short token accepted")
	}
}

func TestParseWho(t *testing.T) {
	cases := map[string]lobby.Target{
		"0123456789abcdef": {PeerID: "0123456789abcdef"}, "Talisman": {Gamertag: "Talisman"},
		"tag:0123456789abcdef": {Gamertag: "0123456789abcdef"}, "peer:00000000000000ff": {PeerID: "00000000000000ff"},
	}
	for in, want := range cases {
		if got := parseWho(in); got != want {
			t.Errorf("%s: %+v", in, got)
		}
	}
}

func TestKeyCommands(t *testing.T) {
	dir := t.TempDir()
	if err := runKey([]string{"-data", dir, "show"}, io.Discard); err == nil {
		t.Fatal("show without a key succeeded")
	}
	sk, err := lobby.LoadOrCreateKey(dir, quietLog())
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := runKey([]string{"-data", dir, "show"}, &out); err != nil {
		t.Fatal(err)
	}
	pub, _ := os.ReadFile(filepath.Join(dir, "server.pub"))
	if !strings.HasPrefix(out.String(), strings.TrimSpace(string(pub))+"\n") {
		t.Fatalf("show printed %q, server.pub %q", out.String(), pub)
	}
	out.Reset()
	if err := runKey([]string{"-data", dir, "rotate"}, &out); err != nil {
		t.Fatal(err)
	}
	pub2, _ := os.ReadFile(filepath.Join(dir, "server.pub"))
	if string(pub2) == string(pub) || !strings.Contains(out.String(), strings.TrimSpace(string(pub2))) {
		t.Fatalf("rotate: %s", out.String())
	}
	olds, _ := filepath.Glob(filepath.Join(dir, "server.key.*.old"))
	if len(olds) != 1 {
		t.Fatalf("backups %v", olds)
	}
	old, _ := os.ReadFile(olds[0])
	if strings.TrimSpace(string(old)) != fmt.Sprintf("%x", sk[:]) {
		t.Fatal("the old secret was not kept")
	}
	if st, _ := os.Stat(olds[0]); st.Mode().Perm() != 0o600 {
		t.Fatalf("backup mode %v", st.Mode())
	}
	// Grace period: the old secret is accepted until "key retire".
	prev, err := lobby.LoadPrevKeys(dir)
	if err != nil || len(prev) != 1 || prev[0] != sk {
		t.Fatalf("server.prev.key %x %v", prev, err)
	}
	out.Reset()
	if err := runKey([]string{"-data", dir, "show"}, &out); err != nil || !strings.Contains(out.String(), "previous key "+strings.TrimSpace(string(pub))) {
		t.Fatalf("show during grace: %q %v", out.String(), err)
	}
	if err := runKey([]string{"-data", dir, "retire"}, io.Discard); err != nil {
		t.Fatal(err)
	}
	if prev, _ := lobby.LoadPrevKeys(dir); len(prev) != 0 {
		t.Fatal("retire left the previous key")
	}
	if err := runKey([]string{"-data", dir, "retire"}, io.Discard); err == nil {
		t.Fatal("second retire succeeded")
	}
	if err := runKey([]string{"-data", dir, "rotate", "-no-grace"}, io.Discard); err != nil {
		t.Fatal(err)
	}
	if prev, _ := lobby.LoadPrevKeys(dir); len(prev) != 0 {
		t.Fatal("-no-grace kept a previous key")
	}
	os.WriteFile(filepath.Join(dir, lobby.PrevKeyFile), []byte("zz\n"), 0o600)
	if _, err := lobby.LoadPrevKeys(dir); err == nil {
		t.Fatal("bad server.prev.key accepted")
	}
}

func TestHealthcheck(t *testing.T) {
	_, addr := udpServer(t)
	var out bytes.Buffer
	if err := runHealthcheck([]string{"-udp", addr}, &out); err != nil || !strings.HasPrefix(out.String(), "ok:") {
		t.Fatalf("healthcheck: %q %v", out.String(), err)
	}
}

func TestHealthcheckNoServer(t *testing.T) {
	addr := fmt.Sprintf("127.0.0.1:%d", mainTestPort()) // nothing listens here outside udpServer
	if err := runHealthcheck([]string{"-udp", addr, "-timeout", "300ms", "-tries", "2"}, io.Discard); err == nil {
		t.Fatal("healthcheck succeeded without a server")
	}
}

func TestServeFlags(t *testing.T) {
	if _, err := parseServeFlags([]string{"stray"}); err == nil {
		t.Fatal("stray argument accepted")
	}
	if _, err := parseServeFlags([]string{"-hello-per-ip", "0"}); err == nil {
		t.Fatal("zero limit accepted")
	}
	f, err := parseServeFlags([]string{"-admin", "127.0.0.1:1", "-log-format", "json"})
	if err != nil || f.admin != "127.0.0.1:1" || f.http != "127.0.0.1:47801" || f.udp != ":47800" {
		t.Fatalf("%+v %v", f, err)
	}
	if _, err := newLogger(io.Discard, "info", "xml"); err == nil {
		t.Fatal("bad log format accepted")
	}
}

func TestAdminCLI(t *testing.T) {
	t.Setenv("SUDO_USER", "opsadmin")
	srv, addr := udpServer(t)
	c := register(t, srv, addr, "Avril")
	dir := t.TempDir()
	tokenPath := filepath.Join(dir, "admin.token")
	token, err := loadOrCreateToken(tokenPath, quietLog())
	if err != nil {
		t.Fatal(err)
	}
	httpAddr := fmt.Sprintf("127.0.0.1:%d", mainTestPort()) // TCP; the UDP server uses the same number
	ln, err := net.Listen("tcp", httpAddr)
	if err != nil {
		t.Fatal(err)
	}
	audit, err := openAudit(filepath.Join(dir, auditFileName), 0, quietLog())
	if err != nil {
		t.Fatal(err)
	}
	hs := adminServer(srv, token, audit, quietLog())
	go hs.Serve(ln)
	t.Cleanup(func() { hs.Close() })

	admin := func(args ...string) (string, error) {
		var out bytes.Buffer
		err := runAdmin(append([]string{"-addr", httpAddr, "-token-file", tokenPath}, args...), &out)
		return out.String(), err
	}
	must := func(want string, args ...string) {
		t.Helper()
		out, err := admin(args...)
		if err != nil || !strings.Contains(out, want) {
			t.Fatalf("admin %v: %q %v (want %q)", args, out, err, want)
		}
	}
	must("1 online", "players")
	must("127.0.0.1:", "players", "-addr")
	must("0 sessions", "sessions")
	must(`"online": 1`, "metrics")
	must(`"gamertag": "Avril"`, "-json", "players")
	must("sent to 1 clients", "broadcast", "hello", "pilots")
	must("banned Avril ("+c.Ack().PeerID+") until", "ban", "-for", "1h", "Avril", "testing", "the", "cli")
	must("Avril", "bans")
	must("unbanned Avril", "unban", c.Ack().PeerID)
	if _, err := admin("kick", "Avril"); err == nil || !strings.Contains(err.Error(), "not online") {
		t.Fatalf("kick offline: %v", err)
	}
	must("deleted account Avril", "forget", "tag:Avril")
	// view_ip, broadcast, ban, unban, the failed kick and forget, newest first, by the sudo user.
	out, err := admin("audit")
	lines := strings.Split(strings.TrimSpace(out), "\n")
	if err != nil || len(lines) != 8 || !strings.Contains(lines[1], "cli:opsadmin") || !strings.Contains(lines[1], "forget") ||
		!strings.Contains(lines[2], "failed: the player is not online") || !strings.Contains(lines[5], "hello pilots") ||
		!strings.Contains(lines[7], "6 entries") || strings.Contains(out, "127.0.0.1") {
		t.Fatalf("audit:\n%s %v", out, err)
	}
	must("1 entries", "audit", "-n", "1")
	if _, err := admin("frobnicate"); err == nil {
		t.Fatal("unknown command accepted")
	}
	if err := runAdmin([]string{"-addr", httpAddr, "-token-file", filepath.Join(dir, "missing"), "players"}, io.Discard); err == nil {
		t.Fatal("missing token file accepted")
	}
	os.WriteFile(filepath.Join(dir, "wrong.token"), []byte(strings.Repeat("0", 64)), 0o600)
	if err := runAdmin([]string{"-addr", httpAddr, "-token-file", filepath.Join(dir, "wrong.token"), "players"}, io.Discard); err == nil ||
		!strings.Contains(err.Error(), "401") {
		t.Fatalf("wrong token: %v", err)
	}
}
