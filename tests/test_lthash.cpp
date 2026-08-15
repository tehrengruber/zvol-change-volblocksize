// Unit tests for the incremental LtHash (lthash.hpp) -- pure, no ZFS needed.
#include <cstdint>
#include <string>
#include <vector>

#include "lthash.hpp"
#include "testing.hpp"

using namespace testing;

// A cell of `len` bytes filled with `byte`.
static std::vector<unsigned char> cell(std::size_t len, unsigned char byte) {
    return std::vector<unsigned char>(len, byte);
}

// add then remove of the same cell is exactly the empty digest (modular inverse).
TEST(lthash_add_remove_is_identity) {
    LtHash empty;
    LtHash h;
    auto a = cell(4096, 'a');
    h.add(7, a.data(), a.size());
    EXPECT_TRUE(h != empty);
    h.remove(7, a.data(), a.size());
    EXPECT_TRUE(h == empty);
    EXPECT_EQ(h.digest(), empty.digest());
}

// The digest is order-independent: the same set of cells hashes the same however
// they are added and removed.
TEST(lthash_order_independent) {
    auto a = cell(4096, 'a'), b = cell(4096, 'b'), c = cell(2048, 'c');
    LtHash h1;
    h1.add(0, a.data(), a.size());
    h1.add(1, b.data(), b.size());
    h1.add(2, c.data(), c.size());
    LtHash h2;
    h2.add(2, c.data(), c.size());
    h2.add(0, a.data(), a.size());
    h2.add(1, b.data(), b.size());
    EXPECT_TRUE(h1 == h2);
}

// Position is mixed in: identical content at different cell indices differs.
TEST(lthash_position_matters) {
    auto a = cell(4096, 'a');
    LtHash h1, h2;
    h1.add(0, a.data(), a.size());
    h2.add(1, a.data(), a.size());
    EXPECT_TRUE(h1 != h2);
}

// All-zero cells contribute nothing, so holes are free and an empty image is 0:
// adding a zero cell doesn't change the digest, and a freed (zeroed) cell leaves no
// trace.
TEST(lthash_zero_cells_are_free) {
    LtHash empty;
    LtHash h;
    auto z = cell(4096, 0);
    h.add(3, z.data(), z.size());
    EXPECT_TRUE(h == empty);      // adding a hole is a no-op
    auto a = cell(4096, 'a');
    h.add(3, a.data(), a.size());
    h.remove(3, a.data(), a.size());
    h.add(3, z.data(), z.size());  // "freed" back to zero
    EXPECT_TRUE(h == empty);
}

// The incremental update (remove old, add new) of one changed cell yields exactly the
// digest of building the final image from scratch -- the property the fingerprint
// relies on across snapshots.
TEST(lthash_incremental_matches_scratch) {
    auto a = cell(4096, 'a'), b = cell(4096, 'b'), c = cell(4096, 'c'), x = cell(4096, 'x');
    LtHash target;
    target.add(0, a.data(), a.size());
    target.add(1, b.data(), b.size());
    target.add(2, c.data(), c.size());

    LtHash incr;
    incr.add(0, a.data(), a.size());
    incr.add(1, x.data(), x.size());  // wrong content at cell 1
    incr.add(2, c.data(), c.size());
    incr.remove(1, x.data(), x.size());  // ... then correct it in place
    incr.add(1, b.data(), b.size());

    EXPECT_TRUE(incr == target);
    EXPECT_EQ(incr.digest(), target.digest());
}

// Different content in a cell gives a different digest (sanity: it tracks content).
TEST(lthash_content_sensitive) {
    auto a = cell(4096, 'a'), b = cell(4096, 'b');
    LtHash h1, h2;
    h1.add(0, a.data(), a.size());
    h2.add(0, b.data(), b.size());
    EXPECT_TRUE(h1.digest() != h2.digest());
}

// add() returns a seed; sub_seed() removes the cell from just that seed -- the fast
// path that lets the fingerprint undo a cell (a free, or the old half of an overwrite)
// without re-reading its content.
TEST(lthash_sub_seed_inverse) {
    LtHash empty, h;
    auto a = cell(4096, 'a');
    uint64_t s = h.add(5, a.data(), a.size());
    EXPECT_TRUE(s != 0);  // non-zero content -> non-zero seed
    h.sub_seed(s);
    EXPECT_TRUE(h == empty);
}

// sub_seed(seed) is identical to remove(content) for the same cell.
TEST(lthash_sub_seed_matches_remove) {
    auto a = cell(4096, 'a'), b = cell(2048, 'b');
    LtHash base;
    base.add(0, a.data(), a.size());
    uint64_t sb = base.add(1, b.data(), b.size());

    LtHash by_seed = base, by_content = base;
    by_seed.sub_seed(sb);
    by_content.remove(1, b.data(), b.size());
    EXPECT_TRUE(by_seed == by_content);

    LtHash only0;
    only0.add(0, a.data(), a.size());
    EXPECT_TRUE(by_seed == only0);  // both leave just cell 0
}

// A hole's seed is 0, and sub_seed(0) is a no-op.
TEST(lthash_hole_seed_is_zero) {
    LtHash empty, h;
    auto z = cell(4096, 0);
    EXPECT_TRUE(h.add(3, z.data(), z.size()) == static_cast<uint64_t>(0));
    h.sub_seed(0);
    EXPECT_TRUE(h == empty);
}

// serialize()/deserialize() round-trip the whole accumulator (the resume state).
TEST(lthash_serialize_roundtrip) {
    LtHash h;
    auto a = cell(4096, 'a'), b = cell(2048, 'b');
    h.add(0, a.data(), a.size());
    h.add(7, b.data(), b.size());
    std::string s = h.serialize();
    LtHash g;
    EXPECT_TRUE(g.deserialize(s));
    EXPECT_TRUE(g == h);
    EXPECT_EQ(g.digest(), h.digest());
    EXPECT_TRUE(!g.deserialize("not hex"));         // rejects bad input
    EXPECT_TRUE(!g.deserialize(s.substr(0, s.size() - 2)));  // rejects wrong length
}
