// Small built-in crypto for Vespera: SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256 and a secure
// random source. Replaces OpenSSL so the server builds as one Windows .exe with no extra DLLs.
// Produces exactly the same password hashes as OpenSSL's PKCS5_PBKDF2_HMAC(..., EVP_sha256()).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fstream>
#endif

namespace vcrypto {

struct Sha256 {
  uint32_t h[8]; uint64_t len = 0; unsigned char buf[64]; size_t used = 0;
  Sha256() { static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; memcpy(h, init, sizeof h); }
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const unsigned char* p) {
    static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const void* data, size_t n) {
    auto p = (const unsigned char*)data; len += n;
    while (n) { size_t take = std::min(n, 64 - used); memcpy(buf + used, p, take); used += take; p += take; n -= take; if (used == 64) { block(buf); used = 0; } }
  }
  void final(unsigned char out[32]) {
    uint64_t bits = len * 8; unsigned char pad = 0x80; update(&pad, 1); unsigned char z = 0;
    while (used != 56) update(&z, 1);
    unsigned char l[8]; for (int i = 0; i < 8; i++) l[i] = (unsigned char)(bits >> (56 - 8 * i)); update(l, 8);
    for (int i = 0; i < 8; i++) { out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i]; }
  }
};

// HMAC-SHA256 with the key's inner/outer states prepared once (PBKDF2 reuses them many times).
struct Hmac {
  Sha256 in, out;
  explicit Hmac(const std::string& key) {
    unsigned char k[64] = {0};
    if (key.size() > 64) { Sha256 s; s.update(key.data(), key.size()); s.final(k); } else memcpy(k, key.data(), key.size());
    unsigned char ip[64], op[64]; for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    in.update(ip, 64); out.update(op, 64);
  }
  void mac(const unsigned char* msg, size_t n, unsigned char res[32]) const {
    Sha256 a = in; a.update(msg, n); unsigned char t[32]; a.final(t);
    Sha256 b = out; b.update(t, 32); b.final(res);
  }
};

// PBKDF2-HMAC-SHA256 producing 32 bytes (one block), as used for Vespera passwords.
inline void pbkdf2_sha256(const std::string& pw, const std::string& salt, int iterations, unsigned char out[32]) {
  Hmac hm(pw); std::string s = salt; s.append("\0\0\0\1", 4);
  unsigned char u[32]; hm.mac((const unsigned char*)s.data(), s.size(), u); memcpy(out, u, 32);
  for (int i = 1; i < iterations; i++) { hm.mac(u, 32, u); for (int j = 0; j < 32; j++) out[j] ^= u[j]; }
}

// Cryptographically secure random bytes from the operating system.
inline void random_bytes(void* buf, size_t n) {
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) throw std::runtime_error("no secure random source");
#else
  std::ifstream f("/dev/urandom", std::ios::binary); if (!f.read((char*)buf, (std::streamsize)n)) throw std::runtime_error("no secure random source");
#endif
}

}  // namespace vcrypto
