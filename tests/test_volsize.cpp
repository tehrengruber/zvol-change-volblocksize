// volsize changing across snapshots must be reproduced in both directions.
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
