package lobby

import (
	"cmp"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/netip"
	"os"
	"slices"
	"strconv"
	"strings"
	"time"

	"ac6lobby/internal/wire"
)

// Virtual IP range (ARCHITECTURE.md section 3): 10.64.0.1 up to 10.127.255.254, skipping
// addresses whose last octet is 0 or 255.
const (
	vipFirst uint32 = 0x0A400001
	vipLast  uint32 = 0x0A7FFFFE
	vipMask  uint32 = 0xFFC00000
	vipBase  uint32 = 0x0A400000

	xuidPrefix  uint64 = 0x0009 << 48
	maxGamertag        = 15
)

// Account is a persisted identity: one per client static public key.
type Account struct {
	PeerID    uint64
	PK        wire.Key
	XUID      uint64
	VIP       uint32
	Gamertag  string // final, unique
	Requested string // sanitized name the client last asked for
	Created   time.Time

	dh        wire.Key // cached X25519(server_sk, PK) once dhOK
	dhOK      bool
	gen       uint64    // accountStore.gen when created; durable once a snapshot of it is written
	prevBoots []bootCtr // recent boots that stopped being current, newest last
}

// bootCtr remembers, for a boot that is no longer registered, the highest counter accepted
// from it (so a replayed HELLO cannot take the registration back) and the last s2c counter
// sent to it (so a re-registration of the same boot, which derives the same s2c key, never
// reuses a nonce).
type bootCtr struct {
	boot    wire.Boot
	ctr     uint64
	sendCtr uint64
}

const maxPrevBoots = 8

func (a *Account) rememberBoot(boot wire.Boot, ctr, sendCtr uint64) {
	for i := range a.prevBoots {
		if a.prevBoots[i].boot == boot {
			a.prevBoots[i].ctr = max(a.prevBoots[i].ctr, ctr)
			a.prevBoots[i].sendCtr = max(a.prevBoots[i].sendCtr, sendCtr)
			return
		}
	}
	if len(a.prevBoots) >= maxPrevBoots {
		a.prevBoots = append(a.prevBoots[:0], a.prevBoots[1:]...)
	}
	a.prevBoots = append(a.prevBoots, bootCtr{boot, ctr, sendCtr})
}

// lastSendCtr returns the last s2c counter used for boot by an earlier registration.
func (a *Account) lastSendCtr(boot wire.Boot) uint64 {
	for _, p := range a.prevBoots {
		if p.boot == boot {
			return p.sendCtr
		}
	}
	return 0
}

// staleHello reports whether a HELLO from boot with ctr is a replay from a previous boot.
func (a *Account) staleHello(boot wire.Boot, ctr uint64) bool {
	for _, p := range a.prevBoots {
		if p.boot == boot {
			return ctr <= p.ctr
		}
	}
	return false
}

type accountJSON struct {
	PeerID    string `json:"peer_id"`
	PK        string `json:"pk"`
	XUID      string `json:"xuid"`
	VIP       string `json:"vip"`
	Gamertag  string `json:"gamertag"`
	Requested string `json:"requested"`
	Created   int64  `json:"created_unix"`
}

type accountsFile struct {
	Version  int           `json:"version"`
	NextVIP  string        `json:"next_vip"`
	Accounts []accountJSON `json:"accounts"`
}

// accountStore indexes accounts. It is owned by the server goroutine (under Server.mu).
type accountStore struct {
	byPK    map[wire.Key]*Account
	byPeer  map[uint64]*Account
	byXUID  map[uint64]*Account
	byVIP   map[uint32]*Account
	byTag   map[string]*Account // lower-case gamertag
	nextVIP uint32
	dirty   bool
	gen     uint64 // bumped by every change; a snapshot carries the gen it includes
}

func newAccountStore() *accountStore {
	return &accountStore{
		byPK:    map[wire.Key]*Account{},
		byPeer:  map[uint64]*Account{},
		byXUID:  map[uint64]*Account{},
		byVIP:   map[uint32]*Account{},
		byTag:   map[string]*Account{},
		nextVIP: vipFirst,
	}
}

func (s *accountStore) count() int { return len(s.byPK) }

var errVIPExhausted = errors.New("virtual IP range exhausted")

