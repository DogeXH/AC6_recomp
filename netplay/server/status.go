package main

import (
	"encoding/json"
	"html/template"
	"net/http"
	"time"

	"ac6lobby/internal/lobby"
)

// The public status page carries counts only: no addresses, gamertags, XUIDs or session IDs.
//
//	GET /status           {"online","sessions","accounts","uptime_s"}
//	GET /status/sessions  per-mode session counts (lobby.SessionCounts)
//	GET /                 a small HTML page with both

func statusServer(addr string, srv *lobby.Server) *http.Server {
	mux := http.NewServeMux()
	mux.HandleFunc("/status", func(w http.ResponseWriter, r *http.Request) {
		if !getOnly(w, r) {
			return
		}
		writeJSON(w, http.StatusOK, srv.Status())
	})
	mux.HandleFunc("/status/sessions", func(w http.ResponseWriter, r *http.Request) {
		if !getOnly(w, r) {
			return
		}
		writeJSON(w, http.StatusOK, srv.SessionCounts())
	})
	mux.HandleFunc("/{$}", func(w http.ResponseWriter, r *http.Request) {
		if !getOnly(w, r) {
			return
		}
		st, counts := srv.Status(), srv.SessionCounts()
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		statusPage.Execute(w, map[string]any{"S": st, "C": counts, "Uptime": (time.Duration(st.UptimeS) * time.Second).String()})
	})
	return &http.Server{
		Addr: addr, Handler: mux,
		ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second,
		WriteTimeout: 10 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 8 << 10,
	}
}

func getOnly(w http.ResponseWriter, r *http.Request) bool {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return false
	}
	return true
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

var statusPage = template.Must(template.New("status").Parse(`<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="30">
<title>AC6 lobby status</title>
<style>
:root{--bg:#f6f7f9;--fg:#1b1f24;--muted:#5d6672;--line:#d9dde3;--card:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#121417;--fg:#e6e8eb;--muted:#9aa3ad;--line:#2a2f36;--card:#1a1d21}}
body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif}
main{max-width:640px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px}
p{margin:0 0 16px;color:var(--muted)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px;margin-bottom:20px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px}
.tile b{display:block;font-size:22px;font-variant-numeric:tabular-nums}
.tile span{color:var(--muted);font-size:13px}
table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:8px}
th,td{padding:7px 10px;border-bottom:1px solid var(--line);text-align:right;font-variant-numeric:tabular-nums}
th:first-child,td:first-child{text-align:left}
th{font-size:13px;color:var(--muted);font-weight:600}
tr:last-child td{border-bottom:0}
</style></head><body><main>
<h1>AC6 lobby status</h1>
<p>Counts only. Refreshes every 30 s. Up {{.Uptime}}.</p>
<div class="tiles">
<div class="tile"><b>{{.S.Online}}</b><span>pilots online</span></div>
<div class="tile"><b>{{.S.Sessions}}</b><span>sessions</span></div>
<div class="tile"><b>{{.C.Lobby}}</b><span>in lobby</span></div>
<div class="tile"><b>{{.C.InGame}}</b><span>in battle</span></div>
</div>
<table><thead><tr><th>Mode</th><th>Standard</th><th>Ranked</th><th>Pilots</th></tr></thead><tbody>
{{range .C.Modes}}<tr><td>{{.Label}}</td><td>{{.Standard}}</td><td>{{.Ranked}}</td><td>{{.Players}}</td></tr>
{{end}}{{if .C.Other}}<tr><td>Other</td><td colspan="2">{{.C.Other}}</td><td></td></tr>{{end}}
</tbody></table>
</main></body></html>
`))
