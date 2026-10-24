package lobby

import (
	"cmp"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"slices"
	"strings"
	"time"

	"ac6lobby/internal/wire"
)

// Operator features: bans, kicks, broadcasts and the read-only views used by the admin
// interface (main.go) and the public status page. Everything here runs under Server.mu.

// AC6 contexts used to label sessions (SPA/XLast: GAME_TYPE 0x800A, 0 = RANKED, 1 = STANDARD;
// GAME_MODE 0x800B carries the online mission enum, 0-7).
const (
	ctxGameType = 0x800A
	ctxGameMode = 0x800B
	ac6Title    = "4E4D07D1"
)

// GameModeNames are the AC6 online mission values (XLast enum RP_OnlineMission, which AC6
// also stores in X_CONTEXT_GAME_MODE 0x800B).
var GameModeNames = [8]string{"SIEGE", "ROYALE", "TEAM", "COOP", "SIEGE_DL", "ROYALE_DL", "TEAM_DL", "COOP_DL"}

// GameModeLabels are human-readable names for GameModeNames.
var GameModeLabels = [8]string{"Siege Battle", "Battle Royale", "Team Battle", "Co-op Battle",
	"Siege Battle (DLC)", "Battle Royale (DLC)", "Team Battle (DLC)", "Co-op Battle (DLC)"}

// ErrNotFound and ErrNotOnline are returned by the admin operations.
var (
	ErrNotFound  = errors.New("no such account")
	ErrNotOnline = errors.New("the player is not online")
	ErrNotBanned = errors.New("the account is not banned")
	ErrBadTarget = errors.New("give exactly one of peer_id or gamertag")
)

// --- bans ---

type banRecord struct {
	PK       wire.Key
	PeerID   uint64
	Gamertag string // at the time of the ban (display only)
	Reason   string
	Created  time.Time
	Expires  time.Time // zero = permanent

	lastRefusedLog time.Time
}

type banJSON struct {
	PK       string `json:"pk"`
	PeerID   string `json:"peer_id"`
	Gamertag string `json:"gamertag"`
	Reason   string `json:"reason"`
	Created  int64  `json:"created_unix"`
	Expires  int64  `json:"expires_unix"` // 0 = permanent
}

type bansFile struct {
	Version int       `json:"version"`
	Bans    []banJSON `json:"bans"`
}

// banStore holds bans keyed by the account's public key (the identity), so a ban survives even
// a lost accounts.json.
type banStore struct {
	byPK  map[wire.Key]*banRecord
	dirty bool
}

func newBanStore() *banStore { return &banStore{byPK: map[wire.Key]*banRecord{}} }

const maxBanReason = 200

func (b *banStore) snapshot() bansFile {
	f := bansFile{Version: 1, Bans: make([]banJSON, 0, len(b.byPK))}
	for _, r := range b.byPK {
		j := banJSON{PK: hex.EncodeToString(r.PK[:]), PeerID: hex16(r.PeerID), Gamertag: r.Gamertag,
			Reason: r.Reason, Created: r.Created.Unix()}
		if !r.Expires.IsZero() {
			j.Expires = r.Expires.Unix()
		}
		f.Bans = append(f.Bans, j)
	}
	slices.SortFunc(f.Bans, func(x, y banJSON) int { return cmp.Compare(x.Created, y.Created) })
	return f
}

func (b *banStore) load(path string) error {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var f bansFile
	if err := json.Unmarshal(data, &f); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	for i, j := range f.Bans {
		pk, err := hex.DecodeString(j.PK)
		if err != nil || len(pk) != wire.KeySize {
			return fmt.Errorf("%s: ban %d: bad pk", path, i)
		}
		r := &banRecord{Gamertag: j.Gamertag, Reason: j.Reason, Created: time.Unix(j.Created, 0).UTC()}
		copy(r.PK[:], pk)
		r.PeerID, _ = parseHex16(j.PeerID)
		if j.Expires != 0 {
			r.Expires = time.Unix(j.Expires, 0).UTC()
		}
		b.byPK[r.PK] = r
	}
	return nil
}

// active returns the ban for pk if it exists and has not expired.
func (b *banStore) active(pk wire.Key, now time.Time) *banRecord {
	r := b.byPK[pk]
	if r == nil || !r.Expires.IsZero() && !now.Before(r.Expires) {
		return nil
	}
	return r
}

