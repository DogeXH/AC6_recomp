package lobby

import (
	"crypto/rand"
	"encoding/hex"
	"strings"
	"time"

	"ac6lobby/internal/wire"
)

// Session flags used by the server (XSESSION_CREATE_*).
const (
	flagMatchmaking       = 0x008
	flagJoinInProgressOff = 0x400
)

// Session limits.
const (
	maxSessionsPerAccount = 4
	maxSessionsTotal      = 20000
	maxSlots              = 64 // public + private
	sessionIdleTimeout    = 2 * time.Hour
)

type sessionState int

const (
	stateLobby sessionState = iota
	stateInGame
)

func (s sessionState) String() string {
	if s == stateInGame {
		return "ingame"
	}
	return "lobby"
}

type member struct {
	XUID    uint64
	Private bool
}

// Session is an in-memory matchmaking session.
type Session struct {
	ID           uint64 // XNKID bytes in guest order, read as a big-endian u64 (0x80 top byte)
	Key          [16]byte
	Nonce        uint64
	Title        string
	Compat       string
	Flags        uint32
	PublicSlots  uint32
	PrivateSlots uint32
	Contexts     []Context
	Properties   []Prop
	State        sessionState

	Host     *Account
	HostBoot wire.Boot

	Members    []member        // join order
	Arbitrated map[uint64]bool // machine IDs that called session.arbitrate

	seq     uint64 // creation order, for newest-first search results
	Created time.Time
	Updated time.Time
}

func (s *Session) filled() (pub, priv uint32) {
	for _, m := range s.Members {
		if m.Private {
			priv++
		} else {
			pub++
		}
	}
	return pub, priv
}

func (s *Session) openPublic() uint32 {
	pub, _ := s.filled()
	if pub >= s.PublicSlots {
		return 0
	}
	return s.PublicSlots - pub
}

func (s *Session) memberIndex(xuid uint64) int {
	for i, m := range s.Members {
		if m.XUID == xuid {
			return i
		}
	}
	return -1
}

func (s *Session) hasContext(c Context) bool {
	for _, own := range s.Contexts {
		if own.ID == c.ID {
			return own.Value == c.Value
		}
	}
	return false
}

func newSessionID(exists func(uint64) bool) uint64 {
	for {
		id := 0x80<<56 | random64()&(1<<56-1)
		if !exists(id) {
			return id
		}
	}
}

func randomKey16() [16]byte {
	var k [16]byte
	if _, err := rand.Read(k[:]); err != nil {
		panic(err)
	}
	return k
}

// --- wire documents ---

type hostDoc struct {
	PeerID   string `json:"peer_id"`
	VIP      string `json:"vip"`
	XUID     string `json:"xuid"`
	Gamertag string `json:"gamertag"`
}

type memberDoc struct {
	XUID    string `json:"xuid"`
	Private bool   `json:"private"`
}

// SessionDoc is the PROTOCOL.md 3.1 SessionDoc plus "nonce" (needed by joining clients for
// session.arbitrate).
type sessionDoc struct {
	SessionID     string      `json:"session_id"`
	Key           string      `json:"key"`
	Nonce         string      `json:"nonce"`
	Title         string      `json:"title"`
	Flags         uint32      `json:"flags"`
	State         string      `json:"state"`
	PublicSlots   uint32      `json:"public_slots"`
	PrivateSlots  uint32      `json:"private_slots"`
	FilledPublic  uint32      `json:"filled_public"`
	FilledPrivate uint32      `json:"filled_private"`
	Host          hostDoc     `json:"host"`
	Contexts      []Context   `json:"contexts"`
	Properties    []Prop      `json:"properties"`
	Members       []memberDoc `json:"members"`
}

func (s *Session) doc() sessionDoc {
	pub, priv := s.filled()
	d := sessionDoc{
		SessionID: hex16(s.ID), Key: hex.EncodeToString(s.Key[:]), Nonce: hex16(s.Nonce),
		Title: s.Title, Flags: s.Flags, State: s.State.String(),
		PublicSlots: s.PublicSlots, PrivateSlots: s.PrivateSlots, FilledPublic: pub, FilledPrivate: priv,
		Host: hostDoc{PeerID: hex16(s.Host.PeerID), VIP: ipString(s.Host.VIP), XUID: hex16(s.Host.XUID),
			Gamertag: s.Host.Gamertag},
		Contexts: nonNil(s.Contexts), Properties: nonNil(s.Properties), Members: make([]memberDoc, 0, len(s.Members)),
	}
	for _, m := range s.Members {
		d.Members = append(d.Members, memberDoc{XUID: hex16(m.XUID), Private: m.Private})
	}
	return d
}

func nonNil[T any](s []T) []T {
	if s == nil {
		return []T{}
	}
	return s
}

// validTitle accepts 8 hex digits.
func validTitle(t string) bool {
	if len(t) != 8 {
		return false
	}
	_, err := hex.DecodeString(t)
	return err == nil
}

func normTitle(t string) string { return strings.ToUpper(t) }

// validCompat accepts 32 hex digits.
func validCompat(c string) bool {
	if len(c) != 32 {
		return false
	}
	_, err := hex.DecodeString(c)
	return err == nil
}
