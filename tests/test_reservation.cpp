// A thick-provisioned source (refreservation set) must come out thick, not
// silently converted to thin.  make_zvol creates without -s, so it is thick.
#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

static bool is_set(const std::string& v) { return v != "0" && v != "none"; }

TEST(reservation_preserved) {
    auto source = make_zvol("8k", "32M");
    EXPECT_TRUE(is_set(get_prop(source, "refreservation")));  // thick to begin with
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    zfs({"set", "readonly=on", source});

    EXPECT_EQ(migrate(source, "16k", {}), 0);

    // The migrated volume must be thick again.
    EXPECT_TRUE(is_set(get_prop(source, "refreservation")));
}
