// An encrypted source under a plaintext destination parent must be refused (it
// would silently produce plaintext), unless --allow-decrypt is given.
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

#include "helpers.hpp"
#include "subprocess.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

TEST(refuses_silent_decrypt) {
    // Write a 32-byte raw key and create an encrypted parent.  If encryption is
    // unavailable in this environment, treat the test as skipped.
    std::string keyfile = "/var/tmp/zcvb-key-refuses_silent_decrypt";
    {
        std::ofstream f(keyfile, std::ios::binary);
        std::mt19937_64 rng(42);
        std::vector<char> key(32);
        for (auto& c : key) c = static_cast<char>(rng());
        f.write(key.data(), key.size());
    }
    std::string enc_parent = testing::g_pool + "/enc";
    try {
        zfs({"create", "-o", "encryption=on", "-o", "keyformat=raw", "-o",
             "keylocation=file://" + keyfile, enc_parent});
    } catch (const std::exception&) {
        std::remove(keyfile.c_str());
        return;  // encryption unsupported here; skip
    }

    std::string source = enc_parent + "/vol";
    zfs({"create", "-V", "32M", "-b", "8k", source});
    write_pattern(source, 0, 8 * MiB, 1);
    snapshot(source, "s0");
    zfs({"set", "readonly=on", source});

    std::string plain_dest = testing::g_pool + "/plainvol";  // under the plaintext root

    // Refused by default; nothing created.
    EXPECT_NE(migrate(source, "16k", {"--dest", plain_dest}), 0);
    EXPECT_TRUE(!dataset_exists(plain_dest));

    // Accepted with --allow-decrypt (use --no-swap to avoid a cross-parent rename).
    EXPECT_EQ(migrate(source, "16k",
                      {"--dest", plain_dest, "--allow-decrypt", "--no-swap"}),
              0);
    EXPECT_EQ(get_prop(plain_dest, "encryption"), std::string("off"));

    std::remove(keyfile.c_str());
}