// expire removes bans whose time is up.
func (b *banStore) expire(now time.Time) []*banRecord {
	var out []*banRecord
	for pk, r := range b.byPK {
		if !r.Expires.IsZero() && !now.Before(r.Expires) {
			delete(b.byPK, pk)
			out = append(out, r)
		}
	}
	if len(out) > 0 {
		b.dirty = true
	}
	return out
}

// bannedHello reports whether a HELLO for pk must be refused, logging it at most once a minute
// per ban.
func (s *Server) bannedHello(pk wire.Key, now time.Time) bool {
	r := s.bans.active(pk, now)
	if r == nil {
		return false
	}
	s.counters.BannedRefused++
	if now.Sub(r.lastRefusedLog) >= time.Minute {
		r.lastRefusedLog = now
		s.log.Info("hello refused: account banned", "peer", hex16(r.PeerID), "gamertag", r.Gamertag)
	}
	return true
}

// --- target resolution ---

// Target names an account for an admin operation: exactly one of PeerID (16 hex) or Gamertag
// (case-insensitive).
type Target struct {
	PeerID   string `json:"peer_id,omitempty"`
	Gamertag string `json:"gamertag,omitempty"`
}

func (t Target) String() string {
	if t.PeerID != "" {
		return "peer_id " + t.PeerID
	}
	return "gamertag " + t.Gamertag
}

func (s *Server) resolve(t Target) (*Account, error) {
	switch {
	case (t.PeerID == "") == (t.Gamertag == ""):
		return nil, ErrBadTarget
	case t.PeerID != "":
		id, ok := parseHex16(strings.ToLower(strings.TrimSpace(t.PeerID)))
		if !ok {
			return nil, fmt.Errorf("peer_id must be 16 hex digits")
		}
		if a := s.accounts.byPeer[id]; a != nil {
			return a, nil
		}
	default:
		if a := s.accounts.byTag[strings.ToLower(strings.TrimSpace(t.Gamertag))]; a != nil {
			return a, nil
		}
	}
	return nil, ErrNotFound
}

// disconnect sends NOTICE(code, msg) to the account's live registration and removes it, plus any
// replaced boot that still hosts sessions. It returns whether the account was online.
func (s *Server) disconnect(a *Account, code, msg string) bool {
	online := false
	if c := s.clients[a.PeerID]; c != nil {
		online = true
		s.sendS2C(c, noticeInner(code, msg))
		s.goOffline(c)
	}
	for r := range s.retired {
		if r.acct == a {
			s.goOffline(r)
		}
	}
	return online
}

// Kick disconnects an online player with a "kick" NOTICE. The player may register again at
// once; use Ban with a duration to keep them out.
func (s *Server) Kick(t Target, reason string) (PlayerInfo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	a, err := s.resolve(t)
	if err != nil {
		return PlayerInfo{}, err
	}
	c := s.clients[a.PeerID]
	if c == nil {
		return PlayerInfo{}, ErrNotOnline
	}
	info := s.playerInfo(c, s.now(), false)
	s.disconnect(a, "kick", noticeText("You were removed from the server", reason))
	s.counters.Kicks++
	s.log.Info("admin kick", "peer", hex16(a.PeerID), "gamertag", a.Gamertag, "reason", reason)
	return info, nil
}

// BanInfo describes a ban for the admin interface.
type BanInfo struct {
	PeerID    string `json:"peer_id"`
	Gamertag  string `json:"gamertag"`
	Reason    string `json:"reason"`
	Created   string `json:"created"`
	Expires   string `json:"expires,omitempty"` // RFC 3339; empty = permanent
	WasOnline bool   `json:"was_online,omitempty"`
}

func (r *banRecord) info() BanInfo {
	b := BanInfo{PeerID: hex16(r.PeerID), Gamertag: r.Gamertag, Reason: r.Reason,
		Created: r.Created.UTC().Format(time.RFC3339)}
	if !r.Expires.IsZero() {
		b.Expires = r.Expires.UTC().Format(time.RFC3339)
	}
	return b
}

