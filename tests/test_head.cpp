// The tool assumes the newest snapshot equals the live head (enforced by
// readonly=on): it reproduces the volume up to the newest snapshot and makes that
// the head.  Demonstrate it concretely -- write data *after* the last snapshot so
// the head diverges, then check the migrated head matches the newest snapshot,
// not the divergent original head.
#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

TEST(head_is_newest_snapshot) {
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    write_pattern(source, 8 * MiB, 8 * MiB, 2);  // diverges from s0 (not snapshotted)
    zfs({"set", "readonly=on", source});

    // A divergent head is rejected by default (see rejects_divergent_head); with
    // --force the tool migrates up to the newest snapshot and makes that the head.
    EXPECT_EQ(migrate(source, "16k", {"--force", "-v"}), 0);

    auto backup = source + "-old";
    // The migrated head equals the newest snapshot's content...
    EXPECT_TRUE(devices_equal(source, backup + "@s0", volsize(backup + "@s0")));
    // ...and the post-snapshot writes are not carried, so it differs from the
    // (divergent) original head -- exactly the documented assumption.
    EXPECT_TRUE(!devices_equal(source, backup, volsize(source)));
}

TEST(rejects_divergent_head) {
    // Data written after the last snapshot means the live head is not captured by
    // any snapshot.  Even with readonly=on set afterwards, the tool must refuse
    // rather than silently drop those writes.
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    write_pattern(source, 8 * MiB, 8 * MiB, 2);  // head now diverges from s0
    zfs({"set", "readonly=on", source});

    EXPECT_NE(migrate(source, "16k", {}), 0);         // refused
    EXPECT_TRUE(!dataset_exists(source + "-new"));    // nothing was created
    EXPECT_TRUE(!dataset_exists(source + "-old"));    // original untouched
}
