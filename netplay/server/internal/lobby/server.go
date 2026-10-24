// Package lobby implements the ac6lobby server: registration (HELLO), the encrypted control
// channel, RPC, accounts, sessions, stats, introductions and the relay (PROTOCOL.md).
package lobby

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"ac6lobby/internal/wire"
)

// Timing and limits (PROTOCOL.md sections 2-3 plus DoS hygiene).
const (
	livenessTimeout   = 30 * time.Second
	introResendEvery  = 500 * time.Millisecond
	introMaxResends   = 3
	tickInterval      = 100 * time.Millisecond
	sweepInterval     = time.Second
	limiterSweepEvery = 10 * time.Second

	accountsDebounce = 500 * time.Millisecond
	statsDebounce    = 2 * time.Second

	maxClients = 100000
)

// Config configures a Server.
type Config struct {
	SecretKey wire.Key // server static X25519 secret
	// PrevSecretKeys are earlier server keys still accepted for HELLO during a key rotation, so
	// clients that pin the old public key keep working until they update.
	PrevSecretKeys []wire.Key
	DataDir        string // accounts.json and stats.json; empty = in-memory only (tests)
	Logger         *slog.Logger
	MOTD           string
	// Now is the clock; nil = time.Now. Tests inject a fake clock.
	Now func() time.Time
	// Limits overrides the per-IP limits; zero fields keep the defaults.
	Limits Limits
	// LogAddresses adds client IP addresses to warnings (rate-limit hits). Off by default:
	// addresses are personal data. Debug-level logs always carry them.
	LogAddresses bool
}

// Limits are the per-source-IP admission limits.
type Limits struct {
	HelloPerSecond       float64 // default 10 per second per IP
	HelloBurst           float64 // default 20
	AccountsPerIPPerHour float64 // default 20
}

func (l Limits) withDefaults() Limits {
	if l.HelloPerSecond == 0 {
		l.HelloPerSecond = 10
	}
	if l.HelloBurst == 0 {
		l.HelloBurst = 20
	}
	if l.AccountsPerIPPerHour == 0 {
		l.AccountsPerIPPerHour = 20
	}
	return l
}

// Status is the public /status document: counts only.
type Status struct {
	Online   int   `json:"online"`
	Sessions int   `json:"sessions"`
	Accounts int   `json:"accounts"`
	UptimeS  int64 `json:"uptime_s"`
}

type packetConn interface {
	WriteToUDPAddrPort(b []byte, addr netip.AddrPort) (int, error)
}

// Server is one lobby instance. All state is guarded by mu; packet handling and timers run
// under it, and the UDP socket is written while holding it.
type Server struct {
	cfg     Config
	log     *slog.Logger
	sk, pk  wire.Key
	prev    []serverKey // accepted previous keys (rotation grace)
	boot    wire.Boot
	started time.Time
	// startMono is the real process start (monotonic), independent of cfg.Now.
	startMono time.Time
	lastBase  uint64

	mu       sync.Mutex
	conn     packetConn
	accounts *accountStore
	stats    *statsStore
	bans     *banStore
	clients  map[uint64]*Client  // current registration per peer_id
	retired  map[*Client]bool    // replaced boots that still host sessions
	sessions map[uint64]*Session // by session id
	seq      uint64

	helloLimiter   *keyedLimiter[netip.Addr]
	helloGlobal    *bucket
	accountLimiter *keyedLimiter[netip.Addr]
	rehelloLimiter *keyedLimiter[netip.AddrPort]

	accountsDirtyAt time.Time
	statsDirtyAt    time.Time
	accountsW       *jsonWriter
	statsW          *jsonWriter
	bansW           *jsonWriter
	// New accounts get their first HELLO_ACK only once accounts.json holds them, so a crash
	// cannot hand the same key a different identity later.
	accountsWritten uint64
	pendingAcks     []*Client

	lastSweep, lastLimiterSweep time.Time
	counters                    Counters

	accountLimitLogged time.Time // last "new-account limit" warning (at most one a minute)
	accountLimitHits   int       // hits since that warning

	notified int // clients sent the shutdown NOTICE by Close

	closeOnce sync.Once
	closed    chan struct{}
	wg        sync.WaitGroup
}

