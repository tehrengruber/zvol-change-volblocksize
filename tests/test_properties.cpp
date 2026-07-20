// The migrated zvol is created directly with the source's *final* properties
// (property changes across snapshots are not replayed), and all explicitly-set
// properties -- including user properties -- are carried over.
#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

TEST(properties) {
    auto source = make_zvol("8k", "32M");

    // A property that CHANGES between snapshots: only the final value should end
    // up on the migrated zvol, since it is created once with the current props.
    zfs({"set", "compression=off", source});
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");

    zfs({"set", "compression=lz4", source});
    zfs({"set", "checksum=sha256", source});
    zfs({"set", "logbias=throughput", source});
    zfs({"set", "com.example:role=database", source});  // user property
    write_pattern(source, 8 * MiB, 8 * MiB, 2);
    snapshot(source, "s1");

    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "16k"), 0);

    // After the swap the original name refers to the migrated zvol.
    EXPECT_EQ(std::stoull(get_prop(source, "volblocksize")), 16u * 1024);
    EXPECT_EQ(get_prop(source, "compression"), std::string("lz4"));  // final, not off
    EXPECT_EQ(get_prop(source, "checksum"), std::string("sha256"));
    EXPECT_EQ(get_prop(source, "logbias"), std::string("throughput"));
    // User properties must survive too (an allowlist would silently drop these).
    EXPECT_EQ(get_prop(source, "com.example:role"), std::string("database"));
    // Managed property is set by the tool, not copied from the 8k source.
    EXPECT_NE(std::stoull(get_prop(source, "volblocksize")), 8u * 1024);

    // Sanity: content still matches the original per snapshot.
    for (const auto& snap : snapshot_names(source))
        EXPECT_TRUE(devices_equal(source + "@" + snap, source + "-old@" + snap,
                                  volsize(source + "@" + snap)));
}