func validVIP(v uint32) bool {
	last := v & 0xFF
	return v&vipMask == vipBase && v >= vipFirst && v <= vipLast && last != 0 && last != 255
}

func (s *accountStore) allocVIP() (uint32, error) {
	for v := s.nextVIP; v <= vipLast && v >= vipFirst; v++ {
		if !validVIP(v) || s.byVIP[v] != nil {
			continue
		}
		s.nextVIP = v + 1
		return v, nil
	}
	return 0, errVIPExhausted
}

func random64() uint64 {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err) // crypto/rand never fails on supported platforms
	}
	return binary.LittleEndian.Uint64(b[:])
}

// create allocates a new account for pk with the requested gamertag.
func (s *accountStore) create(pk, dh wire.Key, requested string, now time.Time) (*Account, error) {
	vip, err := s.allocVIP()
	if err != nil {
		return nil, err
	}
	a := &Account{PK: pk, VIP: vip, Created: now.UTC().Truncate(time.Second), dh: dh, dhOK: true}
	for a.PeerID == 0 || s.byPeer[a.PeerID] != nil {
		a.PeerID = random64()
	}
	for a.XUID == 0 || s.byXUID[a.XUID] != nil {
		a.XUID = xuidPrefix | random64()&(1<<48-1)
	}
	a.Requested = sanitizeGamertag(requested)
	a.Gamertag = s.uniqueGamertag(a.Requested, nil)
	s.index(a)
	s.dirty = true
	s.gen++
	a.gen = s.gen
	return a, nil
}

// rename gives a a new gamertag when the requested name changed.
func (s *accountStore) rename(a *Account, requested string) bool {
	req := sanitizeGamertag(requested)
	if req == a.Requested {
		return false
	}
	delete(s.byTag, strings.ToLower(a.Gamertag))
	a.Requested = req
	a.Gamertag = s.uniqueGamertag(req, a)
	s.byTag[strings.ToLower(a.Gamertag)] = a
	s.dirty = true
	s.gen++
	return true
}

func (s *accountStore) index(a *Account) {
	s.byPK[a.PK] = a
	s.byPeer[a.PeerID] = a
	s.byXUID[a.XUID] = a
	s.byVIP[a.VIP] = a
	s.byTag[strings.ToLower(a.Gamertag)] = a
}

// sanitizeGamertag keeps A-Z, a-z, 0-9 and single spaces, trimmed, at most 15 characters.
func sanitizeGamertag(s string) string {
	var b strings.Builder
	space := false
	for _, r := range s {
		switch {
		case r >= 'A' && r <= 'Z', r >= 'a' && r <= 'z', r >= '0' && r <= '9':
			if space && b.Len() > 0 {
				b.WriteByte(' ')
			}
			space = false
			b.WriteRune(r)
		case r == ' ':
			space = true
		}
		if b.Len() >= maxGamertag {
			break
		}
	}
	out := strings.TrimSpace(b.String())
	if len(out) > maxGamertag {
		out = strings.TrimSpace(out[:maxGamertag])
	}
	if out == "" {
		out = "Pilot"
	}
	return out
}

// uniqueGamertag returns base, or base with a numeric suffix from 2, that no other account
// uses (case-insensitive). self is the account being renamed (its own name does not count).
func (s *accountStore) uniqueGamertag(base string, self *Account) string {
	free := func(tag string) bool {
		owner := s.byTag[strings.ToLower(tag)]
		return owner == nil || owner == self
	}
	if free(base) {
		return base
	}
	try := func(n int) (string, bool) {
		suffix := strconv.Itoa(n)
		stem := base
		if len(stem)+len(suffix) > maxGamertag {
			stem = strings.TrimSpace(stem[:maxGamertag-len(suffix)])
		}
		tag := stem + suffix
		return tag, free(tag)
	}
	for n := 2; n < 100; n++ {
		if tag, ok := try(n); ok {
			return tag
		}
	}
	// A very common name: random suffixes keep allocation O(1) on average.
	for {
		if tag, ok := try(100 + int(random64()%999900)); ok {
			return tag
		}
	}
}

// --- persistence ---

