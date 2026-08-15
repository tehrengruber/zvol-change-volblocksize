#include "sha256.hpp"

#include <cstring>

namespace sha256 {

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t rotr(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void init(Ctx& c) {
    c.h[0] = 0x6a09e667; c.h[1] = 0xbb67ae85; c.h[2] = 0x3c6ef372;
    c.h[3] = 0xa54ff53a; c.h[4] = 0x510e527f; c.h[5] = 0x9b05688c;
    c.h[6] = 0x1f83d9ab; c.h[7] = 0x5be0cd19;
    c.len = 0; c.buf_len = 0;
}

void update(Ctx& c, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    c.len += len;
    if (c.buf_len) {  // fill the partial block first
        size_t take = 64 - c.buf_len;
        if (take > len) take = len;
        memcpy(c.buf + c.buf_len, p, take);
        c.buf_len += take; p += take; len -= take;
        if (c.buf_len == 64) { compress(c.h, c.buf); c.buf_len = 0; }
    }
    while (len >= 64) { compress(c.h, p); p += 64; len -= 64; }
    if (len) { memcpy(c.buf, p, len); c.buf_len = len; }
}

Digest raw(Ctx& c) {
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80;
    update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.buf_len != 56) update(c, &zero, 1);
    uint8_t lenbe[8];
    for (int i = 0; i < 8; ++i) lenbe[i] = (uint8_t)(bits >> (56 - i * 8));
    update(c, lenbe, 8);  // triggers the final compress()

    Digest d;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) d[i * 4 + j] = (uint8_t)(c.h[i] >> (24 - j * 8));
    return d;
}

std::string to_hex(const Digest& d) {
    static const char* hexd = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < d.size(); ++i) {
        out[i * 2] = hexd[d[i] >> 4];
        out[i * 2 + 1] = hexd[d[i] & 0xf];
    }
    return out;
}

std::string hex(Ctx& c) { return to_hex(raw(c)); }

Digest raw_of(const void* data, size_t len) {
    Ctx c;
    init(c);
    update(c, data, len);
    return raw(c);
}

}  // namespace sha256
