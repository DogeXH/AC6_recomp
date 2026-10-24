// ac6net - cryptographic primitives (PROTOCOL.md section 1), thin wrappers over Monocypher.
//
//   keypairs  X25519
//   KDF       BLAKE2b-256(key = dh, message = label || parts...)
//   AEAD      XChaCha20-Poly1305 (IETF construction), 24-byte nonce = ctr u64 LE || 16 zero
//             bytes, 16-byte tag appended after the ciphertext
//   replay    highest ctr + 1024-bit window
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace ac6net::crypto {

inline constexpr size_t kKeySize = 32;
inline constexpr size_t kNonceSize = 24;
inline constexpr size_t kTagSize = 16;

using Key = std::array<uint8_t, kKeySize>;  // symmetric key or DH output
using SecretKey = std::array<uint8_t, kKeySize>;
using PublicKey = std::array<uint8_t, kKeySize>;

// CSPRNG: arc4random_buf (Apple/BSD), getrandom (Linux), BCryptGenRandom (Windows).
// Aborts the process if the OS generator fails; there is no safe fallback.
void RandomBytes(uint8_t* out, size_t len);
uint64_t RandomU64();
uint32_t RandomU32();

// Overwrites memory in a way the compiler may not elide.
void Wipe(void* data, size_t len);

// Fills *sk with fresh random bytes and *pk with X25519(sk, basepoint).
void GenerateKeypair(SecretKey* sk, PublicKey* pk);
PublicKey PublicKeyFromSecret(const SecretKey& sk);

// dh = X25519(sk, pk). Returns false when the result is all zero (a low-order public key), the
// same inputs Go's crypto/ecdh rejects.
bool Dh(const SecretKey& sk, const PublicKey& pk, Key* out);

// KDF(dh, label, parts...) = BLAKE2b-256(key = dh, message = label || parts...).
Key Kdf(const Key& dh, std::string_view label,
        std::initializer_list<std::span<const uint8_t>> parts);

// Plain and keyed BLAKE2b with an arbitrary digest size (1..64).
void Blake2b(uint8_t* out, size_t out_len, const uint8_t* key, size_t key_len,
             const uint8_t* msg, size_t msg_len);

// nonce = ctr (u64 LE) || 16 zero bytes.
std::array<uint8_t, kNonceSize> CounterNonce(uint64_t ctr);

// Writes `pt_len + kTagSize` bytes to `out` (ciphertext, then tag). `out` may alias `pt`.
void Seal(const Key& key, uint64_t ctr, const uint8_t* ad, size_t ad_len, const uint8_t* pt,
          size_t pt_len, uint8_t* out);
// `in_len` includes the tag. On success writes `in_len - kTagSize` bytes to `out` (which may
// alias `in`). Returns false (and leaves `out` unspecified) on a short input or a bad tag.
bool Open(const Key& key, uint64_t ctr, const uint8_t* ad, size_t ad_len, const uint8_t* in,
          size_t in_len, uint8_t* out);

// Sliding anti-replay window over packet counters. ctr 0 is never valid (counters start at 1).
// Usage: if (!w.Check(ctr)) drop; authenticate; w.Accept(ctr).
class ReplayWindow {
 public:
  static constexpr uint64_t kWindowBits = 1024;

  // True if `ctr` is newer than anything seen, or inside the window and not yet seen.
  bool Check(uint64_t ctr) const;
  // Marks `ctr` as seen (call only after the packet authenticated). Returns Check(ctr).
  bool Accept(uint64_t ctr);
  // Forgets everything; with floor > 0, every ctr <= floor counts as already seen.
  void Reset(uint64_t floor = 0);
  uint64_t highest() const { return highest_; }

 private:
  bool TestBit(uint64_t ctr) const {
    const uint64_t i = ctr % kWindowBits;
    return (bits_[i / 64] >> (i % 64)) & 1u;
  }
  void SetBit(uint64_t ctr) {
    const uint64_t i = ctr % kWindowBits;
    bits_[i / 64] |= uint64_t{1} << (i % 64);
  }
  void ClearBit(uint64_t ctr) {
    const uint64_t i = ctr % kWindowBits;
    bits_[i / 64] &= ~(uint64_t{1} << (i % 64));
  }

  uint64_t highest_ = 0;
  std::array<uint64_t, kWindowBits / 64> bits_{};  // bit (ctr % 1024) set = ctr seen
};

}  // namespace ac6net::crypto
