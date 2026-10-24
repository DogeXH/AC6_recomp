package main

import (
	"crypto/rand"
	"crypto/subtle"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"net"
	"net/http"
	"os"
	"slices"
	"strconv"
	"strings"
	"time"

	"ac6lobby/internal/lobby"
)

// The admin interface is HTTP on a loopback address, behind a bearer token read from a file.
//
//	GET  /admin/v1/players[?addr=1[&peer_id=X]]  online players (addresses only with addr=1;
//	                                              peer_id limits the list to one player)
//	GET  /admin/v1/sessions           sessions with host, mode, type, state and members
//	GET  /admin/v1/bans               active bans
//	GET  /admin/v1/metrics            counts, cumulative counters and process figures
//	GET  /admin/v1/audit[?limit=200]  admin actions, newest first
//	POST /admin/v1/kick       {"peer_id"|"gamertag", "reason"}
//	POST /admin/v1/ban        {"peer_id"|"gamertag", "reason", "duration"}  ("" = permanent, e.g. "24h")
//	POST /admin/v1/unban      {"peer_id"|"gamertag", "reason"}
//	POST /admin/v1/broadcast  {"msg"}
//	POST /admin/v1/forget     {"peer_id"|"gamertag", "reason"}  delete the account and its leaderboard rows
//	GET  /admin/ui/           the moderation panel (static page; it asks for the token)
//
// Every request must name a loopback host (127.0.0.1, [::1] or localhost), which stops DNS
// rebinding, and browsers' cross-origin POSTs are refused (http.CrossOriginProtection), so a
// page on another site cannot drive the API through an SSH tunnel. Kicks, bans, unbans,
// broadcasts, forgets and address lookups are written to the audit log (audit.go).

const (
	adminPrefix = "/admin/v1/"
	adminUIPath = "/admin/ui/"
)

//go:embed adminui
var adminUIFiles embed.FS

// loopbackAddr checks that addr is host:port with a loopback host ("localhost" becomes
// 127.0.0.1).
func loopbackAddr(addr string) (string, error) {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return "", fmt.Errorf("-admin %q: %w", addr, err)
	}
	if host == "localhost" {
		host = "127.0.0.1"
	}
	ip := net.ParseIP(host)
	if ip == nil || !ip.IsLoopback() {
		return "", fmt.Errorf("-admin %q: the admin interface binds to a loopback address only (127.0.0.1 or [::1])", addr)
	}
	return net.JoinHostPort(host, port), nil
}

// loadOrCreateToken reads the admin token (at least 32 characters) or creates the file with a
// random 64-hex token and mode 0600.
func loadOrCreateToken(path string, log *slog.Logger) ([]byte, error) {
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		var raw [32]byte
		if _, err := rand.Read(raw[:]); err != nil {
			return nil, err
		}
		tok := hex.EncodeToString(raw[:])
		if err := lobby.WriteFileAtomic(path, []byte(tok+"\n"), 0o600); err != nil {
			return nil, err
		}
		log.Info("generated an admin token", "file", path)
		return []byte(tok), nil
	}
	if err != nil {
		return nil, err
	}
	if info, err := os.Stat(path); err == nil && info.Mode().Perm()&0o077 != 0 {
		log.Warn("admin token file is readable by other users; run chmod 600", "file", path)
	}
	tok := strings.TrimSpace(string(b))
	if len(tok) < 32 {
		return nil, fmt.Errorf("%s: the admin token must be at least 32 characters", path)
	}
	return []byte(tok), nil
}

type adminAPI struct {
	srv     *lobby.Server
	token   []byte
	tokenID string // fingerprint for the audit log
	audit   *auditLog
	log     *slog.Logger
}

// adminServer serves the admin API and the panel. audit may be nil (no audit log).
func adminServer(srv *lobby.Server, token []byte, audit *auditLog, log *slog.Logger) *http.Server {
	a := &adminAPI{srv: srv, token: token, tokenID: tokenFingerprint(token), audit: audit, log: log}
	api := http.NewServeMux()
	api.HandleFunc("GET "+adminPrefix+"players", a.players)
	api.HandleFunc("GET "+adminPrefix+"sessions", a.sessions)
	api.HandleFunc("GET "+adminPrefix+"bans", a.bans)
	api.HandleFunc("GET "+adminPrefix+"metrics", a.metrics)
	api.HandleFunc("GET "+adminPrefix+"audit", a.auditList)
	api.HandleFunc("POST "+adminPrefix+"kick", a.kick)
	api.HandleFunc("POST "+adminPrefix+"ban", a.ban)
	api.HandleFunc("POST "+adminPrefix+"unban", a.unban)
	api.HandleFunc("POST "+adminPrefix+"broadcast", a.broadcast)
	api.HandleFunc("POST "+adminPrefix+"forget", a.forget)

	ui, err := fs.Sub(adminUIFiles, "adminui")
	if err != nil {
		panic(err)
	}
	toUI := func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, adminUIPath, http.StatusFound) }
	mux := http.NewServeMux()
	mux.Handle(adminPrefix, a.auth(api))
	mux.Handle("GET "+adminUIPath, uiHeaders(http.StripPrefix(adminUIPath, http.FileServerFS(ui))))
	mux.HandleFunc("GET /admin/{$}", toUI)
	mux.HandleFunc("GET /{$}", toUI)
	return &http.Server{
		Handler:           adminGuard(mux),
		ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second,
		WriteTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 8 << 10,
	}
}

