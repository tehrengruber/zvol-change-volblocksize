// --verify-only compares an existing destination against the source with no
// transfer, and fails if they differ.
#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

TEST(verify_only) {
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    write_pattern(source, 8 * MiB, 8 * MiB, 2);
    snapshot(source, "s1");
    zfs({"set", "readonly=on", source});

    std::string dest = source + "-new";
    EXPECT_EQ(migrate(source, "16k", {"--no-swap"}), 0);

    // Passes on the freshly migrated (matching) destination.
    EXPECT_EQ(migrate(source, "16k", {"--verify-only"}), 0);

    // Corrupt the destination's live head; verify-only must now fail.
    write_pattern(dest, 0, 1 * MiB, 999);  // dest is writable (not swapped)
    EXPECT_NE(migrate(source, "16k", {"--verify-only"}), 0);

    // Missing destination is an error, not a silent pass.
    EXPECT_NE(migrate(source, "16k", {"--dest", source + "-absent", "--verify-only"}),
              0);
}

// A NON-head snapshot differing must be caught even when s0 and the head both match --
// i.e. the chunk-major sweep really compares every snapshot, not just the head.
TEST(verify_only_snapshot_mismatch) {
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    write_pattern(source, 8 * MiB, 8 * MiB, 2);
    snapshot(source, "s1");
    zfs({"set", "readonly=on", source});

    // Control: a faithful stand-in dest (every snapshot matches) must PASS -- so a
    // failure below is attributable to content, not a structural rejection of `--dest`.
    auto good = make_zvol("16k", "32M", "good");
    write_pattern(good, 0, 8 * MiB, 1);
    snapshot(good, "s0");
    write_pattern(good, 8 * MiB, 8 * MiB, 2);
    snapshot(good, "s1");
    zfs({"set", "readonly=on", good});
    EXPECT_EQ(migrate(source, "16k", {"--dest", good, "--verify-only"}), 0);

    // A stand-in whose s0 and head match the source, but whose s1 diverges, must FAIL.
    auto other = make_zvol("16k", "32M", "other");
    write_pattern(other, 0, 8 * MiB, 1);        // == source@s0
    snapshot(other, "s0");
    write_pattern(other, 8 * MiB, 8 * MiB, 3);   // s1 diverges (seed 3 vs 2)
    snapshot(other, "s1");
    write_pattern(other, 8 * MiB, 8 * MiB, 2);   // head restored to match the source's
    zfs({"set", "readonly=on", other});
    EXPECT_NE(migrate(source, "16k", {"--dest", other, "--verify-only"}), 0);
}

// A difference far from offset 0 (many chunks in) must still be caught by the sweep.
TEST(verify_only_deep_mismatch) {
    auto source = make_zvol("8k", "64M");
    write_pattern(source, 0, 64 * MiB, 1);
    snapshot(source, "s0");
    zfs({"set", "readonly=on", source});

    std::string dest = source + "-new";
    EXPECT_EQ(migrate(source, "16k", {"--no-swap"}), 0);
    EXPECT_EQ(migrate(source, "16k", {"--verify-only"}), 0);
    write_pattern(dest, 40 * MiB, 1 * MiB, 999);  // corrupt the head deep in
    EXPECT_NE(migrate(source, "16k", {"--verify-only"}), 0);
}

// The rounded-up tail check: when the dest is rounded up to the new blocksize, its extra
// tail must read as zeros; a dirtied tail must fail verify.
TEST(verify_only_rounded_tail_not_zero) {
    auto source = make_zvol("8k", "24584k");  // mult of 8k, not of 32k -> dest rounded up
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    zfs({"set", "readonly=on", source});

    std::string dest = source + "-new";
    EXPECT_EQ(migrate(source, "32k", {"--no-swap"}), 0);
    EXPECT_EQ(migrate(source, "32k", {"--verify-only"}), 0);  // rounded tail is zeros
    write_pattern(dest, 24584ull * 1024, 8 * 1024, 77);       // dirty the rounded-up tail
    EXPECT_NE(migrate(source, "32k", {"--verify-only"}), 0);
}