type serverKey struct{ sk, pk wire.Key }

// Counters are cumulative server-wide counters (for logs, metrics and tests).
type Counters struct {
	Hellos        uint64 `json:"hellos"`         // authenticated HELLOs
	RPCs          uint64 `json:"rpcs"`           // RPC requests executed or refused (not cache hits)
	Intros        uint64 `json:"intros"`         // introductions made
	Relayed       uint64 `json:"relayed"`        // RELAY packets forwarded
	RelayedBytes  uint64 `json:"relayed_bytes"`  // bytes of the forwarded RELAY datagrams
	RelayDropped  uint64 `json:"relay_dropped"`  // RELAY packets dropped (unauthorized or over the limit)
	Malformed     uint64 `json:"malformed"`      // undecodable or unauthenticated packets
	Rehellos      uint64 `json:"rehellos"`       // REHELLO replies sent
	BannedRefused uint64 `json:"banned_refused"` // HELLOs refused because the account is banned
	Kicks         uint64 `json:"kicks"`          // admin kicks
}

// New creates a server and loads persisted state from cfg.DataDir.
func New(cfg Config) (*Server, error) {
	if cfg.Logger == nil {
		cfg.Logger = slog.New(slog.NewTextHandler(os.Stderr, nil))
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	lim := cfg.Limits.withDefaults()
	s := &Server{
		cfg: cfg, log: cfg.Logger, sk: cfg.SecretKey, pk: wire.PublicKey(cfg.SecretKey),
		accounts: newAccountStore(), stats: newStatsStore(), bans: newBanStore(),
		clients: map[uint64]*Client{}, retired: map[*Client]bool{}, sessions: map[uint64]*Session{},
		helloLimiter:   newKeyedLimiter[netip.Addr](lim.HelloPerSecond, lim.HelloBurst, 65536),
		accountLimiter: newKeyedLimiter[netip.Addr](lim.AccountsPerIPPerHour/3600, lim.AccountsPerIPPerHour, 65536),
		rehelloLimiter: newKeyedLimiter[netip.AddrPort](2, 2, 65536),
		closed:         make(chan struct{}),
	}
	now := cfg.Now()
	s.started, s.lastSweep, s.lastLimiterSweep = now, now, now
	s.startMono = time.Now()
	for _, k := range cfg.PrevSecretKeys {
		if k != cfg.SecretKey {
			s.prev = append(s.prev, serverKey{k, wire.PublicKey(k)})
		}
	}
	s.helloGlobal = newBucket(500, 1000, now)
	if _, err := rand.Read(s.boot[:]); err != nil {
		return nil, err
	}
	if cfg.DataDir != "" {
		if err := s.accounts.load(filepath.Join(cfg.DataDir, "accounts.json")); err != nil {
			return nil, err
		}
		if err := s.stats.load(filepath.Join(cfg.DataDir, "stats.json")); err != nil {
			return nil, err
		}
		if err := s.bans.load(filepath.Join(cfg.DataDir, "bans.json")); err != nil {
			return nil, err
		}
		s.accountsW = newJSONWriter(filepath.Join(cfg.DataDir, "accounts.json"), s.log, s.accountsPersisted)
		s.statsW = newJSONWriter(filepath.Join(cfg.DataDir, "stats.json"), s.log, s.statsPersisted)
		s.bansW = newJSONWriter(filepath.Join(cfg.DataDir, "bans.json"), s.log, nil)
	}
	s.wg.Add(1)
	go s.timerLoop()
	return s, nil
}

// PublicKey returns the server's static public key.
func (s *Server) PublicKey() wire.Key { return s.pk }

// PrevPublicKeys returns the public keys of the previous keys still accepted.
func (s *Server) PrevPublicKeys() []wire.Key {
	out := make([]wire.Key, 0, len(s.prev))
	for _, k := range s.prev {
		out = append(out, k.pk)
	}
	return out
}

// openWithPrevKeys tries to authenticate a HELLO under each previous server key. It returns the
// plaintext, the c2s key, the DH value and the server public key that matched.
func (s *Server) openWithPrevKeys(f wire.HelloFields, ad, sealed []byte) (pt []byte, c2s, dh, spk wire.Key, ok bool) {
	for _, k := range s.prev {
		d, err := wire.DH(k.sk, f.ClientPK)
		if err != nil {
			continue
		}
		key := wire.C2SKey(d, f.ClientPK, k.pk, f.Boot)
		if p, err := wire.Open(key, f.Ctr, ad, sealed); err == nil {
			return p, key, d, k.pk, true
		}
	}
	return nil, c2s, dh, spk, false
}

// Boot returns this process's boot ID.
func (s *Server) Boot() wire.Boot { return s.boot }

// counterBase returns a starting s2c counter: 2^20 per millisecond of process uptime
// (monotonic clock), and strictly above the previous base.
func (s *Server) counterBase() uint64 {
	base := uint64(time.Since(s.startMono).Milliseconds()+1) << 20
	if base <= s.lastBase {
		base = s.lastBase + 1<<20
	}
	s.lastBase = base
	return base
}

func (s *Server) dhFor(pk wire.Key) (wire.Key, error) { return wire.DH(s.sk, pk) }

func (s *Server) now() time.Time { return s.cfg.Now() }

// Serve reads datagrams from conn until the socket is closed. The timer loop runs from New
// until Close.
func (s *Server) Serve(conn *net.UDPConn) error {
	s.mu.Lock()
	s.conn = conn
	s.mu.Unlock()

	buf := make([]byte, 2048)
	for {
		n, from, err := conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			select {
			case <-s.closed:
				return nil
			default:
			}
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			s.log.Warn("udp read", "err", err)
			time.Sleep(10 * time.Millisecond) // never spin on a persistent socket error
			continue
		}
		from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
		s.mu.Lock()
		s.handlePacket(buf[:n], from)
		s.mu.Unlock()
	}
}

