package main

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"ac6lobby/internal/lobby"
)

func TestStatusEndpoint(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	sk, err := lobby.LoadOrCreateKey(t.TempDir(), log)
	if err != nil {
		t.Fatal(err)
	}
	srv, err := lobby.New(lobby.Config{SecretKey: sk, Logger: log})
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	h := statusServer("127.0.0.1:0", srv).Handler

	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/status", nil))
	if rec.Code != http.StatusOK || !strings.HasPrefix(rec.Header().Get("Content-Type"), "application/json") {
		t.Fatalf("status %d %q", rec.Code, rec.Header().Get("Content-Type"))
	}
	var doc map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &doc); err != nil {
		t.Fatal(err)
	}
	keys := []string{}
	for k := range doc {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	if strings.Join(keys, ",") != "accounts,online,sessions,uptime_s" {
		t.Fatalf("status keys %v", keys)
	}

	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/status", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST: %d", rec.Code)
	}
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/sessions", nil))
	if rec.Code != http.StatusNotFound {
		t.Fatalf("other path: %d", rec.Code)
	}
}

func TestServerKeyFiles(t *testing.T) {
	dir := t.TempDir()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	sk, err := lobby.LoadOrCreateKey(dir, log)
	if err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(filepath.Join(dir, "server.key"))
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatalf("server.key mode %v %v", info.Mode(), err)
	}
	again, err := lobby.LoadOrCreateKey(dir, log)
	if err != nil || again != sk {
		t.Fatal("key not reloaded")
	}
	srv, err := lobby.New(lobby.Config{SecretKey: sk, Logger: log})
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	pub, _ := os.ReadFile(filepath.Join(dir, "server.pub"))
	pk := srv.PublicKey()
	if strings.TrimSpace(string(pub)) != strings.ToLower(hexOf(pk[:])) {
		t.Fatalf("server.pub %q", pub)
	}
	os.WriteFile(filepath.Join(dir, "server.key"), []byte("nothex"), 0o600)
	if _, err := lobby.LoadOrCreateKey(dir, log); err == nil {
		t.Fatal("bad key file accepted")
	}
}

func hexOf(b []byte) string {
	const d = "0123456789abcdef"
	out := make([]byte, 0, 2*len(b))
	for _, v := range b {
		out = append(out, d[v>>4], d[v&15])
	}
	return string(out)
}
