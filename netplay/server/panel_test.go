package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"ac6lobby/internal/lobby"
)

const testAdminOrigin = "http://127.0.0.1:47802"

// sendAdmin sends one request to h. hdr holds extra headers ("Host" sets the request's host).
func sendAdmin(h http.Handler, method, path, token, body string, hdr map[string]string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, testAdminOrigin+path, strings.NewReader(body))
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	for k, v := range hdr {
		if k == "Host" {
			req.Host = v
		} else {
			req.Header.Set(k, v)
		}
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func TestAdminPanelServed(t *testing.T) {
	srv, _ := udpServer(t)
	h := adminServer(srv, []byte(strings.Repeat("cd", 32)), nil, quietLog()).Handler

	rec := sendAdmin(h, "GET", "/admin/ui/", "", "", nil)
	if rec.Code != 200 || !strings.HasPrefix(rec.Header().Get("Content-Type"), "text/html") ||
		!strings.Contains(rec.Body.String(), "AC6 lobby") || !strings.Contains(rec.Body.String(), `src="app.js"`) {
		t.Fatalf("panel page: %d %q", rec.Code, rec.Header().Get("Content-Type"))
	}
	csp := rec.Header().Get("Content-Security-Policy")
	if !strings.Contains(csp, "script-src 'self'") || !strings.Contains(csp, "frame-ancestors 'none'") ||
		strings.Contains(csp, "unsafe-inline") || rec.Header().Get("X-Frame-Options") != "DENY" ||
		rec.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("panel headers: %v", rec.Header())
	}
	for path, ctype := range map[string]string{"/admin/ui/app.js": "javascript", "/admin/ui/app.css": "text/css"} {
		if rec := sendAdmin(h, "GET", path, "", "", nil); rec.Code != 200 || !strings.Contains(rec.Header().Get("Content-Type"), ctype) {
			t.Fatalf("%s: %d %q", path, rec.Code, rec.Header().Get("Content-Type"))
		}
	}
	for _, path := range []string{"/", "/admin/", "/admin/ui"} {
		if rec := sendAdmin(h, "GET", path, "", "", nil); rec.Code/100 != 3 || !strings.HasSuffix(rec.Header().Get("Location"), "/admin/ui/") {
			t.Fatalf("%s: %d %q", path, rec.Code, rec.Header().Get("Location"))
		}
	}
	if rec := sendAdmin(h, "GET", "/admin/ui/missing.js", "", "", nil); rec.Code != 404 {
		t.Fatalf("missing file: %d", rec.Code)
	}
	if rec := sendAdmin(h, "POST", "/admin/ui/", "", "", nil); rec.Code != 405 && rec.Code != 403 {
		t.Fatalf("POST to the panel: %d", rec.Code)
	}
	// The page is public on the loopback listener; the API behind it is not.
	if rec := sendAdmin(h, "GET", "/admin/v1/players", "", "", nil); rec.Code != 401 {
		t.Fatalf("API without a token: %d", rec.Code)
	}
	// Loopback host names in any form are fine; anything else (DNS rebinding) is refused.
	for _, host := range []string{"127.0.0.1:47802", "localhost:9000", "[::1]:47802", "LOCALHOST", "127.0.0.1"} {
		if rec := sendAdmin(h, "GET", "/admin/ui/", "", "", map[string]string{"Host": host}); rec.Code != 200 {
			t.Fatalf("host %s: %d", host, rec.Code)
		}
	}
	for _, host := range []string{"evil.example:47802", "evil.example", "10.0.0.5:47802", "localhost.evil.example:47802", ""} {
		if rec := sendAdmin(h, "GET", "/admin/ui/", "", "", map[string]string{"Host": host}); rec.Code != 403 {
			t.Fatalf("host %q: %d", host, rec.Code)
		}
	}
}

