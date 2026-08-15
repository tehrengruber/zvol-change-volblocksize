#include "lthash.hpp"

#include <cstring>

#include "sha256.hpp"

namespace {

bool all_zero(const unsigned char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

// wyhash / wyrand (Wang Yi, public domain): a fast, non-linear hash whose mixing is a
// 64x64->128-bit multiply -- native on x86-64 and arm64, so it needs no intrinsics or
// per-architecture code and still runs at multiple GB/s.  It is not cryptographic, but
// it is a strong, well-distributed 64-bit hash -- vastly better than a CRC for
// distinguishing content -- which is all GUID alignment needs (its threat model is
// accidental divergence between two pools you control, not adversarial collisions).
// Requires a 64-bit target with __int128 (every platform ZFS runs on).
static const std::uint64_t WYS[4] = {0xa0761d6478bd642full, 0xe7037ed1a0b428dbull,
                                     0x8ebc6af09c88c6e3ull, 0x589965cc75374cc3ull};

inline std::uint64_t wymix(std::uint64_t a, std::uint64_t b) {
    __uint128_t r = static_cast<__uint128_t>(a) * b;
    return static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64);
}
inline std::uint64_t wyr8(const unsigned char* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);  // read little-endian so hosts agree on the digest
#endif
    return v;
}
inline std::uint64_t wyr4(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}
inline std::uint64_t wyr3(const unsigned char* p, std::size_t k) {
    return (static_cast<std::uint64_t>(p[0]) << 16) |
           (static_cast<std::uint64_t>(p[k >> 1]) << 8) | p[k - 1];
}

std::uint64_t wyhash(const void* key, std::size_t len, std::uint64_t seed) {
    const unsigned char* p = static_cast<const unsigned char*>(key);
    seed ^= WYS[0];
    std::uint64_t a, b;
    if (len <= 16) {
        if (len >= 4) {
            a = (wyr4(p) << 32) | wyr4(p + ((len >> 3) << 2));
            b = (wyr4(p + len - 4) << 32) | wyr4(p + len - 4 - ((len >> 3) << 2));
        } else if (len > 0) {
            a = wyr3(p, len);
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        std::size_t i = len;
        if (i > 48) {
            std::uint64_t s1 = seed, s2 = seed;
            do {
                seed = wymix(wyr8(p) ^ WYS[1], wyr8(p + 8) ^ seed);
                s1 = wymix(wyr8(p + 16) ^ WYS[2], wyr8(p + 24) ^ s1);
                s2 = wymix(wyr8(p + 32) ^ WYS[3], wyr8(p + 40) ^ s2);
                p += 48;
                i -= 48;
            } while (i > 48);
            seed ^= s1 ^ s2;
        }
        while (i > 16) {
            seed = wymix(wyr8(p) ^ WYS[1], wyr8(p + 8) ^ seed);
            i -= 16;
            p += 16;
        }
        a = wyr8(p + i - 16);
        b = wyr8(p + i - 8);
    }
    a ^= WYS[1];
    b ^= seed;
    __uint128_t r = static_cast<__uint128_t>(a) * b;
    a = static_cast<std::uint64_t>(r);
    b = static_cast<std::uint64_t>(r >> 64);
    return wymix(a ^ WYS[0] ^ len, b ^ WYS[1]);
}

// Stretch a 64-bit seed into LANES little-endian 16-bit lanes with the wyrand PRNG --
// 4 lanes per step.  Deterministic and endian-independent, so two hosts agree.
void stretch(std::uint64_t s, std::array<std::uint16_t, LtHash::LANES>& out) {
    static_assert(LtHash::LANES % 4 == 0, "4 lanes per wyrand step");
    for (std::size_t j = 0; j < LtHash::LANES / 4; ++j) {
        s += WYS[0];
        std::uint64_t r = wymix(s, s ^ WYS[1]);
        // Split the 64-bit wyrand output into four 16-bit lanes.  This is a
        // little-endian 64-bit store (the compiler merges it into one), but written
        // as explicit shifts so the lane values -- and hence the digest -- are the
        // same on a big-endian host (OpenZFS runs on s390x); a memcpy would be LE-only.
        out[j * 4 + 0] = static_cast<std::uint16_t>(r);
        out[j * 4 + 1] = static_cast<std::uint16_t>(r >> 16);
        out[j * 4 + 2] = static_cast<std::uint16_t>(r >> 32);
        out[j * 4 + 3] = static_cast<std::uint16_t>(r >> 48);
    }
}

}  // namespace

// wyhash(content, index) keyed so identical content at different positions differs;
// 0 is reserved to mean "hole", so a hole returns 0 and a non-zero cell that happens
// to hash to 0 is nudged to 1.
std::uint64_t LtHash::seed_of(std::uint64_t index, const void* data, std::size_t len) {
    if (all_zero(static_cast<const unsigned char*>(data), len)) return HOLE;  // hole
    std::uint64_t s = wyhash(data, len, index);
    return (s == HOLE || s == UNKNOWN) ? 1 : s;  // reserve those as caller sentinels
}

void LtHash::apply(std::uint64_t seed, int sign) {
    if (seed == 0) return;  // a hole contributes nothing
    std::array<std::uint16_t, LANES> e;
    stretch(seed, e);
    if (sign > 0)
        for (std::size_t k = 0; k < LANES; ++k) lane_[k] += e[k];
    else
        for (std::size_t k = 0; k < LANES; ++k) lane_[k] -= e[k];
}

std::string LtHash::digest() const {
    unsigned char bytes[LANES * 2];
    for (std::size_t k = 0; k < LANES; ++k) {
        bytes[2 * k] = static_cast<unsigned char>(lane_[k] & 0xff);
        bytes[2 * k + 1] = static_cast<unsigned char>(lane_[k] >> 8);
    }
    return sha256::to_hex(sha256::raw_of(bytes, sizeof bytes));
}

std::string LtHash::serialize() const {
    static const char* hexd = "0123456789abcdef";
    std::string out(LANES * 4, '0');  // 2 bytes per lane, 2 hex chars per byte
    for (std::size_t k = 0; k < LANES; ++k) {
        unsigned lo = lane_[k] & 0xff, hi = lane_[k] >> 8;  // little-endian bytes
        out[4 * k + 0] = hexd[lo >> 4];
        out[4 * k + 1] = hexd[lo & 0xf];
        out[4 * k + 2] = hexd[hi >> 4];
        out[4 * k + 3] = hexd[hi & 0xf];
    }
    return out;
}

bool LtHash::deserialize(const std::string& hex) {
    if (hex.size() != LANES * 4) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t k = 0; k < LANES; ++k) {
        int a = nib(hex[4 * k]), b = nib(hex[4 * k + 1]);
        int c = nib(hex[4 * k + 2]), d = nib(hex[4 * k + 3]);
        if ((a | b | c | d) < 0) return false;
        unsigned lo = (a << 4) | b, hi = (c << 4) | d;
        lane_[k] = static_cast<std::uint16_t>(lo | (hi << 8));
    }
    return true;
}
