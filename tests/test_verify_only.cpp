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
