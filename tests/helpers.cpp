#include "helpers.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "subprocess.hpp"
#include "testing.hpp"

namespace th {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void zfs(const std::vector<std::string>& args) {
    std::vector<std::string> cmd = {"zfs"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    sp::check_call(cmd);
}

std::string get_prop(const std::string& dataset, const std::string& prop) {
    return trim(
        sp::check_output({"zfs", "get", "-Hp", "-o", "value", prop, dataset}));
}

uint64_t volsize(const std::string& dataset) {
    return std::stoull(get_prop(dataset, "volsize"));
}

bool dataset_exists(const std::string& dataset) {
    return sp::run_quiet({"zfs", "list", dataset}) == 0;
}

std::string dev_path(const std::string& dataset) {
    // Snapshot device nodes only appear when snapdev=visible on the parent zvol.
    // The tool restores the source's original snapdev after migrating, so tests
    // must (re-)enable visibility on whatever dataset they want to inspect.
    auto at = dataset.find('@');
    if (at != std::string::npos)
        sp::run_quiet({"zfs", "set", "snapdev=visible", dataset.substr(0, at)});
    std::string path = "/dev/zvol/" + dataset;
    sp::run_quiet({"udevadm", "settle"});
    for (int i = 0; i < 150; ++i) {
        if (std::filesystem::exists(path)) return path;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("device never appeared: " + path);
}

void snapshot(const std::string& dataset, const std::string& name) {
    zfs({"snapshot", dataset + "@" + name});
}

void set_volsize(const std::string& dataset, uint64_t size) {
    zfs({"set", "volsize=" + std::to_string(size), dataset});
}

std::vector<std::string> snapshot_names(const std::string& dataset) {
    std::string out = sp::check_output({"zfs", "list", "-H", "-d", "1", "-t",
                                        "snapshot", "-o", "name", "-s",
                                        "creation", dataset});
    std::vector<std::string> names;
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        line = trim(line);
        if (line.empty()) continue;
        names.push_back(line.substr(line.find('@') + 1));
    }
    return names;
}

std::string make_zvol(const std::string& volblocksize, const std::string& size,
                      const std::string& name) {
    // Deliberately do NOT set snapdev here, so the default-hidden path (which the
    // tool must handle by toggling it itself) is what's exercised.
    std::string dataset = testing::g_pool + "/" + name;
    zfs({"create", "-V", size, "-b", volblocksize, dataset});
    return dataset;
}

static int open_dev(const std::string& dataset, int flags) {
    int fd = open(dev_path(dataset).c_str(), flags);
    if (fd < 0) throw std::runtime_error("cannot open device for " + dataset);
    return fd;
}

using sp::pread_full;

void write_pattern(const std::string& dataset, uint64_t offset, uint64_t size,
                   uint64_t seed) {
    std::vector<uint8_t> data(size);
    std::mt19937_64 rng(seed);
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t v = rng();
        std::memcpy(&data[i], &v, std::min<uint64_t>(8, size - i));
    }
    int fd = open_dev(dataset, O_RDWR);
    ssize_t n = pwrite(fd, data.data(), size, offset);
    fsync(fd);
    close(fd);
    if (n != static_cast<ssize_t>(size))
        throw std::runtime_error("short write to " + dataset);
}

void punch_hole(const std::string& dataset, uint64_t offset, uint64_t length) {
    sp::check_call({"blkdiscard", "--offset", std::to_string(offset), "--length",
                    std::to_string(length), dev_path(dataset)});
}

std::vector<uint8_t> read_range(const std::string& dataset, uint64_t offset,
                                uint64_t size) {
    std::vector<uint8_t> out(size);
    int fd = open_dev(dataset, O_RDONLY);
    bool ok = pread_full(fd, out.data(), size, offset);
    close(fd);
    if (!ok) throw std::runtime_error("short read from " + dataset);
    return out;
}

bool devices_equal(const std::string& a, const std::string& b, uint64_t size) {
    int fa = open_dev(a, O_RDONLY);
    int fb = open_dev(b, O_RDONLY);
    const size_t CH = 4 * 1024 * 1024;
    std::vector<char> ba(CH), bb(CH);
    bool equal = true;
    for (uint64_t pos = 0; pos < size && equal;) {
        size_t n = std::min<uint64_t>(CH, size - pos);
        if (!pread_full(fa, ba.data(), n, pos) ||
            !pread_full(fb, bb.data(), n, pos)) {
            equal = false;
            break;
        }
        if (std::memcmp(ba.data(), bb.data(), n) != 0) equal = false;
        pos += n;
    }
    close(fa);
    close(fb);
    return equal;
}

int migrate(const std::string& source, const std::string& volblocksize,
            const std::vector<std::string>& extra) {
    std::vector<std::string> cmd = {testing::g_tool, source, volblocksize};
    cmd.insert(cmd.end(), extra.begin(), extra.end());
    return sp::call_status(cmd);
}

}  // namespace th