func TestAdminCrossOrigin(t *testing.T) {
	srv, addr := udpServer(t)
	register(t, srv, addr, "Pixy")
	dir := t.TempDir()
	audit, err := openAudit(filepath.Join(dir, auditFileName), 0, quietLog())
	if err != nil {
		t.Fatal(err)
	}
	tok := strings.Repeat("ef", 32)
	h := adminServer(srv, []byte(tok), audit, quietLog()).Handler
	ban := `{"gamertag":"Pixy","reason":"x","duration":"1h"}`

	refused := []map[string]string{
		{"Origin": "http://evil.example", "Sec-Fetch-Site": "cross-site"},
		{"Origin": "http://evil.example"},                                  // older browser: no Sec-Fetch-Site
		{"Origin": "http://localhost:8080", "Sec-Fetch-Site": "same-site"}, // another local port
		{"Origin": "null", "Sec-Fetch-Site": "cross-site"},
		{"Sec-Fetch-Site": "cross-site"},
		{"Origin": "http://127.0.0.1:9999"},
		// DNS rebinding: evil.example now resolves to 127.0.0.1, so the browser calls it same-origin.
		{"Host": "evil.example:47802", "Origin": "http://evil.example:47802", "Sec-Fetch-Site": "same-origin"},
	}
	for _, hdr := range refused {
		for _, path := range []string{"/admin/v1/ban", "/admin/v1/broadcast", "/admin/v1/forget"} {
			body := ban
			if path == "/admin/v1/broadcast" {
				body = `{"msg":"hi"}`
			}
			if rec := sendAdmin(h, "POST", path, tok, body, hdr); rec.Code != 403 {
				t.Fatalf("%s with %v: %d %s", path, hdr, rec.Code, rec.Body)
			}
		}
	}
	if len(srv.Bans()) != 0 || srv.Status().Accounts != 1 || len(audit.recent(10)) != 0 {
		t.Fatalf("a refused request acted: bans %v status %+v audit %v", srv.Bans(), srv.Status(), audit.recent(10))
	}
	// Reads stay possible cross-origin only with the token, and the browser cannot read the
	// answer (no CORS headers).
	if rec := sendAdmin(h, "GET", "/admin/v1/players", "", "", map[string]string{"Origin": "http://evil.example",
		"Sec-Fetch-Site": "cross-site"}); rec.Code != 401 || rec.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatalf("cross-origin read: %d %v", rec.Code, rec.Header())
	}
	// The panel itself: same origin, through a tunnel on any local port.
	for _, hdr := range []map[string]string{
		{"Origin": testAdminOrigin, "Sec-Fetch-Site": "same-origin"},
		{"Host": "localhost:9000", "Origin": "http://localhost:9000", "Sec-Fetch-Site": "same-origin"},
		{"Origin": testAdminOrigin}, // no Sec-Fetch-Site: Origin matches Host
	} {
		if rec := sendAdmin(h, "POST", "/admin/v1/broadcast", tok, `{"msg":"hi"}`, hdr); rec.Code != 200 {
			t.Fatalf("same-origin POST %v: %d %s", hdr, rec.Code, rec.Body)
		}
	}
	if rec := sendAdmin(h, "POST", "/admin/v1/ban", tok, ban, map[string]string{"Origin": testAdminOrigin,
		"Sec-Fetch-Site": "same-origin"}); rec.Code != 200 || len(srv.Bans()) != 1 {
		t.Fatalf("same-origin ban: %d %s", rec.Code, rec.Body)
	}
	// A forged-looking but correct token still needs to be the right one.
	if rec := sendAdmin(h, "POST", "/admin/v1/unban", strings.Repeat("0", 64), `{"gamertag":"Pixy"}`,
		map[string]string{"Origin": testAdminOrigin, "Sec-Fetch-Site": "same-origin"}); rec.Code != 401 {
		t.Fatalf("same-origin POST with a wrong token: %d", rec.Code)
	}
}

func readAuditFile(t *testing.T, path string) []map[string]any {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var out []map[string]any
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		var m map[string]any
		if err := json.Unmarshal(sc.Bytes(), &m); err != nil {
			t.Fatalf("audit line %q: %v", sc.Text(), err)
		}
		out = append(out, m)
	}
	return out
}