func (s *Server) timerLoop() {
	defer s.wg.Done()
	t := time.NewTicker(tickInterval)
	defer t.Stop()
	for {
		select {
		case <-s.closed:
			return
		case <-t.C:
			s.mu.Lock()
			s.tick()
			s.mu.Unlock()
		}
	}
}

// Close sends a shutdown NOTICE to every online client, stops the timers and writes any
// pending state. The caller closes the UDP socket afterwards.
func (s *Server) Close() {
	s.closeOnce.Do(func() {
		close(s.closed)
		s.wg.Wait()
		s.mu.Lock()
		if s.conn != nil {
			for _, c := range s.clients {
				s.sendS2C(c, noticeInner("shutdown", "server shutting down"))
			}
			s.notified = len(s.clients)
		}
		s.persist(true)
		s.persistBans()
		s.mu.Unlock()
		if s.accountsW != nil {
			s.accountsW.close()
			s.statsW.close()
			s.bansW.close()
		}
	})
}

// Notified returns how many clients Close sent the shutdown NOTICE to.
func (s *Server) Notified() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.notified
}

// Status returns the public counts.
func (s *Server) Status() Status {
	s.mu.Lock()
	defer s.mu.Unlock()
	return Status{Online: len(s.clients), Sessions: len(s.sessions), Accounts: s.accounts.count(),
		UptimeS: int64(s.now().Sub(s.started) / time.Second)}
}

// Stats returns a copy of the packet counters.
func (s *Server) Stats() Counters {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.counters
}

// --- packet dispatch ---

func (s *Server) handlePacket(b []byte, from netip.AddrPort) {
	if len(b) < 2 || len(b) > wire.MaxDatagram || b[0] != wire.Magic {
		s.counters.Malformed++
		return
	}
	switch b[1] {
	case wire.TypeHello:
		s.handleHello(b, from)
	case wire.TypeC2S:
		s.handleC2S(b, from)
	case wire.TypeRelay:
		s.handleRelay(b, from)
	default:
		s.counters.Malformed++
	}
}

