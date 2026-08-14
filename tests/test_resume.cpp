// --resume-from continues an interrupted run: replay from a given source snapshot
// onto an existing destination that already holds the earlier snapshots.
#include <string>
#include <vector>

#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

TEST(resume_from) {
    auto source = make_zvol("8k", "32M");
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    write_pattern(source, 8 * MiB, 8 * MiB, 2);
    snapshot(source, "s1");
    zfs({"set", "readonly=on", source});

    // "Prior run": migrate s0,s1 into a partial destination (like an interrupted
    // migration that got as far as s1).
    std::string dest = source + "-new";
    EXPECT_EQ(migrate(source, "16k", {"--no-swap"}), 0);
    EXPECT_TRUE(snapshot_names(dest) == (std::vector<std::string>{"s0", "s1"}));

    // The source gains a new snapshot after that run.
    zfs({"set", "readonly=off", source});
    write_pattern(source, 16 * MiB, 8 * MiB, 3);
    snapshot(source, "s2");
    zfs({"set", "readonly=on", source});

    // Resume: replay s2 onto the existing dest (which already has s0, s1).
    // Use the @-qualified form to exercise snapshot-name normalization.
    EXPECT_EQ(migrate(source, "16k", {"--no-swap", "--resume-from", "@s2"}), 0);
    EXPECT_TRUE(snapshot_names(dest) ==
                (std::vector<std::string>{"s0", "s1", "s2"}));

    // Every snapshot's content matches the source.
    for (const auto& s : {"s0", "s1", "s2"})
        EXPECT_TRUE(devices_equal(dest + "@" + s, source + "@" + s,
                                  volsize(source + "@" + s)));

    // Resuming from a snapshot the target doesn't have the predecessors for fails.
    EXPECT_NE(migrate(source, "16k",
                      {"--no-swap", "--dest", source + "-fresh", "--resume-from", "s1"}),
              0);
}
