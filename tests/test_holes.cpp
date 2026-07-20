// Holes (FREE records) must be replicated: dest reads zeros and stays sparse.
#include <algorithm>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;
static const uint64_t HOLE_OFF = 16 * MiB;
static const uint64_t HOLE_LEN = 16 * MiB;

TEST(holes) {
    auto source = make_zvol("8k", "64M");
    write_pattern(source, 0, 64 * MiB, 1);
    snapshot(source, "full");

    punch_hole(source, HOLE_OFF, HOLE_LEN);
    snapshot(source, "holed");

    zfs({"set", "readonly=on", source});
    EXPECT_EQ(migrate(source, "32k", {"-v"}), 0);

    auto backup = source + "-old";

    // 1. Per-snapshot content matches (incl. the hole reading back as zeros).
    EXPECT_TRUE(snapshot_names(source) == (std::vector<std::string>{"full", "holed"}));
    for (const auto& snap : {"full", "holed"})
        EXPECT_TRUE(devices_equal(source + "@" + snap, backup + "@" + snap,
                                  volsize(source + "@" + snap)));

    // 2. The punched region reads back as zeros on the migrated volume.
    auto region = read_range(source + "@holed", HOLE_OFF, HOLE_LEN);
    EXPECT_TRUE(std::all_of(region.begin(), region.end(),
                            [](uint8_t b) { return b == 0; }));

    // 3. Sparseness: the migrated volume must not have re-materialised the hole
    //    as allocated zeros (compression=off, so a 16M hole should show clearly).
    EXPECT_TRUE(std::stoull(get_prop(source, "referenced")) < 56 * MiB);
}
