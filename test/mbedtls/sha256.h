#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

struct mbedtls_sha256_context {
  uint32_t state[8];
  uint64_t bitcount;
  uint8_t buffer[64];
};

inline uint32_t mbedtls_rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

inline void mbedtls_sha256_transform(uint32_t state[8], const uint8_t block[64]) {
  static constexpr uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
  };
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = mbedtls_rotr32(w[i - 15], 7) ^ mbedtls_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = mbedtls_rotr32(w[i - 2], 17) ^ mbedtls_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = mbedtls_rotr32(e, 6) ^ mbedtls_rotr32(e, 11) ^ mbedtls_rotr32(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + S1 + ch + k[i] + w[i];
    const uint32_t S0 = mbedtls_rotr32(a, 2) ^ mbedtls_rotr32(a, 13) ^ mbedtls_rotr32(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

inline void mbedtls_sha256_init(mbedtls_sha256_context* ctx) { std::memset(ctx, 0, sizeof(*ctx)); }

inline void mbedtls_sha256_free(mbedtls_sha256_context* ctx) { std::memset(ctx, 0, sizeof(*ctx)); }

inline void mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int /*is224*/) {
  ctx->bitcount = 0;
  ctx->state[0] = 0x6a09e667u;
  ctx->state[1] = 0xbb67ae85u;
  ctx->state[2] = 0x3c6ef372u;
  ctx->state[3] = 0xa54ff53au;
  ctx->state[4] = 0x510e527fu;
  ctx->state[5] = 0x9b05688cu;
  ctx->state[6] = 0x1f83d9abu;
  ctx->state[7] = 0x5be0cd19u;
}

inline void mbedtls_sha256_update(mbedtls_sha256_context* ctx, const uint8_t* input, size_t ilen) {
  size_t idx = static_cast<size_t>((ctx->bitcount >> 3) & 0x3Fu);
  ctx->bitcount += static_cast<uint64_t>(ilen) << 3;
  size_t i = 0;
  if (idx > 0) {
    const size_t fill = 64 - idx;
    if (ilen < fill) {
      std::memcpy(ctx->buffer + idx, input, ilen);
      return;
    }
    std::memcpy(ctx->buffer + idx, input, fill);
    mbedtls_sha256_transform(ctx->state, ctx->buffer);
    i += fill;
  }
  for (; i + 64 <= ilen; i += 64) {
    mbedtls_sha256_transform(ctx->state, input + i);
  }
  if (i < ilen) {
    std::memcpy(ctx->buffer, input + i, ilen - i);
  }
}

inline void mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t output[32]) {
  uint8_t pad[64] = {0x80};
  const uint64_t bits = ctx->bitcount;
  const size_t idx = static_cast<size_t>((bits >> 3) & 0x3Fu);
  const size_t padLen = (idx < 56) ? (56 - idx) : (120 - idx);
  mbedtls_sha256_update(ctx, pad, padLen);
  uint8_t lenBytes[8];
  for (int i = 0; i < 8; ++i) {
    lenBytes[7 - i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFFu);
  }
  mbedtls_sha256_update(ctx, lenBytes, 8);
  for (int i = 0; i < 8; ++i) {
    output[i * 4] = static_cast<uint8_t>((ctx->state[i] >> 24) & 0xFFu);
    output[i * 4 + 1] = static_cast<uint8_t>((ctx->state[i] >> 16) & 0xFFu);
    output[i * 4 + 2] = static_cast<uint8_t>((ctx->state[i] >> 8) & 0xFFu);
    output[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i] & 0xFFu);
  }
}
