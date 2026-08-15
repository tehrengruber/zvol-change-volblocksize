// Resume safety: refuse to resume onto a destination that has been written to
// since its newest migrated snapshot (it would corrupt the resumed snapshot);
// and a whole-run advisory flock prevents two concurrent migrations of one dest.
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "helpers.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

TEST(resume_refuses_written_dest) {
    auto src = make_zvol("8k", "24M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    write_pattern(src, 8 * MiB, 8 * MiB, 2);
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});

    std::string dest = src + "-r";
    EXPECT_EQ(migrate(src, "16k", {"--no-swap", "--dest", dest}), 0);  // builds s0,s1

    // Source gains s2.
    zfs({"set", "readonly=off", src});
    write_pattern(src, 16 * MiB, 8 * MiB, 3);
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});

    // Something writes to the destination since its newest snapshot...
    write_pattern(dest, 0, 1 * MiB, 999);
    // ...so resume refuses (the live volume diverged from the resume base)...
    EXPECT_NE(
        migrate(src, "16k", {"--no-swap", "--dest", dest, "--resume-from", "s2"}), 0);
    // ...unless --force overrides.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--resume-from", "s2", "--force"}),
              0);
    EXPECT_TRUE(snapshot_names(dest) == (std::vector<std::string>{"s0", "s1", "s2"}));
}

// The tool's per-destination lock file (mirrors FileLock in main.cpp).
static std::string lock_path(const std::string& dest) {
    std::string key;
    for (char c : dest) key += (c == '/' || c == '@') ? '_' : c;
    return "/run/lock/zvol-change-volblocksize-" + key + ".lock";
}

TEST(dest_lock_blocks_concurrent) {
    auto src = make_zvol("8k", "16M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    zfs({"set", "readonly=on", src});
    std::string dest = src + "-locked";

    // Hold the destination lock from this (test) process; the tool runs as a
    // separate process, so its flock request is denied.  Different open file
    // descriptions conflict even across processes.
    int fd = open(lock_path(dest).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    EXPECT_TRUE(fd >= 0);
    EXPECT_TRUE(flock(fd, LOCK_EX | LOCK_NB) == 0);

    // While held, a migration to that destination fails fast.
    EXPECT_NE(migrate(src, "16k", {"--no-swap", "--dest", dest}), 0);

    // Once released, it succeeds.
    close(fd);
    EXPECT_EQ(migrate(src, "16k", {"--no-swap", "--dest", dest}), 0);
}