func (s *Server) send(b []byte, to netip.AddrPort) {
	if s.conn == nil || len(b) > wire.MaxDatagram {
		return
	}
	if _, err := s.conn.WriteToUDPAddrPort(b, to); err != nil {
		s.log.Debug("udp write", "err", err)
	}
}

// sendS2C encrypts inner for c and sends it to c's registered address.
func (s *Server) sendS2C(c *Client, inner []byte) {
	c.sendCtr++
	s.send(wire.BuildServerPacket(wire.TypeS2C, s.boot, c.sendCtr, c.s2cKey, inner), c.addr)
}

func noticeInner(code, msg string) []byte {
	return append([]byte{wire.KindNotice}, mustJSON(map[string]string{"code": code, "msg": msg})...)
}

// --- timers ---

func (s *Server) tick() {
	now := s.now()
	s.resendIntros(now)
	if now.Sub(s.lastSweep) >= sweepInterval {
		s.lastSweep = now
		s.sweep(now)
	}
	if now.Sub(s.lastLimiterSweep) >= limiterSweepEvery {
		s.lastLimiterSweep = now
		s.helloLimiter.sweep(now)
		s.accountLimiter.sweep(now)
		s.rehelloLimiter.sweep(now)
	}
	s.persist(false)
}

// sweep applies liveness, the session idle timeout and RPC cache expiry.
func (s *Server) sweep(now time.Time) {
	for _, c := range s.clients {
		if now.Sub(c.lastSeen) > livenessTimeout {
			s.log.Info("client offline", "peer", hex16(c.acct.PeerID), "reason", "timeout")
			s.goOffline(c)
		} else {
			c.expireRPC(now)
		}
	}
	for c := range s.retired {
		if now.Sub(c.lastSeen) > livenessTimeout {
			s.goOffline(c)
		}
	}
	for id, sess := range s.sessions {
		if now.Sub(sess.Updated) > sessionIdleTimeout {
			s.log.Info("session expired", "session", hex16(id))
			delete(s.sessions, id)
		}
	}
	for _, r := range s.bans.expire(now) {
		s.log.Info("ban expired", "peer", hex16(r.PeerID), "gamertag", r.Gamertag)
	}
	s.persistBans()
}

// goOffline removes a registration: deletes the sessions its boot hosts, drops the account
// from every session when it has no other live registration, and forgets introductions.
func (s *Server) goOffline(c *Client) {
	for id, sess := range s.sessions {
		if sess.Host == c.acct && sess.HostBoot == c.boot {
			delete(s.sessions, id)
		}
	}
	c.acct.rememberBoot(c.boot, c.replay.Highest(), c.sendCtr)
	if s.clients[c.acct.PeerID] == c {
		delete(s.clients, c.acct.PeerID)
		for _, sess := range s.sessions {
			if i := sess.memberIndex(c.acct.XUID); i >= 0 {
				sess.Members = append(sess.Members[:i], sess.Members[i+1:]...)
			}
		}
	}
	delete(s.retired, c)
	for peer, boot := range c.introduced {
		if other := s.clients[peer]; other != nil && other.boot == boot {
			if b, ok := other.introduced[c.acct.PeerID]; ok && b == c.boot {
				delete(other.introduced, c.acct.PeerID)
			}
		}
	}
	c.introduced = nil
	c.pendingIntros = nil
}

// hostsSessions reports whether c's boot hosts any session.
func (s *Server) hostsSessions(c *Client) bool {
	for _, sess := range s.sessions {
		if sess.Host == c.acct && sess.HostBoot == c.boot {
			return true
		}
	}
	return false
}

// persist hands snapshots to the writers once the debounce delay passed (or at once if force).
func (s *Server) persist(force bool) {
	if s.accountsW == nil {
		s.accounts.dirty, s.stats.dirty = false, false
		return
	}
	now := s.now()
	if s.accounts.dirty {
		if s.accountsDirtyAt.IsZero() {
			s.accountsDirtyAt = now
		}
		// New accounts waiting for their HELLO_ACK skip the debounce.
		if force || len(s.pendingAcks) > 0 || now.Sub(s.accountsDirtyAt) >= accountsDebounce {
			s.accountsW.submit(s.accounts.snapshot(), s.accounts.gen)
			s.accounts.dirty, s.accountsDirtyAt = false, time.Time{}
		}
	}
	if s.stats.dirty {
		if s.statsDirtyAt.IsZero() {
			s.statsDirtyAt = now
		}
		if force || now.Sub(s.statsDirtyAt) >= statsDebounce {
			s.statsW.submit(s.stats.snapshot(), 0)
			s.stats.dirty, s.statsDirtyAt = false, time.Time{}
		}
	}
}