func (s *accountStore) snapshot() accountsFile {
	f := accountsFile{Version: 1, NextVIP: ipString(s.nextVIP), Accounts: make([]accountJSON, 0, len(s.byPK))}
	for _, a := range s.byPK {
		f.Accounts = append(f.Accounts, accountJSON{
			PeerID: hex16(a.PeerID), PK: hex.EncodeToString(a.PK[:]), XUID: hex16(a.XUID),
			VIP: ipString(a.VIP), Gamertag: a.Gamertag, Requested: a.Requested, Created: a.Created.Unix(),
		})
	}
	// Deterministic file order: by VIP (allocation order).
	slices.SortFunc(f.Accounts, func(x, y accountJSON) int {
		return cmp.Compare(parseVIPOrZero(x.VIP), parseVIPOrZero(y.VIP))
	})
	return f
}

// load reads accounts.json. A missing file is an empty store. DH values are computed lazily
// on each account's first HELLO, so a large file loads quickly.
func (s *accountStore) load(path string) error {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var f accountsFile
	if err := json.Unmarshal(data, &f); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	for i, j := range f.Accounts {
		a, err := accountFromJSON(j)
		if err != nil {
			return fmt.Errorf("%s: account %d: %w", path, i, err)
		}
		if s.byPK[a.PK] != nil || s.byPeer[a.PeerID] != nil || s.byXUID[a.XUID] != nil ||
			s.byVIP[a.VIP] != nil || s.byTag[strings.ToLower(a.Gamertag)] != nil {
			return fmt.Errorf("%s: account %d duplicates another account", path, i)
		}
		s.index(a)
		if a.VIP >= s.nextVIP {
			s.nextVIP = a.VIP + 1
		}
	}
	if v, ok := parseVIP(f.NextVIP); ok && v > s.nextVIP {
		s.nextVIP = v
	}
	return nil
}

func accountFromJSON(j accountJSON) (*Account, error) {
	a := &Account{Gamertag: j.Gamertag, Requested: j.Requested, Created: time.Unix(j.Created, 0).UTC()}
	var ok bool
	if a.PeerID, ok = parseHex16(j.PeerID); !ok || a.PeerID == 0 {
		return nil, errors.New("bad peer_id")
	}
	if a.XUID, ok = parseHex16(j.XUID); !ok || a.XUID>>48 != 0x0009 {
		return nil, errors.New("bad xuid")
	}
	if a.VIP, ok = parseVIP(j.VIP); !ok || !validVIP(a.VIP) {
		return nil, errors.New("bad vip")
	}
	pk, err := hex.DecodeString(j.PK)
	if err != nil || len(pk) != wire.KeySize {
		return nil, errors.New("bad pk")
	}
	copy(a.PK[:], pk)
	if a.Gamertag == "" || len(a.Gamertag) > maxGamertag {
		return nil, errors.New("bad gamertag")
	}
	if a.Requested == "" {
		a.Requested = a.Gamertag
	}
	return a, nil
}

// --- small helpers shared by the package ---

func hex16(v uint64) string { return fmt.Sprintf("%016x", v) }

func parseHex16(s string) (uint64, bool) {
	if len(s) != 16 {
		return 0, false
	}
	v, err := strconv.ParseUint(s, 16, 64)
	return v, err == nil
}

func ipString(v uint32) string {
	var b [4]byte
	binary.BigEndian.PutUint32(b[:], v)
	return netip.AddrFrom4(b).String()
}

func parseVIP(s string) (uint32, bool) {
	a, err := netip.ParseAddr(s)
	if err != nil || !a.Is4() {
		return 0, false
	}
	b := a.As4()
	return binary.BigEndian.Uint32(b[:]), true
}

func parseVIPOrZero(s string) uint32 {
	v, _ := parseVIP(s)
	return v
}

// remove deletes a from every index. Its virtual IP is not handed out again.
func (s *accountStore) remove(a *Account) {
	delete(s.byPK, a.PK)
	delete(s.byPeer, a.PeerID)
	delete(s.byXUID, a.XUID)
	delete(s.byVIP, a.VIP)
	if s.byTag[strings.ToLower(a.Gamertag)] == a {
		delete(s.byTag, strings.ToLower(a.Gamertag))
	}
	s.dirty = true
	s.gen++
}