// adminGuard refuses requests for a non-loopback host name (DNS rebinding: a page on
// evil.example whose name now points at 127.0.0.1) and browsers' cross-origin state-changing
// requests (Sec-Fetch-Site, or Origin against Host). Clients without those headers, such as
// the CLI, pass; they still need the token.
func adminGuard(next http.Handler) http.Handler {
	cop := http.NewCrossOriginProtection()
	cop.SetDenyHandler(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "cross-origin request refused"})
	}))
	h := cop.Handler(next)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		if !loopbackHost(r.Host) {
			writeJSON(w, http.StatusForbidden, map[string]string{
				"error": "open the admin interface as http://127.0.0.1:<port>/ or http://localhost:<port>/"})
			return
		}
		h.ServeHTTP(w, r)
	})
}

// loopbackHost reports whether a Host header names localhost or a loopback address.
func loopbackHost(hostport string) bool {
	host := hostport
	if h, _, err := net.SplitHostPort(hostport); err == nil {
		host = h
	}
	host = strings.TrimSuffix(strings.TrimPrefix(host, "["), "]")
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

// uiPolicy lets the panel load only its own script and style and talk only to this origin.
const uiPolicy = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; " +
	"img-src 'self' data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"

func uiHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", uiPolicy)
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Set("Cross-Origin-Resource-Policy", "same-origin")
		next.ServeHTTP(w, r)
	})
}

func (a *adminAPI) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		got, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
		if !ok || subtle.ConstantTimeCompare([]byte(strings.TrimSpace(got)), a.token) != 1 {
			w.Header().Set("WWW-Authenticate", `Bearer realm="ac6lobby-admin"`)
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "missing or wrong admin token"})
			return
		}
		if r.Method == http.MethodPost && !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
			writeJSON(w, http.StatusUnsupportedMediaType, map[string]string{"error": "send application/json"})
			return
		}
		next.ServeHTTP(w, r)
	})
}

// record writes an audit entry for r. A non-nil err marks the action as failed; the target is
// then what the request named.
func (a *adminAPI) record(r *http.Request, e auditEntry, t lobby.Target, err error) {
	a.who(r, &e)
	e.Reason = cleanText(e.Reason, 300)
	e.OK = err == nil
	if err != nil {
		e.Error = err.Error()
		if e.PeerID == "" && e.Gamertag == "" {
			e.PeerID, e.Gamertag = cleanText(t.PeerID, 32), cleanText(t.Gamertag, 64)
		}
	}
	a.audit.record(e)
}

func (a *adminAPI) players(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	withAddr := q.Get("addr") == "1"
	list := a.srv.Players(withAddr)
	if id := strings.ToLower(strings.TrimSpace(q.Get("peer_id"))); id != "" {
		list = slices.DeleteFunc(list, func(p lobby.PlayerInfo) bool { return p.PeerID != id })
		if withAddr {
			e := auditEntry{Action: "view_ip", PeerID: cleanText(id, 32)}
			var err error
			if len(list) == 0 {
				err = lobby.ErrNotOnline
			} else {
				e.Gamertag = list[0].Gamertag
			}
			a.record(r, e, lobby.Target{}, err)
		}
	} else if withAddr {
		a.record(r, auditEntry{Action: "view_ip", Detail: fmt.Sprintf("all online players (%d)", len(list))},
			lobby.Target{}, nil)
	}
	writeJSON(w, http.StatusOK, map[string]any{"players": list})
}

func (a *adminAPI) sessions(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"sessions": a.srv.Sessions()})
}

func (a *adminAPI) bans(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"bans": a.srv.Bans()})
}

func (a *adminAPI) metrics(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"server": a.srv.Metrics(), "process": readProcStats(),
		"version": version})
}

func (a *adminAPI) auditList(w http.ResponseWriter, r *http.Request) {
	n := 200
	if v := r.URL.Query().Get("limit"); v != "" {
		var err error
		if n, err = strconv.Atoi(v); err != nil || n < 1 {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "limit must be a positive number"})
			return
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"audit": a.audit.recent(min(n, maxAuditList))})
}

type adminReq struct {
	lobby.Target
	Reason   string `json:"reason"`
	Duration string `json:"duration"`
	Msg      string `json:"msg"`
}

