#include "testing.hpp"

#include <unistd.h>

#include <filesystem>
#include <iostream>

#include "subprocess.hpp"

namespace testing {

std::map<std::string, TestFn>& registry() {
    static std::map<std::string, TestFn> r;
    return r;
}

Registrar::Registrar(const std::string& name, TestFn fn) {
    registry()[name] = std::move(fn);
}

std::string g_pool;
std::string g_tool;

}  // namespace testing

static std::string g_vdev;

static void setup_pool(const std::string& test) {
    testing::g_pool = "tp_" + test;
    g_vdev = "/var/tmp/zcvb-" + test + ".img";
    sp::run_quiet({"zpool", "destroy", "-f", testing::g_pool});
    sp::check_call({"truncate", "-s", "4G", g_vdev});
    // compression=off so holes (discard) are distinguishable from zero-filled
    // blocks when asserting sparseness.
    sp::check_call(
        {"zpool", "create", "-f", "-O", "compression=off", testing::g_pool, g_vdev});
}

static void teardown_pool() {
    sp::run_quiet({"zpool", "destroy", "-f", testing::g_pool});
    std::error_code ec;
    std::filesystem::remove(g_vdev, ec);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: zvol_tests <test-name> <tool-path>\n";
        return 2;
    }
    std::string name = argv[1];
    testing::g_tool = argv[2];

    auto& r = testing::registry();
    auto it = r.find(name);
    if (it == r.end()) {
        std::cerr << "unknown test: " << name << "\n";
        return 2;
    }

    // Integration tests need root and a working ZFS; otherwise report SKIP (77).
    if (geteuid() != 0 ||
        sp::run_quiet({"sh", "-c", "command -v zpool"}) != 0) {
        std::cerr << "SKIP " << name << " (needs root + zfs)\n";
        return 77;
    }

    setup_pool(name);
    int rc = 0;
    try {
        it->second();
        std::cout << "PASS " << name << "\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << name << ": " << e.what() << "\n";
        rc = 1;
    }
    teardown_pool();
    return rc;
}
