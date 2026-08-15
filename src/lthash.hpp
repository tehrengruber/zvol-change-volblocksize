// Incremental (homomorphic) multiset hash -- an "LtHash".  The digest of a set of
// position-keyed cells is the component-wise sum, modulo 2^16, of a wide vector
// derived from each cell.  Because it is just modular vector addition, a single cell
// can be added or removed in O(cell) without touching the rest -- so a snapshot's
// content fingerprint updates in time proportional to the bytes it changed, not the
// image size, with O(1) state (one 2 KiB accumulator, independent of the volume).
//
// Properties relied on by GUID alignment's fingerprint:
//   * All-zero cells contribute nothing, so holes -- and cells written back to zero,
//     which read identically on a zvol -- are free, and an empty image hashes to 0.
//   * The cell index is mixed in, so moving data between positions changes the digest
//     (a plain multiset hash would be order-independent).
//   * add and remove are exact inverses (modular add), so H(next) = H(prev) with each
//     changed cell's old contribution removed and its new one added.
//
// A cell's contribution is derived in two stages: a 64-bit *seed* = wyhash(content,
// index), then a wyrand stretch of that seed into the LANES lanes.  Only the first
// stage touches the (expensive) content, so add() returns the seed and sub_seed()
// removes a cell from just that 8-byte seed -- letting a caller cache seeds and undo
// cells (a free, or the old half of an overwrite) without re-reading their content.
//
// It is tuned to guard against *accidental* divergence -- both pools are under the
// operator's control -- not adversarial collisions.  A fingerprint file stores the full
// serialized accumulator per snapshot (see serialize()), so a resume can reload it;
// digest() is a compact sha256 of the accumulator for display/comparison.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class LtHash {
public:
    static constexpr std::size_t LANES = 1024;  // 16-bit lanes -> 2048-byte state

    // Reserved seed values a caller may use as sentinels: seed_of() never returns
    // either for real content (0 is returned only for a hole).
    static constexpr std::uint64_t HOLE = 0;               // an all-zero cell
    static constexpr std::uint64_t UNKNOWN = ~std::uint64_t{0};  // seed not (yet) known

    // Add one cell's contribution.  `index` is the cell's position, `data` its `len`
    // bytes.  Returns the cell's seed, which is HOLE (0) exactly when the cell is
    // all-zero.  Keep the seed to later sub_seed() the cell away.
    std::uint64_t add(std::uint64_t index, const void* data, std::size_t len) {
        std::uint64_t s = seed_of(index, data, len);
        apply(s, +1);
        return s;
    }
    // Remove a cell by content (recomputes its seed); returns whether it contributed.
    bool remove(std::uint64_t index, const void* data, std::size_t len) {
        std::uint64_t s = seed_of(index, data, len);
        apply(s, -1);
        return s != 0;
    }
    // Remove a cell by the seed add() returned; a 0 seed (a hole) is a no-op.
    void sub_seed(std::uint64_t seed) { apply(seed, -1); }

    std::string digest() const;  // sha256 of the accumulator, lowercase hex
    // Serialize / restore the full accumulator (for persisting resume state): the raw
    // lanes as lowercase hex (LANES*4 chars).  deserialize returns false on bad input.
    std::string serialize() const;
    bool deserialize(const std::string& hex);
    bool operator==(const LtHash& o) const { return lane_ == o.lane_; }
    bool operator!=(const LtHash& o) const { return !(*this == o); }

private:
    std::array<std::uint16_t, LANES> lane_{};  // zero-initialised == empty image
    // wyhash(content, index), or 0 for an all-zero cell (0 is reserved as the hole
    // seed, so a non-zero cell that happens to hash to 0 is nudged to 1).
    static std::uint64_t seed_of(std::uint64_t index, const void* data, std::size_t len);
    // Add (sign>0) or subtract (sign<0) the lanes stretched from `seed`; 0 is a no-op.
    void apply(std::uint64_t seed, int sign);
};