// Ban bans an account (permanently when d is 0), disconnects it if online and persists the ban
// at once. Banning an already banned account replaces its reason and expiry.
func (s *Server) Ban(t Target, reason string, d time.Duration) (BanInfo, error) {
	if d < 0 {
		return BanInfo{}, fmt.Errorf("negative duration")
	}
	if len(reason) > maxBanReason {
		reason = reason[:maxBanReason]
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	a, err := s.resolve(t)
	if err != nil {
		return BanInfo{}, err
	}
	now := s.now()
	r := &banRecord{PK: a.PK, PeerID: a.PeerID, Gamertag: a.Gamertag, Reason: reason,
		Created: now.UTC().Truncate(time.Second)}
	if d > 0 {
		r.Expires = now.Add(d).UTC().Truncate(time.Second)
	}
	s.bans.byPK[a.PK] = r
	s.bans.dirty = true
	info := r.info()
	info.WasOnline = s.disconnect(a, "banned", noticeText("You are banned from this server", reason))
	s.log.Info("admin ban", "peer", hex16(a.PeerID), "gamertag", a.Gamertag, "reason", reason,
		"expires", info.Expires, "was_online", info.WasOnline)
	s.persistBans()
	return info, nil
}

// Unban lifts a ban. A gamertag is looked up among accounts first, then among the names
// recorded in bans.
func (s *Server) Unban(t Target) (BanInfo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var r *banRecord
	if a, err := s.resolve(t); err == nil {
		r = s.bans.byPK[a.PK]
	} else if !errors.Is(err, ErrNotFound) {
		return BanInfo{}, err
	}
	if r == nil {
		for _, b := range s.bans.byPK {
			if t.PeerID != "" && hex16(b.PeerID) == strings.ToLower(strings.TrimSpace(t.PeerID)) ||
				t.Gamertag != "" && strings.EqualFold(b.Gamertag, strings.TrimSpace(t.Gamertag)) {
				r = b
				break
			}
		}
	}
	if r == nil {
		return BanInfo{}, ErrNotBanned
	}
	delete(s.bans.byPK, r.PK)
	s.bans.dirty = true
	s.log.Info("admin unban", "peer", hex16(r.PeerID), "gamertag", r.Gamertag)
	s.persistBans()
	return r.info(), nil
}

// Bans lists the active bans, oldest first.
func (s *Server) Bans() []BanInfo {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	f := s.bans.snapshot()
	out := make([]BanInfo, 0, len(f.Bans))
	for _, j := range f.Bans {
		pk, _ := hex.DecodeString(j.PK)
		var k wire.Key
		copy(k[:], pk)
		if r := s.bans.active(k, now); r != nil {
			out = append(out, r.info())
		}
	}
	return out
}

// ForgetInfo describes a deleted account.
type ForgetInfo struct {
	PeerID    string `json:"peer_id"`
	Gamertag  string `json:"gamertag"`
	XUID      string `json:"xuid"`
	StatsRows int    `json:"stats_rows"`
	WasOnline bool   `json:"was_online"`
	Banned    bool   `json:"banned"` // the ban (if any) is kept, so the key stays banned
}

// Forget deletes an account and its leaderboard rows (for example on a deletion request). The
// player is disconnected; if their game connects again it gets a new account. A ban on the
// key is kept; lift it with Unban first to forget it too.
func (s *Server) Forget(t Target) (ForgetInfo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	a, err := s.resolve(t)
	if err != nil {
		return ForgetInfo{}, err
	}
	info := ForgetInfo{PeerID: hex16(a.PeerID), Gamertag: a.Gamertag, XUID: hex16(a.XUID),
		Banned: s.bans.byPK[a.PK] != nil}
	info.WasOnline = s.disconnect(a, "kick", "Your account was deleted from this server")
	for id, sess := range s.sessions {
		if sess.Host == a {
			delete(s.sessions, id)
		} else if i := sess.memberIndex(a.XUID); i >= 0 {
			sess.Members = append(sess.Members[:i], sess.Members[i+1:]...)
		}
	}
	info.StatsRows = s.stats.removeXUID(a.XUID)
	s.accounts.remove(a)
	s.log.Info("admin forget", "peer", info.PeerID, "stats_rows", info.StatsRows, "was_online", info.WasOnline)
	s.persist(true)
	return info, nil
}

// Broadcast sends NOTICE {"code":"info"} with msg to every online client and returns how many
// it reached. Clients log the message (they do not show it in game).
func (s *Server) Broadcast(msg string) int {
	if len(msg) > 300 {
		msg = msg[:300]
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.clients {
		s.sendS2C(c, noticeInner("info", msg))
	}
	s.log.Info("admin broadcast", "clients", len(s.clients), "msg", msg)
	return len(s.clients)
}

func noticeText(base, reason string) string {
	if reason == "" {
		return base
	}
	return base + ": " + reason
}

// persistBans hands the bans to their writer at once (no debounce: a ban must survive a crash).
func (s *Server) persistBans() {
	if !s.bans.dirty {
		return
	}
	s.bans.dirty = false
	if s.bansW != nil {
		s.bansW.submit(s.bans.snapshot(), 0)
	}
}

// --- views ---

// PlayerInfo is one online player in the admin listing.
type PlayerInfo struct {
	PeerID     string `json:"peer_id"`
	Gamertag   string `json:"gamertag"`
	XUID       string `json:"xuid"`
	VIP        string `json:"vip"`
	Client     string `json:"client"`
	Privacy    bool   `json:"privacy"`
	OldKey     bool   `json:"old_server_key"` // pinned the previous server key (rotation grace)
	OnlineS    int64  `json:"online_s"`
	IdleS      int64  `json:"idle_s"`
	Hosting    int    `json:"hosting"`
	Peers      int    `json:"peers"`
	RelayPkts  uint64 `json:"relay_pkts"`
	RelayBytes uint64 `json:"relay_bytes"`
	Addr       string `json:"addr,omitempty"` // only when asked for: real addresses are personal data
}

func (s *Server) playerInfo(c *Client, now time.Time, withAddr bool) PlayerInfo {
	p := PlayerInfo{PeerID: hex16(c.acct.PeerID), Gamertag: c.acct.Gamertag, XUID: hex16(c.acct.XUID),
		VIP: ipString(c.acct.VIP), Client: c.clientVer, Privacy: c.privacy, OldKey: c.oldKey,
		OnlineS: int64(now.Sub(c.since) / time.Second), IdleS: int64(now.Sub(c.lastSeen) / time.Second),
		Peers: len(c.introduced), RelayPkts: c.relayedPkts, RelayBytes: c.relayedBytes}
	for _, sess := range s.sessions {
		if sess.Host == c.acct && sess.HostBoot == c.boot {
			p.Hosting++
		}
	}
	if withAddr {
		p.Addr = c.addr.String()
	}
	return p
}

// Players lists the online players by gamertag. withAddr adds their registered address.
func (s *Server) Players(withAddr bool) []PlayerInfo {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	out := make([]PlayerInfo, 0, len(s.clients))
	for _, c := range s.clients {
		out = append(out, s.playerInfo(c, now, withAddr))
	}
	slices.SortFunc(out, func(x, y PlayerInfo) int {
		return cmp.Compare(strings.ToLower(x.Gamertag), strings.ToLower(y.Gamertag))
	})
	return out
}

// SessionInfo is one session in the admin listing.
type SessionInfo struct {
	SessionID     string   `json:"session_id"`
	Title         string   `json:"title"`
	Host          string   `json:"host"`
	HostPeerID    string   `json:"host_peer_id"`
	HostOnline    bool     `json:"host_online"`
	Mode          int      `json:"mode"` // 0x800B value, -1 when absent
	ModeName      string   `json:"mode_name,omitempty"`
	ModeLabel     string   `json:"mode_label,omitempty"` // GameModeLabels, e.g. "Battle Royale"
	Type          string   `json:"type,omitempty"`       // "ranked" or "standard" (0x800A)
	State         string   `json:"state"`
	Flags         uint32   `json:"flags"`
	PublicSlots   uint32   `json:"public_slots"`
	PrivateSlots  uint32   `json:"private_slots"`
	FilledPublic  uint32   `json:"filled_public"`
	FilledPrivate uint32   `json:"filled_private"`
	Members       []string `json:"members"` // gamertags (XUID when unknown)
	AgeS          int64    `json:"age_s"`
	IdleS         int64    `json:"idle_s"`
}

// sessionKind returns the AC6 mode (-1 if none) and game type ("ranked", "standard" or "").
func sessionKind(sess *Session) (mode int, typ string) {
	mode = -1
	if sess.Title != ac6Title {
		return mode, ""
	}
	for _, c := range sess.Contexts {
		switch c.ID {
		case ctxGameMode:
			mode = int(min(c.Value, 1<<20))
		case ctxGameType:
			switch c.Value {
			case 0:
				typ = "ranked"
			case 1:
				typ = "standard"
			}
		}
	}
	return mode, typ
}

// Sessions lists the sessions, newest first.
func (s *Server) Sessions() []SessionInfo {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	list := make([]*Session, 0, len(s.sessions))
	for _, sess := range s.sessions {
		list = append(list, sess)
	}
	slices.SortFunc(list, func(x, y *Session) int { return cmp.Compare(y.seq, x.seq) })
	out := make([]SessionInfo, 0, len(list))
	for _, sess := range list {
		pub, priv := sess.filled()
		mode, typ := sessionKind(sess)
		si := SessionInfo{SessionID: hex16(sess.ID), Title: sess.Title, Host: sess.Host.Gamertag,
			HostPeerID: hex16(sess.Host.PeerID), HostOnline: s.hostLive(sess), Mode: mode, Type: typ,
			State: sess.State.String(), Flags: sess.Flags, PublicSlots: sess.PublicSlots,
			PrivateSlots: sess.PrivateSlots, FilledPublic: pub, FilledPrivate: priv,
			Members: make([]string, 0, len(sess.Members)),
			AgeS:    int64(now.Sub(sess.Created) / time.Second), IdleS: int64(now.Sub(sess.Updated) / time.Second)}
		if mode >= 0 && mode < len(GameModeNames) {
			si.ModeName, si.ModeLabel = GameModeNames[mode], GameModeLabels[mode]
		}
		for _, m := range sess.Members {
			if a := s.accounts.byXUID[m.XUID]; a != nil {
				si.Members = append(si.Members, a.Gamertag)
			} else {
				si.Members = append(si.Members, hex16(m.XUID))
			}
		}
		out = append(out, si)
	}
	return out
}

// ModeCount is one row of the public per-mode session counts.
type ModeCount struct {
	Mode     int    `json:"mode"`
	Name     string `json:"name"`
	Label    string `json:"label"`
	Ranked   int    `json:"ranked"`
	Standard int    `json:"standard"`
	Players  int    `json:"players"` // members of these sessions
}

// SessionCounts is the public per-mode breakdown: counts only, no names or addresses.
type SessionCounts struct {
	Total   int         `json:"total"`
	Lobby   int         `json:"lobby"`
	InGame  int         `json:"ingame"`
	Players int         `json:"players"` // members of all sessions
	Modes   []ModeCount `json:"modes"`   // AC6 modes 0-7, always all eight
	Other   int         `json:"other"`   // sessions without a known AC6 mode
}

// SessionCounts returns the public per-mode session counts.
func (s *Server) SessionCounts() SessionCounts {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := SessionCounts{Modes: make([]ModeCount, len(GameModeNames))}
	for i := range out.Modes {
		out.Modes[i] = ModeCount{Mode: i, Name: GameModeNames[i], Label: GameModeLabels[i]}
	}
	for _, sess := range s.sessions {
		out.Total++
		if sess.State == stateInGame {
			out.InGame++
		} else {
			out.Lobby++
		}
		n := len(sess.Members)
		out.Players += n
		mode, typ := sessionKind(sess)
		if mode < 0 || mode >= len(GameModeNames) {
			out.Other++
			continue
		}
		m := &out.Modes[mode]
		m.Players += n
		if typ == "ranked" {
			m.Ranked++
		} else {
			m.Standard++
		}
	}
	return out
}

// Metrics is the admin metrics document: counts, cumulative packet counters and process figures.
type Metrics struct {
	Status
	Counters
	Bans         int `json:"bans"`
	Retired      int `json:"retired"`
	OnlineOldKey int `json:"online_old_key"` // online clients that pin the previous server key
}

// Metrics returns the current counts and cumulative counters.
func (s *Server) Metrics() Metrics {
	s.mu.Lock()
	defer s.mu.Unlock()
	return Metrics{
		Status: Status{Online: len(s.clients), Sessions: len(s.sessions), Accounts: s.accounts.count(),
			UptimeS: int64(s.now().Sub(s.started) / time.Second)},
		Counters: s.counters, Bans: len(s.bans.byPK), Retired: len(s.retired),
		OnlineOldKey: s.onlineOldKey(),
	}
}

func (s *Server) onlineOldKey() int {
	n := 0
	for _, c := range s.clients {
		if c.oldKey {
			n++
		}
	}
	return n
}