func decodeAdmin(w http.ResponseWriter, r *http.Request) (adminReq, bool) {
	var req adminReq
	dec := json.NewDecoder(io.LimitReader(r.Body, 8<<10))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad request body: " + err.Error()})
		return req, false
	}
	return req, true
}

func adminError(w http.ResponseWriter, err error) {
	code := http.StatusBadRequest
	switch {
	case errors.Is(err, lobby.ErrNotFound), errors.Is(err, lobby.ErrNotBanned):
		code = http.StatusNotFound
	case errors.Is(err, lobby.ErrNotOnline):
		code = http.StatusConflict
	}
	writeJSON(w, code, map[string]string{"error": err.Error()})
}

func (a *adminAPI) kick(w http.ResponseWriter, r *http.Request) {
	req, ok := decodeAdmin(w, r)
	if !ok {
		return
	}
	p, err := a.srv.Kick(req.Target, req.Reason)
	a.record(r, auditEntry{Action: "kick", PeerID: p.PeerID, Gamertag: p.Gamertag, Reason: req.Reason}, req.Target, err)
	if err != nil {
		adminError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"kicked": p})
}

func (a *adminAPI) ban(w http.ResponseWriter, r *http.Request) {
	req, ok := decodeAdmin(w, r)
	if !ok {
		return
	}
	e := auditEntry{Action: "ban", Reason: req.Reason, Duration: "permanent"}
	var d time.Duration
	if req.Duration != "" {
		e.Duration = cleanText(req.Duration, 32)
		var err error
		if d, err = parseDuration(req.Duration); err != nil || d <= 0 {
			err = fmt.Errorf("bad duration %q (examples: 30m, 24h, 7d)", req.Duration)
			a.record(r, e, req.Target, err)
			adminError(w, err)
			return
		}
	}
	b, err := a.srv.Ban(req.Target, req.Reason, d)
	e.PeerID, e.Gamertag, e.Expires = b.PeerID, b.Gamertag, b.Expires
	if b.WasOnline {
		e.Detail = "was online; disconnected"
	}
	a.record(r, e, req.Target, err)
	if err != nil {
		adminError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"banned": b})
}

func (a *adminAPI) unban(w http.ResponseWriter, r *http.Request) {
	req, ok := decodeAdmin(w, r)
	if !ok {
		return
	}
	b, err := a.srv.Unban(req.Target)
	a.record(r, auditEntry{Action: "unban", PeerID: b.PeerID, Gamertag: b.Gamertag, Reason: req.Reason}, req.Target, err)
	if err != nil {
		adminError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"unbanned": b})
}

func (a *adminAPI) forget(w http.ResponseWriter, r *http.Request) {
	req, ok := decodeAdmin(w, r)
	if !ok {
		return
	}
	f, err := a.srv.Forget(req.Target)
	// A deleted account's gamertag leaves the audit log too: this entry and the earlier ones
	// keep only the peer ID.
	e := auditEntry{Action: "forget", PeerID: f.PeerID, Reason: req.Reason}
	if err == nil {
		e.Detail = fmt.Sprintf("deleted the account and %d leaderboard rows", f.StatsRows)
		if n := a.audit.forgetGamertag(f.PeerID, f.Gamertag); n > 0 {
			e.Detail += fmt.Sprintf("; gamertag removed from %d audit entries", n)
		}
		if f.Banned {
			e.Detail += "; its ban is kept"
		}
	}
	a.record(r, e, req.Target, err)
	if err != nil {
		adminError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"forgotten": f})
}

func (a *adminAPI) broadcast(w http.ResponseWriter, r *http.Request) {
	req, ok := decodeAdmin(w, r)
	if !ok {
		return
	}
	if strings.TrimSpace(req.Msg) == "" {
		adminError(w, errors.New("msg is empty"))
		return
	}
	n := a.srv.Broadcast(req.Msg)
	a.record(r, auditEntry{Action: "broadcast", Message: cleanText(req.Msg, 300),
		Detail: fmt.Sprintf("sent to %d clients", n)}, lobby.Target{}, nil)
	writeJSON(w, http.StatusOK, map[string]any{"sent": n})
}

// parseDuration accepts Go durations plus a "d" (days) suffix, e.g. "7d" or "1d12h".
func parseDuration(s string) (time.Duration, error) {
	s = strings.TrimSpace(s)
	days := time.Duration(0)
	if i := strings.IndexByte(s, 'd'); i > 0 {
		var n int
		if _, err := fmt.Sscanf(s[:i], "%d", &n); err != nil || n < 0 || fmt.Sprint(n) != s[:i] {
			return 0, fmt.Errorf("bad duration %q", s)
		}
		days = time.Duration(n) * 24 * time.Hour
		s = s[i+1:]
		if s == "" {
			return days, nil
		}
	}
	d, err := time.ParseDuration(s)
	if err != nil {
		return 0, err
	}
	return days + d, nil
}