func TestAuditEntries(t *testing.T) {
	srv, addr := udpServer(t)
	c := register(t, srv, addr, "Pixy")
	dir := t.TempDir()
	path := filepath.Join(dir, auditFileName)
	audit, err := openAudit(path, 90*24*time.Hour, quietLog())
	if err != nil {
		t.Fatal(err)
	}
	tok := strings.Repeat("12", 32)
	h := adminServer(srv, []byte(tok), audit, quietLog()).Handler
	panel := map[string]string{hdrAdminClient: "panel", "Origin": testAdminOrigin, "Sec-Fetch-Site": "same-origin"}
	do := func(method, p, body string, want int) map[string]any {
		t.Helper()
		rec := sendAdmin(h, method, p, tok, body, panel)
		if rec.Code != want {
			t.Fatalf("%s %s: %d %s", method, p, rec.Code, rec.Body)
		}
		var doc map[string]any
		json.Unmarshal(rec.Body.Bytes(), &doc)
		return doc
	}
	peer := c.Ack().PeerID

	// Reading lists is not audited; looking up an address is, without the address.
	do("GET", "/admin/v1/players", "", 200)
	doc := do("GET", "/admin/v1/players?addr=1&peer_id="+strings.ToUpper(peer), "", 200)
	if ps := doc["players"].([]any); len(ps) != 1 || !strings.HasPrefix(ps[0].(map[string]any)["addr"].(string), "127.0.0.1:") {
		t.Fatalf("one player's address: %v", doc)
	}
	if ps := do("GET", "/admin/v1/players?peer_id=0000000000000001", "", 200)["players"].([]any); len(ps) != 0 {
		t.Fatalf("filter by an unknown peer: %v", ps)
	}
	do("POST", "/admin/v1/kick", `{"gamertag":"Pixy","reason":"spam"}`, 200)
	do("POST", "/admin/v1/ban", `{"gamertag":"Nobody","reason":"typo","duration":"24h"}`, 404)
	do("POST", "/admin/v1/ban", `{"peer_id":"`+peer+`","reason":"griefing","duration":"7d"}`, 200)
	do("POST", "/admin/v1/ban", `{"gamertag":"Pixy","reason":"x","duration":"soon"}`, 400)
	do("POST", "/admin/v1/unban", `{"gamertag":"Pixy","reason":"appeal accepted"}`, 200)
	do("POST", "/admin/v1/broadcast", `{"msg":"Restarting\nin 5 minutes"}`, 200)
	do("POST", "/admin/v1/forget", `{"gamertag":"Pixy","reason":"deletion request"}`, 200)
	// Not audited: refused before any action.
	sendAdmin(h, "POST", "/admin/v1/ban", "", `{"gamertag":"Pixy"}`, panel)
	sendAdmin(h, "POST", "/admin/v1/ban", tok, `{"gamertag":"Pixy"}`, map[string]string{"Origin": "http://evil.example"})
	do("POST", "/admin/v1/ban", `{"gamertag":"Pixy","bogus":1}`, 400)
	// A different client, as the CLI names itself.
	if rec := sendAdmin(h, "POST", "/admin/v1/broadcast", tok, `{"msg":"from the cli"}`,
		map[string]string{hdrAdminClient: "cli", hdrAdminOperator: "cole\x1b[31m"}); rec.Code != 200 {
		t.Fatalf("cli broadcast: %d", rec.Code)
	}
	sendAdmin(h, "POST", "/admin/v1/broadcast", tok, `{"msg":"curl"}`, map[string]string{hdrAdminClient: "root"})

	lines := readAuditFile(t, path)
	type want struct {
		action, gamertag, reason string
		ok                       bool
	}
	// forget removed Pixy's gamertag from the entries for Pixy's peer ID and from the failed ban
	// that named only the gamertag. "Nobody" is another name and stays.
	wants := []want{
		{"view_ip", "", "", true}, {"kick", "", "spam", true}, {"ban", "Nobody", "typo", false},
		{"ban", "", "griefing", true}, {"ban", "", "x", false}, {"unban", "", "appeal accepted", true},
		{"broadcast", "", "", true}, {"forget", "", "deletion request", true}, {"broadcast", "", "", true},
		{"broadcast", "", "", true},
	}
	if len(lines) != len(wants) {
		t.Fatalf("%d audit lines, want %d: %v", len(lines), len(wants), lines)
	}
	fp := tokenFingerprint([]byte(tok))
	for i, w := range wants {
		e := lines[i]
		gt, _ := e["gamertag"].(string)
		reason, _ := e["reason"].(string)
		if e["action"] != w.action || gt != w.gamertag || reason != w.reason || e["ok"] != w.ok || e["token"] != fp {
			t.Fatalf("entry %d: %v, want %+v", i, e, w)
		}
		if _, err := time.Parse(time.RFC3339, e["time"].(string)); err != nil {
			t.Fatalf("entry %d time: %v", i, e["time"])
		}
		if i < 8 && e["via"] != "panel" {
			t.Fatalf("entry %d via %v", i, e["via"])
		}
	}
	if e := lines[0]; e["peer_id"] != peer {
		t.Fatalf("view_ip entry: %v", e)
	}
	if e := lines[2]; e["error"] != lobby.ErrNotFound.Error() {
		t.Fatalf("failed ban entry: %v", e)
	}
	if e := lines[3]; e["peer_id"] != peer || e["duration"] != "7d" || e["expires"] == nil || e["detail"] != nil {
		t.Fatalf("ban entry: %v", e)
	}
	if e := lines[6]; e["message"] != "Restarting in 5 minutes" || e["detail"] != "sent to 0 clients" {
		t.Fatalf("broadcast entry: %v", e)
	}
	if e := lines[7]; e["peer_id"] != peer || e["gamertag"] != nil ||
		!strings.Contains(e["detail"].(string), "gamertag removed from 5 audit entries") {
		t.Fatalf("forget entry keeps the gamertag or lacks the peer ID: %v", e)
	}
	if e := lines[8]; e["via"] != "cli" || e["operator"] != "cole[31m" {
		t.Fatalf("cli entry: %v", e)
	}
	if e := lines[9]; e["via"] != "api" {
		t.Fatalf("unnamed client entry: %v", e)
	}
	raw, _ := os.ReadFile(path)
	if strings.Contains(string(raw), "127.0.0.1") {
		t.Fatal("the audit log holds an IP address")
	}
	if st, _ := os.Stat(path); st.Mode().Perm() != 0o600 {
		t.Fatalf("audit log mode %v", st.Mode())
	}

	// The API lists entries newest first; limit is capped and validated.
	doc = do("GET", "/admin/v1/audit?limit=2", "", 200)
	if es := doc["audit"].([]any); len(es) != 2 || es[0].(map[string]any)["message"] != "curl" ||
		es[1].(map[string]any)["message"] != "from the cli" {
		t.Fatalf("audit list: %v", doc)
	}
	if es := do("GET", "/admin/v1/audit", "", 200)["audit"].([]any); len(es) != len(wants) {
		t.Fatalf("audit default list: %d", len(es))
	}
	do("GET", "/admin/v1/audit?limit=0", "", 400)
	do("GET", "/admin/v1/audit?limit=x", "", 400)
	if rec := sendAdmin(h, "GET", "/admin/v1/audit", "", "", nil); rec.Code != 401 {
		t.Fatalf("audit without a token: %d", rec.Code)
	}

	// Reopening keeps everything.
	again, err := openAudit(path, 90*24*time.Hour, quietLog())
	if err != nil || len(again.recent(100)) != len(wants) {
		t.Fatalf("reopen: %v %d", err, len(again.recent(100)))
	}
}

