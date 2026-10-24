package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"slices"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"ac6lobby/internal/lobby"
)

// The audit log records every admin action in <data>/audit.jsonl, one JSON object per line,
// mode 0600. It holds no IP addresses: a "view_ip" entry records that an address was looked at,
// not the address. Entries older than the retention period (-audit-days) are dropped at start
// and whenever a new entry is written, and the file keeps at most auditMaxEntries entries.
// Deleting an account (forget) removes its gamertag from the log; the peer ID stays.

const (
	auditFileName   = "audit.jsonl"
	auditMaxEntries = 5000
	auditMaxLine    = 64 << 10 // longer lines are skipped as damaged
	maxAuditList    = 1000     // most entries one GET /admin/v1/audit returns
)

// auditEntry is one admin action. Who did it is what the client says (via, operator) plus a
// fingerprint of the token it used.
type auditEntry struct {
	Time     time.Time `json:"time"`
	Via      string    `json:"via"`                // "cli", "panel" or "api"
	Operator string    `json:"operator,omitempty"` // the CLI's OS user (SUDO_USER or USER)
	Token    string    `json:"token"`              // first 8 hex digits of SHA-256(admin token)
	Action   string    `json:"action"`             // kick, ban, unban, broadcast, forget, view_ip
	PeerID   string    `json:"peer_id,omitempty"`
	Gamertag string    `json:"gamertag,omitempty"`
	Reason   string    `json:"reason,omitempty"`
	Duration string    `json:"duration,omitempty"` // ban: "24h", "7d", ... or "permanent"
	Expires  string    `json:"expires,omitempty"`  // ban: RFC 3339
	Message  string    `json:"message,omitempty"`  // broadcast text
	Detail   string    `json:"detail,omitempty"`   // outcome, e.g. "sent to 3 clients"
	OK       bool      `json:"ok"`
	Error    string    `json:"error,omitempty"`
}

type auditLog struct {
	mu      sync.Mutex
	path    string
	keep    time.Duration // 0 = no age limit
	now     func() time.Time
	log     *slog.Logger
	entries []auditEntry // oldest first
}

// openAudit loads the audit log at path (a missing file is an empty log) and drops expired
// entries.
func openAudit(path string, keep time.Duration, log *slog.Logger) (*auditLog, error) {
	a := &auditLog{path: path, keep: keep, now: time.Now, log: log}
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return a, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()
	if info, err := f.Stat(); err == nil && info.Mode().Perm()&0o077 != 0 {
		log.Warn("audit log is readable by other users; run chmod 600", "file", path)
	}
	rd := bufio.NewReader(f)
	bad := 0
	for {
		line, err := rd.ReadBytes('\n')
		if line = bytes.TrimSpace(line); len(line) > 0 {
			var e auditEntry
			if len(line) > auditMaxLine || json.Unmarshal(line, &e) != nil || e.Time.IsZero() {
				bad++
			} else {
				a.entries = append(a.entries, e)
			}
		}
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
	}
	if bad > 0 {
		log.Warn("skipped damaged audit log lines; they are dropped at the next rewrite", "file", path, "lines", bad)
	}
	if a.prune() {
		if err := a.rewrite(); err != nil {
			return nil, err
		}
	}
	return a, nil
}

// prune drops entries past the retention period and, above auditMaxEntries, the oldest ones
// (down to 90% of the cap, so a full log is not rewritten on every action).
func (a *auditLog) prune() bool {
	n := 0
	if a.keep > 0 {
		cut := a.now().Add(-a.keep)
		for n < len(a.entries) && a.entries[n].Time.Before(cut) {
			n++
		}
	}
	if len(a.entries)-n > auditMaxEntries {
		n = len(a.entries) - auditMaxEntries*9/10
	}
	if n == 0 {
		return false
	}
	a.entries = slices.Clone(a.entries[n:])
	return true
}

func (a *auditLog) rewrite() error {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	for _, e := range a.entries {
		if err := enc.Encode(e); err != nil {
			return err
		}
	}
	return lobby.WriteFileAtomic(a.path, buf.Bytes(), 0o600)
}

func (a *auditLog) appendLine(e auditEntry) error {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(e); err != nil {
		return err
	}
	f, err := os.OpenFile(a.path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o600)
	if err != nil {
		return err
	}
	if _, err := f.Write(buf.Bytes()); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// record stamps e with the time and writes it. A write error is logged, not returned: the
// action it describes has already happened.
func (a *auditLog) record(e auditEntry) {
	if a == nil {
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	e.Time = a.now().UTC().Truncate(time.Second)
	a.entries = append(a.entries, e)
	var err error
	if a.prune() {
		err = a.rewrite()
	} else {
		err = a.appendLine(e)
	}
	if err != nil {
		a.log.Error("audit log write failed", "file", a.path, "action", e.Action, "err", err)
	}
}

// forgetGamertag blanks the gamertag of a deleted account in every entry for its peer ID, and
// in entries without a peer ID that name the same gamertag (failed attempts), then rewrites the
// file if any changed. It returns how many entries changed.
func (a *auditLog) forgetGamertag(peerID, gamertag string) int {
	if a == nil || peerID == "" {
		return 0
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	n := 0
	for i := range a.entries {
		e := &a.entries[i]
		if e.Gamertag != "" && (e.PeerID == peerID || e.PeerID == "" && strings.EqualFold(e.Gamertag, gamertag)) {
			e.Gamertag = ""
			n++
		}
	}
	if n > 0 {
		if err := a.rewrite(); err != nil {
			a.log.Error("audit log rewrite failed", "file", a.path, "err", err)
		}
	}
	return n
}

// recent returns up to n entries, newest first.
func (a *auditLog) recent(n int) []auditEntry {
	out := []auditEntry{}
	if a == nil {
		return out
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	for i := len(a.entries) - 1; i >= 0 && len(out) < n; i-- {
		out = append(out, a.entries[i])
	}
	return out
}

// Clients name themselves with these headers; the values go into the audit log as given.
const (
	hdrAdminClient   = "X-AC6-Admin-Client"   // "cli" or "panel"
	hdrAdminOperator = "X-AC6-Admin-Operator" // free text, e.g. the CLI's OS user
)

func tokenFingerprint(token []byte) string {
	sum := sha256.Sum256(token)
	return hex.EncodeToString(sum[:4])
}

// who fills in the actor fields of an audit entry from the request.
func (a *adminAPI) who(r *http.Request, e *auditEntry) {
	switch v := r.Header.Get(hdrAdminClient); v {
	case "cli", "panel":
		e.Via = v
	default:
		e.Via = "api"
	}
	e.Operator = cleanText(r.Header.Get(hdrAdminOperator), 64)
	e.Token = a.tokenID
}

// cleanText turns tabs and line breaks into spaces, drops other control characters, trims s
// and cuts it to max bytes on a rune boundary.
func cleanText(s string, max int) string {
	s = strings.TrimSpace(strings.Map(func(r rune) rune {
		switch {
		case r == '\t' || r == '\n' || r == '\r':
			return ' '
		case r < 0x20 || r == 0x7f:
			return -1
		}
		return r
	}, s))
	for len(s) > max {
		_, size := utf8.DecodeLastRuneInString(s)
		s = s[:len(s)-size]
	}
	return s
}
