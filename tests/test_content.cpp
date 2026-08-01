// Per-snapshot content correctness across a volblocksize change.
#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

static void build_history(const std::string& ds) {
    write_pattern(ds, 0, 4 * MiB, 1);
    write_pattern(ds, 20 * MiB, 4 * MiB, 2);
    snapshot(ds, "s0");

    write_pattern(ds, 2 * MiB, 3 * MiB, 3);   // overwrite
    write_pattern(ds, 40 * MiB, 5 * MiB, 4);  // new region
    snapshot(ds, "s1");

    write_pattern(ds, 0, 1 * MiB, 5);         // overwrite head
    write_pattern(ds, 60 * MiB, 2 * MiB, 6);  // near end
    snapshot(ds, "s2");
}

static void assert_migrated_equal(const std::string& source,
                                  const std::string& backup,
                                  uint64_t target_bytes) {
    EXPECT_TRUE(snapshot_names(source) == snapshot_names(backup));
    EXPECT_EQ(std::stoull(get_prop(source, "volblocksize")), target_bytes);
    for (const auto& snap : snapshot_names(source)) {
        EXPECT_TRUE(devices_equal(source + "@" + snap, backup + "@" + snap,
                                  volsize(source + "@" + snap)));
    }
    EXPECT_TRUE(devices_equal(source, backup, volsize(source)));
}

TEST(content_larger) {
    auto source = make_zvol("8k", "64M");
    build_history(source);
    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "32k", {"-v"}), 0);
    assert_migrated_equal(source, source + "-old", 32u * 1024);
}

TEST(content_smaller) {
    auto source = make_zvol("64k", "64M");
    build_history(source);
    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "8k", {"-v"}), 0);
    assert_migrated_equal(source, source + "-old", 8u * 1024);
}

TEST(single_snapshot) {
    auto source = make_zvol("16k", "32M");
    write_pattern(source, 0, 8 * MiB, 10);
    write_pattern(source, 16 * MiB, 4 * MiB, 11);
    snapshot(source, "only");
    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "64k", {"-v"}), 0);
    assert_migrated_equal(source, source + "-old", 64u * 1024);
}

TEST(verify_all) {
    auto source = make_zvol("8k", "64M");
    build_history(source);
    zfs({"set", "readonly=on", source});
    // The built-in verifier byte-compares every snapshot + head before swapping;
    // exit 0 means it passed. Then confirm externally too.
    EXPECT_EQ(migrate(source, "32k", {"--verify", "all"}), 0);
    assert_migrated_equal(source, source + "-old", 32u * 1024);
}

TEST(verify_one) {
    // --verify @<snapshot> compares only that one snapshot (not the head).
    auto source = make_zvol("8k", "64M");
    build_history(source);
    zfs({"set", "readonly=on", source});

    EXPECT_EQ(migrate(source, "32k",
                      {"--no-swap", "--dest", source + "-n1", "--verify", "@s1"}),
              0);
    // A snapshot that doesn't exist on the source is rejected (non-zero exit).
    EXPECT_NE(migrate(source, "32k",
                      {"--no-swap", "--dest", source + "-n2", "--verify", "@nope"}),
              0);
    // A bare (non-@) name is not a valid --verify argument.
    EXPECT_NE(migrate(source, "32k",
                      {"--no-swap", "--dest", source + "-n3", "--verify", "s1"}),
              0);
}

TEST(verify_detects_mismatch) {
    // Make the live head diverge from the newest snapshot by writing after it.
    // The tool only reproduces up to the newest snapshot, so the migrated head
    // will differ from the original head -- exactly what --verify must catch.
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    write_pattern(source, 8 * MiB, 8 * MiB, 2);  // head now differs from s0
    zfs({"set", "readonly=on", source});

    // --force gets past the divergent-head precondition so we exercise --verify
    // itself: it compares the migrated head to the (divergent) original head, must
    // fail (non-zero exit), and since it runs before the swap the original is left
    // untouched (no <source>-old backup was created).
    EXPECT_NE(migrate(source, "16k", {"--force", "--verify", "head"}), 0);
    EXPECT_TRUE(!dataset_exists(source + "-old"));
}

TEST(no_swap) {
    auto source = make_zvol("8k", "16M");
    write_pattern(source, 0, 4 * MiB, 20);
    snapshot(source, "s0");
    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "16k", {"--no-swap"}), 0);
    auto dest = source + "-new";
    EXPECT_EQ(std::stoull(get_prop(dest, "volblocksize")), 16u * 1024);
    EXPECT_TRUE(devices_equal(dest + "@s0", source + "@s0", volsize(source + "@s0")));
}