// accountsPersisted runs on the accounts writer goroutine after each write attempt. It sends
// the HELLO_ACKs that waited for their account to be durable. After a failed write it sends
// them anyway (and logs it): a client that never registers is worse than a lost account.
func (s *Server) accountsPersisted(gen uint64, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err == nil {
		s.accountsWritten = max(s.accountsWritten, gen)
	}
	keep := s.pendingAcks[:0]
	for _, c := range s.pendingAcks {
		if c.acct.gen > s.accountsWritten && err == nil {
			keep = append(keep, c)
			continue
		}
		c.ackPending = false
		if s.clients[c.acct.PeerID] == c {
			s.sendHelloAck(c)
		}
	}
	clear(s.pendingAcks[len(keep):])
	s.pendingAcks = keep
}

// statsPersisted runs on the stats writer goroutine after each write attempt. A failed write
// (disk full, say) marks the stats dirty again, so the next persist tick retries it after the
// debounce instead of leaving the newest stats only in memory until some later write.
func (s *Server) statsPersisted(_ uint64, err error) {
	if err == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.stats.dirty = true
}

// LoadOrCreateKey reads <dir>/server.key (64 hex chars) or creates it with mode 0600, and
// writes <dir>/server.pub. It returns the secret key.
func LoadOrCreateKey(dir string, log *slog.Logger) (wire.Key, error) {
	var sk wire.Key
	path := filepath.Join(dir, "server.key")
	data, err := os.ReadFile(path)
	switch {
	case err == nil:
		if info, err := os.Stat(path); err == nil && info.Mode().Perm()&0o077 != 0 {
			log.Warn("server.key is readable by other users; run chmod 600", "mode", fmt.Sprintf("%o", info.Mode().Perm()))
		}
		k, ok := parseKeyHex(string(data))
		if !ok {
			return sk, fmt.Errorf("%s: expected 64 hex characters", path)
		}
		sk = k
	case errors.Is(err, os.ErrNotExist):
		if _, err := rand.Read(sk[:]); err != nil {
			return sk, err
		}
		if err := WriteFileAtomic(path, []byte(fmt.Sprintf("%x\n", sk[:])), 0o600); err != nil {
			return sk, err
		}
		log.Info("generated a new server key", "file", path)
	default:
		return sk, err
	}
	pk := wire.PublicKey(sk)
	if err := WriteFileAtomic(filepath.Join(dir, "server.pub"), []byte(fmt.Sprintf("%x\n", pk[:])), 0o644); err != nil {
		return sk, err
	}
	return sk, nil
}

// PrevKeyFile is the file holding the previous server secret key during a rotation grace period.
const PrevKeyFile = "server.prev.key"

// LoadPrevKeys reads <dir>/server.prev.key if it exists: one 64-hex secret key per line. The
// server accepts HELLOs under these keys too.
func LoadPrevKeys(dir string) ([]wire.Key, error) {
	path := filepath.Join(dir, PrevKeyFile)
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var keys []wire.Key
	for i, line := range strings.Split(string(data), "\n") {
		if strings.TrimSpace(line) == "" {
			continue
		}
		k, ok := parseKeyHex(line)
		if !ok {
			return nil, fmt.Errorf("%s line %d: expected 64 hex characters", path, i+1)
		}
		keys = append(keys, k)
	}
	return keys, nil
}

func parseKeyHex(s string) (wire.Key, bool) {
	var k wire.Key
	b, err := hex.DecodeString(strings.TrimSpace(s))
	if err != nil || len(b) != wire.KeySize {
		return k, false
	}
	copy(k[:], b)
	return k, true
}
