// Minimal, dependency-free SHA-256 (FIPS 180-4) with a streaming update API.
// Used to fingerprint the logical content of a snapshot for GUID alignment.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sha256 {

struct Ctx {
    uint32_t h[8];
    uint64_t len;       // total bytes fed
    uint8_t buf[64];    // partial block
    size_t buf_len;     // bytes in buf
};

using Digest = std::array<uint8_t, 32>;

void init(Ctx& c);
void update(Ctx& c, const void* data, size_t len);
// Finalize and return the 32-byte digest.  `c` is consumed.
Digest raw(Ctx& c);
// Finalize and return the lowercase hex digest (64 chars).  `c` is consumed.
std::string hex(Ctx& c);

// Convenience one-shot helpers.
Digest raw_of(const void* data, size_t len);
std::string to_hex(const Digest& d);

}  // namespace sha256
