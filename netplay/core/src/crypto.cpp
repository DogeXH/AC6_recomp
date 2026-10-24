// ac6net - cryptographic primitives over Monocypher 4.0.2 (see crypto.h).
//
// AEAD note: Monocypher's crypto_aead_lock/unlock derive a subkey with HChaCha20(key,
// nonce[0..16]), then run ChaCha20 (64-bit block counter, 64-bit nonce = nonce[16..24]); block 0
// gives the Poly1305 key, the payload starts at block 1, and the tag covers
// ad || pad16 || ct || pad16 || le64(|ad|) || le64(|ct|). For messages under 2^32 blocks that is
// bit-for-bit the IETF XChaCha20-Poly1305 construction (96-bit nonce 0x00000000 || nonce[16..24],
// 32-bit counter), i.e. Go's chacha20poly1305.NewX. We append the tag after the ciphertext.
#include "crypto.h"

#include <cstdlib>
#include <cstring>

#include "monocypher/monocypher.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>  // arc4random_buf
#elif defined(__linux__)
#include <errno.h>
#include <sys/random.h>
#else
#error "ac6net: no CSPRNG for this platform"
#endif

namespace ac6net::crypto {

void RandomBytes(uint8_t* out, size_t len) {
  if (len == 0) return;
#if defined(_WIN32)
  while (len > 0) {
    const ULONG chunk = len > 0x10000000 ? 0x10000000 : static_cast<ULONG>(len);
    if (BCryptGenRandom(nullptr, out, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) std::abort();
    out += chunk;
    len -= chunk;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  arc4random_buf(out, len);
#else
  while (len > 0) {
    const ssize_t n = getrandom(out, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::abort();
    }
    out += n;
    len -= static_cast<size_t>(n);
  }
#endif
}

uint64_t RandomU64() {
  uint8_t b[8];
  RandomBytes(b, sizeof(b));
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
  return v;
}

uint32_t RandomU32() { return static_cast<uint32_t>(RandomU64()); }

void Wipe(void* data, size_t len) { crypto_wipe(data, len); }

void GenerateKeypair(SecretKey* sk, PublicKey* pk) {
  RandomBytes(sk->data(), sk->size());
  crypto_x25519_public_key(pk->data(), sk->data());
}

PublicKey PublicKeyFromSecret(const SecretKey& sk) {
  PublicKey pk;
  crypto_x25519_public_key(pk.data(), sk.data());
  return pk;
}

bool Dh(const SecretKey& sk, const PublicKey& pk, Key* out) {
  crypto_x25519(out->data(), sk.data(), pk.data());
  uint8_t acc = 0;
  for (uint8_t b : *out) acc |= b;
  return acc != 0;
}

Key Kdf(const Key& dh, std::string_view label,
        std::initializer_list<std::span<const uint8_t>> parts) {
  crypto_blake2b_ctx ctx;
  crypto_blake2b_keyed_init(&ctx, kKeySize, dh.data(), dh.size());
  crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(label.data()), label.size());
  for (const auto& part : parts) crypto_blake2b_update(&ctx, part.data(), part.size());
  Key out;
  crypto_blake2b_final(&ctx, out.data());
  return out;
}

void Blake2b(uint8_t* out, size_t out_len, const uint8_t* key, size_t key_len,
             const uint8_t* msg, size_t msg_len) {
  crypto_blake2b_keyed(out, out_len, key, key_len, msg, msg_len);
}

std::array<uint8_t, kNonceSize> CounterNonce(uint64_t ctr) {
  std::array<uint8_t, kNonceSize> nonce{};
  for (int i = 0; i < 8; ++i) nonce[i] = static_cast<uint8_t>(ctr >> (8 * i));
  return nonce;
}

void Seal(const Key& key, uint64_t ctr, const uint8_t* ad, size_t ad_len, const uint8_t* pt,
          size_t pt_len, uint8_t* out) {
  const auto nonce = CounterNonce(ctr);
  uint8_t mac[kTagSize];
  crypto_aead_lock(out, mac, key.data(), nonce.data(), ad, ad_len, pt, pt_len);
  std::memcpy(out + pt_len, mac, kTagSize);
}

bool Open(const Key& key, uint64_t ctr, const uint8_t* ad, size_t ad_len, const uint8_t* in,
          size_t in_len, uint8_t* out) {
  if (in_len < kTagSize) return false;
  const size_t ct_len = in_len - kTagSize;
  const auto nonce = CounterNonce(ctr);
  uint8_t mac[kTagSize];
  std::memcpy(mac, in + ct_len, kTagSize);  // copy first: `out` may alias `in`
  return crypto_aead_unlock(out, mac, key.data(), nonce.data(), ad, ad_len, in, ct_len) == 0;
}

bool ReplayWindow::Check(uint64_t ctr) const {
  if (ctr == 0) return false;
  if (ctr > highest_) return true;
  if (highest_ - ctr >= kWindowBits) return false;
  return !TestBit(ctr);
}

bool ReplayWindow::Accept(uint64_t ctr) {
  if (!Check(ctr)) return false;
  if (ctr > highest_) {
    const uint64_t advance = ctr - highest_;
    if (advance >= kWindowBits) {
      bits_.fill(0);
    } else {
      for (uint64_t c = highest_ + 1; c < ctr; ++c) ClearBit(c);
    }
    highest_ = ctr;
  }
  SetBit(ctr);
  return true;
}

void ReplayWindow::Reset(uint64_t floor) {
  highest_ = floor;
  bits_.fill(floor > 0 ? ~uint64_t{0} : 0);
}

}  // namespace ac6net::crypto