func TestAuditRetention(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, auditFileName)
	a, err := openAudit(path, 24*time.Hour, quietLog())
	if err != nil {
		t.Fatal(err)
	}
	clock := time.Now().Add(-48 * time.Hour)
	a.now = func() time.Time { return clock }
	a.record(auditEntry{Action: "kick", Gamertag: "Old1"})
	a.record(auditEntry{Action: "kick", Gamertag: "Old2"})
	clock = clock.Add(25 * time.Hour)
	a.record(auditEntry{Action: "ban", Gamertag: "New"})
	if lines := readAuditFile(t, path); len(lines) != 1 || lines[0]["gamertag"] != "New" {
		t.Fatalf("after expiry: %v", lines)
	}
	// At start: kept within the period, dropped past it, damaged lines skipped.
	f, _ := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0)
	fmt.Fprintln(f, `{"not json`)
	fmt.Fprintln(f, `{"action":"kick"}`) // no time
	f.Close()
	b, err := openAudit(path, 24*time.Hour, quietLog())
	if err != nil || len(b.recent(10)) != 1 {
		t.Fatalf("reopen within the period: %v %v", b.recent(10), err)
	}
	if _, err := openAudit(path, time.Hour, quietLog()); err != nil {
		t.Fatal(err)
	}
	if lines := readAuditFile(t, path); len(lines) != 0 {
		t.Fatalf("past the period: %v", lines)
	}
	// No age limit: only the entry cap applies, trimming to 90% of it.
	c, err := openAudit(path, 0, quietLog())
	if err != nil {
		t.Fatal(err)
	}
	for i := range auditMaxEntries + 1 {
		c.entries = append(c.entries, auditEntry{Time: time.Unix(int64(i), 0), Action: "kick"})
	}
	if !c.prune() || len(c.entries) != auditMaxEntries*9/10 || c.entries[len(c.entries)-1].Time.Unix() != auditMaxEntries {
		t.Fatalf("cap: %d entries", len(c.entries))
	}
	if c.prune() {
		t.Fatal("pruned again under the cap")
	}
	var nilLog *auditLog
	nilLog.record(auditEntry{Action: "kick"})
	if got := nilLog.recent(5); got == nil || len(got) != 0 {
		t.Fatalf("nil log recent: %v", got)
	}
}

func TestCleanText(t *testing.T) {
	for in, want := range map[string]string{"  cole ": "cole", "a\x00b\nc\x7f": "ab c", "ééé": "éé", "\x1b[2Jx": "[2Jx"} {
		if got := cleanText(in, 5); got != want {
			t.Errorf("%q: %q, want %q", in, got, want)
		}
	}
}
