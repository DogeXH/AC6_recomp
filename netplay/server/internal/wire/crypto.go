// Package wire implements the ac6net v1 wire protocol (netplay/docs/PROTOCOL.md): the
// cryptographic primitives, the replay window and the packet layouts shared by the lobby
// server, the test client and the vector generator.
package wire

import (
	"crypto/ecdh"
	"encoding/binary"
	"errors"

	"golang.org/x/crypto/blake2b"
	"golang.org/x/crypto/chacha20poly1305"
)

// Sizes from PROTOCOL.md section 1.
const (
	KeySize   = 32 // X25519 keys, DH outputs and AEAD keys
	BootSize  = 8  // boot IDs
	NonceSize = chacha20poly1305.NonceSizeX
	TagSize   = chacha20poly1305.Overhead
)

// KDF labels.
const (
	LabelC2S = "AC6NET-C2S-1"
	LabelS2C = "AC6NET-S2C-1"
	LabelP2P = "AC6NET-P2P-1"
)

// Key is a 32-byte X25519 key, DH output or symmetric key.
type Key = [KeySize]byte

// Boot is an 8-byte boot ID.
type Boot = [BootSize]byte

var errLowOrder = errors.New("wire: X25519 produced a low-order shared secret")

// PublicKey returns X25519(sk, basepoint). The scalar is clamped as RFC 7748 requires.
func PublicKey(sk Key) Key {
	priv, err := ecdh.X25519().NewPrivateKey(sk[:])
	if err != nil {
		// NewPrivateKey only fails for a wrong length, which the array type rules out.
		panic(err)
	}
	var pk Key
	copy(pk[:], priv.PublicKey().Bytes())
	return pk
}

// DH returns X25519(sk, pk). It fails for a low-order peer key (all-zero output).
func DH(sk, pk Key) (Key, error) {
	var out Key
	priv, err := ecdh.X25519().NewPrivateKey(sk[:])
	if err != nil {
		return out, err
	}
	pub, err := ecdh.X25519().NewPublicKey(pk[:])
	if err != nil {
		return out, err
	}
	shared, err := priv.ECDH(pub)
	if err != nil {
		return out, errLowOrder
	}
	copy(out[:], shared)
	return out, nil
}

// KDF returns BLAKE2b-256(key = dh, message = label || parts...).
func KDF(dh Key, label string, parts ...[]byte) Key {
	h, err := blake2b.New256(dh[:])
	if err != nil {
		panic(err) // only for keys longer than 64 bytes
	}
	h.Write([]byte(label))
	for _, p := range parts {
		h.Write(p)
	}
	var out Key
	copy(out[:], h.Sum(nil))
	return out
}

// C2SKey derives the client-to-server key.
func C2SKey(dh, cPK, sPK Key, cBoot Boot) Key {
	return KDF(dh, LabelC2S, cPK[:], sPK[:], cBoot[:])
}

// S2CKey derives the server-to-client key.
func S2CKey(dh, cPK, sPK Key, cBoot, sBoot Boot) Key {
	return KDF(dh, LabelS2C, cPK[:], sPK[:], cBoot[:], sBoot[:])
}

// P2PKey derives the key a sender S uses for packets to a receiver R.
func P2PKey(dh, senderPK, receiverPK Key, senderBoot, receiverBoot Boot) Key {
	return KDF(dh, LabelP2P, senderPK[:], receiverPK[:], senderBoot[:], receiverBoot[:])
}

// Nonce returns ctr (u64 LE) followed by 16 zero bytes.
func Nonce(ctr uint64) [NonceSize]byte {
	var n [NonceSize]byte
	binary.LittleEndian.PutUint64(n[:8], ctr)
	return n
}

// Seal appends XChaCha20-Poly1305(key, Nonce(ctr), ad, plaintext) (ciphertext || tag) to dst.
func Seal(dst []byte, key Key, ctr uint64, ad, plaintext []byte) []byte {
	aead, err := chacha20poly1305.NewX(key[:])
	if err != nil {
		panic(err)
	}
	n := Nonce(ctr)
	return aead.Seal(dst, n[:], plaintext, ad)
}

// Open authenticates and decrypts ciphertext||tag. It returns a new slice.
func Open(key Key, ctr uint64, ad, sealed []byte) ([]byte, error) {
	if len(sealed) < TagSize {
		return nil, errors.New("wire: sealed data shorter than the tag")
	}
	aead, err := chacha20poly1305.NewX(key[:])
	if err != nil {
		return nil, err
	}
	n := Nonce(ctr)
	return aead.Open(nil, n[:], sealed, ad)
}

// CompatLabel prefixes the compat-key message.
const CompatLabel = "AC6NET-COMPAT-1"

// CompatMessage returns "AC6NET-COMPAT-1" || LE u32 title || media || version || protocol ||
// fps_mode bytes.
func CompatMessage(title, media, version, protocol uint32, fpsMode string) []byte {
	msg := make([]byte, 0, len(CompatLabel)+16+len(fpsMode))
	msg = append(msg, CompatLabel...)
	msg = binary.LittleEndian.AppendUint32(msg, title)
	msg = binary.LittleEndian.AppendUint32(msg, media)
	msg = binary.LittleEndian.AppendUint32(msg, version)
	msg = binary.LittleEndian.AppendUint32(msg, protocol)
	msg = append(msg, fpsMode...)
	return msg
}

// CompatKey returns BLAKE2b-128, keyed with 32 zero bytes, of CompatMessage(...).
func CompatKey(title, media, version, protocol uint32, fpsMode string) [16]byte {
	var zero [KeySize]byte
	h, err := blake2b.New(16, zero[:])
	if err != nil {
		panic(err)
	}
	h.Write(CompatMessage(title, media, version, protocol, fpsMode))
	var out [16]byte
	copy(out[:], h.Sum(nil))
	return out
}
