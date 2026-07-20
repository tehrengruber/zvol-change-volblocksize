// User holds on source snapshots must be carried over to the migrated snapshots.
#include <string>

#include "helpers.hpp"
#include "subprocess.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

static bool has_hold(const std::string& snap, const std::string& tag) {
    return sp::check_output({"zfs", "holds", "-H", snap}).find(tag) !=
           std::string::npos;
}

TEST(holds_transferred) {
    auto source = make_zvol("8k", "16M");
    write_pattern(source, 0, 4 * MiB, 1);
    snapshot(source, "s0");
    zfs({"hold", "keep", source + "@s0"});  // protect s0 from destruction
    write_pattern(source, 4 * MiB, 4 * MiB, 2);
    snapshot(source, "s1");
    zfs({"set", "readonly=on", source});

    EXPECT_EQ(migrate(source, "16k", {}), 0);

    // The migrated s0 carries the hold; s1 (which had none) does not.
    EXPECT_TRUE(has_hold(source + "@s0", "keep"));
    EXPECT_TRUE(!has_hold(source + "@s1", "keep"));
}
