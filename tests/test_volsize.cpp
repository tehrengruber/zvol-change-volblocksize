// volsize changing across snapshots must be reproduced in both directions.
#include <algorithm>

#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

static void assert_snapshots_match(const std::string& source,
                                   const std::string& backup) {
    EXPECT_TRUE(snapshot_names(source) == snapshot_names(backup));
    for (const auto& snap : snapshot_names(source)) {
        uint64_t old_size = volsize(backup + "@" + snap);
        EXPECT_EQ(volsize(source + "@" + snap), old_size);
        EXPECT_TRUE(devices_equal(source + "@" + snap, backup + "@" + snap, old_size));
    }
}

TEST(volsize_increases) {
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 16 * MiB, 1);
    snapshot(source, "small");

    set_volsize(source, 64 * MiB);
    write_pattern(source, 40 * MiB, 16 * MiB, 2);
    snapshot(source, "grown");

    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "32k", {"-v"}), 0);
    assert_snapshots_match(source, source + "-old");
}

TEST(volsize_decreases) {
    auto source = make_zvol("8k", "64M");
    write_pattern(source, 0, 48 * MiB, 3);
    snapshot(source, "big");

    set_volsize(source, 32 * MiB);  // shrink; tail truncated
    write_pattern(source, 8 * MiB, 4 * MiB, 4);
    snapshot(source, "shrunk");

    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "32k", {"-v"}), 0);
    assert_snapshots_match(source, source + "-old");
}

TEST(volsize_shrink_then_grow) {
    // Shrink then regrow past the old data: if ZFS didn't free the truncated tail
    // on shrink, stale data would reappear in the regrown-but-unwritten region.
    auto source = make_zvol("8k", "64M");
    write_pattern(source, 0, 48 * MiB, 1);
    snapshot(source, "big");
    set_volsize(source, 16 * MiB);
    snapshot(source, "small");
    set_volsize(source, 64 * MiB);  // regrow past the old 48M of data
    write_pattern(source, 20 * MiB, 4 * MiB, 2);
    snapshot(source, "regrown");

    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "32k", {"-v"}), 0);
    assert_snapshots_match(source, source + "-old");

    // The regrown region beyond 16M that wasn't rewritten must read as zeros.
    auto tail = read_range(source + "@regrown", 40 * MiB, 8 * MiB);
    EXPECT_TRUE(std::all_of(tail.begin(), tail.end(),
                            [](uint8_t b) { return b == 0; }));
}

TEST(volsize_unaligned) {
    // volsize is a multiple of the 8k source block but not of the 32k target, so
    // the destination is rounded up and its tail must read as zeros -- which
    // --verify all now checks (per snapshot and head).
    auto source = make_zvol("8k", "24584k");  // mult of 8k, not of 32k
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    zfs({"set", "readonly=on", source});

    EXPECT_EQ(migrate(source, "32k", {"--verify", "all"}), 0);

    uint64_t vs = volsize(source);
    EXPECT_EQ(vs % (32u * 1024), 0u);      // rounded up to a 32k multiple
    EXPECT_TRUE(vs >= 24584u * 1024);
}
