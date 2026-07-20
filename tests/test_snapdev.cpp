// The source defaults to snapdev=hidden (make_zvol no longer forces it).  The
// tool must toggle it visible for the migration and restore the original value
// afterwards on both the migrated volume and the backup.
#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

TEST(snapdev_hidden_restored) {
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    EXPECT_EQ(get_prop(source, "snapdev"), std::string("hidden"));  // default
    zfs({"set", "readonly=on", source});

    EXPECT_EQ(migrate(source, "16k", {}), 0);

    // Check snapdev *before* any device comparison (dev_path would re-enable it).
    EXPECT_EQ(get_prop(source, "snapdev"), std::string("hidden"));           // migrated
    EXPECT_EQ(get_prop(source + "-old", "snapdev"), std::string("hidden"));  // backup

    // And the content is correct (this re-enables snapdev via dev_path).
    EXPECT_TRUE(devices_equal(source + "@s0", source + "-old@s0",
                              volsize(source + "-old@s0")));
}
