// Change a zvol's volblocksize while retaining all of its snapshots.
//
// volblocksize is fixed at creation time, and a plain `zfs send | zfs recv`
// recreates the volume with the source's block size.  The only way to change it
// is to write the data through a freshly created zvol so ZFS re-blocks it.  This
// tool recreates the full snapshot history on a new, correctly-blocked zvol by
// replaying each snapshot in creation order: for every step it asks ZFS which
// byte ranges changed (`zfs send [-i] | zstream dump -v`) and copies only those
// ranges from the source snapshot's block device to the destination device, then
// takes a matching snapshot.  Finally it swaps the new zvol into the original
// name (keeping the original as <name>-old).
//
// See README.md for the rationale, preconditions and limitations.

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "subprocess.hpp"

namespace {

struct MigrateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- zvol / send-stream constants ---------------------------------------- //

// A zvol objset stores volume data in object 1 and its properties (incl. volsize)
// in a ZAP at object 2; we reproduce object 1 and handle volsize out of band.
constexpr uint64_t ZVOL_OBJ = 1;
constexpr uint64_t ZVOL_ZAP_OBJ = 2;

// dnode geometry (OpenZFS include/sys/dnode.h): a 512-byte dnode slot in a 16 KiB
// meta-dnode block gives a fixed number of object slots per meta-dnode block.
constexpr uint64_t DNODE_SHIFT = 9;
constexpr uint64_t DNODE_BLOCK_SHIFT = 14;
constexpr uint64_t DNODES_PER_BLOCK = 1ull << (DNODE_BLOCK_SHIFT - DNODE_SHIFT);  // 32
constexpr uint64_t FREEOBJECTS_FIRST = ZVOL_ZAP_OBJ + 1;                          // 3

constexpr size_t CHUNK = 4 * 1024 * 1024;

const std::set<std::string> KNOWN_RECORDS = {
    "BEGIN", "END", "OBJECT", "OBJECT_RANGE", "FREEOBJECTS", "FREE",
    "WRITE", "WRITE_BYREF", "WRITE_EMBEDDED", "SPILL", "REDACT"};
const std::set<std::string> IGNORED_RECORDS = {"BEGIN", "END", "OBJECT"};
const std::set<std::string> SUMMARY_TOKENS = {"SUMMARY:", "SUMMARY", "Total",
                                              "Estimated"};

// Properties we manage ourselves or that are creation-time / key material and
// must not be copied from the source.  Everything else that is explicitly set is
// carried over (a denylist can't silently drop a property the way an allowlist can).
const std::set<std::string> UNSAFE_TO_COPY = {
    "volsize", "volblocksize", "readonly", "snapdev", "reservation",
    "refreservation", "encryption", "keyformat", "keylocation", "keystatus",
    "pbkdf2iters", "casesensitivity", "normalization", "utf8only"};

// ---- small string helpers ------------------------------------------------ //

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream is(s);
    while (std::getline(is, line)) out.push_back(line);
    return out;
}

// Extract `key = value` triples from a token list.
std::map<std::string, std::string> kv_pairs(const std::vector<std::string>& t) {
    std::map<std::string, std::string> m;
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if (t[i] == "=" && i > 0) m[t[i - 1]] = t[i + 1];
    return m;
}

uint64_t align_up(uint64_t v, uint64_t mult) { return (v + mult - 1) / mult * mult; }

// ---- zfs command wrappers ------------------------------------------------ //

std::string zfs_get(const std::string& ds, const std::string& prop) {
    return trim(sp::check_output({"zfs", "get", "-Hp", "-o", "value", prop, ds}));
}

std::vector<std::string> list_snapshots(const std::string& source) {
    std::string out = sp::check_output({"zfs", "list", "-H", "-d", "1", "-t",
                                        "snapshot", "-o", "name", "-s",
                                        "creation", source});
    std::vector<std::string> snaps;
    for (auto& l : split_lines(out))
        if (!trim(l).empty()) snaps.push_back(trim(l));
    return snaps;
}

// Every explicitly-set (local/received) property safe to recreate on the new zvol.
std::vector<std::pair<std::string, std::string>> carried_properties(
    const std::string& source) {
    std::string out = sp::check_output(
        {"zfs", "get", "-Hp", "-o", "property,value,source", "all", source});
    std::vector<std::pair<std::string, std::string>> props;
    for (const auto& line : split_lines(out)) {
        size_t t1 = line.find('\t');
        size_t t2 = line.rfind('\t');
        if (t1 == std::string::npos || t1 == t2) continue;
        std::string prop = line.substr(0, t1);
        std::string value = line.substr(t1 + 1, t2 - t1 - 1);
        std::string src = line.substr(t2 + 1);
        if ((src == "local" || src == "received") && !UNSAFE_TO_COPY.count(prop))
            props.emplace_back(prop, value);
    }
    return props;
}

std::vector<std::string> which_zstream() {
    if (sp::run_quiet({"sh", "-c", "command -v zstream"}) == 0)
        return {"zstream", "dump"};
    if (sp::run_quiet({"sh", "-c", "command -v zstreamdump"}) == 0)
        return {"zstreamdump"};
    throw MigrateError("neither 'zstream' nor 'zstreamdump' found in PATH");
}

std::string wait_for_device(const std::string& path, double timeout_s = 15.0) {
    sp::run_quiet({"udevadm", "settle"});
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<long>(timeout_s * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) return path;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw MigrateError("device node did not appear: " + path);
}

// ---- streaming parse of "zfs send | zstream dump -v" --------------------- //

enum class Op { None, Write, Free };
struct Change {
    Op op = Op::None;
    uint64_t offset = 0;
    int64_t length = 0;
};

// Classify a completed record for the zvol data object, raising on anything we do
// not understand (silently skipping could mean data loss).
Change classify(const std::string& rtype,
                const std::map<std::string, std::string>& f) {
    if (rtype.empty() || IGNORED_RECORDS.count(rtype)) return {};
    if (rtype == "REDACT")
        throw MigrateError("redacted send streams are not supported");

    if (rtype == "FREEOBJECTS") {
        // A zvol send frees exactly the unused tail of the first meta-dnode block,
        // objects [3, 32), leaving data (1) and props (2) intact.  Assert that.
        uint64_t firstobj = std::stoull(f.at("firstobj"));
        uint64_t numobjs = std::stoull(f.at("numobjs"));
        if (firstobj != FREEOBJECTS_FIRST ||
            firstobj + numobjs != DNODES_PER_BLOCK)
            throw MigrateError("unexpected FREEOBJECTS firstobj=" +
                               std::to_string(firstobj) + " numobjs=" +
                               std::to_string(numobjs) +
                               ": a plain zvol send must free exactly objects [" +
                               std::to_string(FREEOBJECTS_FIRST) + ", " +
                               std::to_string(DNODES_PER_BLOCK) + ")");
        return {};
    }

    if (rtype == "FREE" || rtype.rfind("WRITE", 0) == 0) {
        uint64_t obj = std::stoull(f.at("object"));
        if (obj == ZVOL_ZAP_OBJ) return {};  // props; volsize handled separately
        if (obj != ZVOL_OBJ)
            throw MigrateError("unexpected " + rtype + " to object " +
                               std::to_string(obj) + " (only the zvol data object " +
                               std::to_string(ZVOL_OBJ) + " is expected)");
        uint64_t offset = std::stoull(f.at("offset"));
        if (rtype == "FREE")
            return {Op::Free, offset, std::stoll(f.at("length"))};
        // WRITE / WRITE_EMBEDDED / WRITE_BYREF: we only need the changed range;
        // the bytes come from the snapshot device, so the encoding is moot.
        for (const char* key : {"logical_size", "lsize", "length"}) {
            auto it = f.find(key);
            if (it != f.end())
                return {Op::Write, offset, std::stoll(it->second)};
        }
        throw MigrateError(rtype + " record without a length field");
    }

    throw MigrateError("unsupported send record '" + rtype +
                       "': this does not look like a plain zvol send "
                       "(OBJECT_RANGE implies a raw send; SPILL implies SA overflow)");
}

void for_each_change(const std::vector<std::string>& send_cmd,
                     const std::vector<std::string>& zstream_cmd,
                     const std::function<void(const Change&)>& cb) {
    std::string cur_type;
    std::map<std::string, std::string> cur;
    bool have = false, ended = false;

    auto flush = [&] {
        if (have) {
            Change c = classify(cur_type, cur);
            if (c.op != Op::None) cb(c);
        }
    };

    sp::pipeline_for_each_line(send_cmd, zstream_cmd, [&](const std::string& line) {
        if (ended || trim(line).empty()) return;
        bool header = !std::isspace(static_cast<unsigned char>(line[0]));
        std::vector<std::string> toks = split_ws(line);
        if (header) {
            const std::string& rt = toks[0];
            if (!KNOWN_RECORDS.count(rt)) {
                if (SUMMARY_TOKENS.count(rt)) {
                    ended = true;
                    return;
                }
                throw MigrateError("unexpected send output line: " + trim(line));
            }
            flush();
            cur_type = rt;
            // The record keyword at toks[0] never sits adjacent to '=', so
            // scanning the whole line is equivalent to skipping it.
            cur = kv_pairs(toks);
            have = true;
            if (rt == "END") ended = true;
        } else {
            for (auto& [k, v] : kv_pairs(toks)) cur[k] = v;
        }
    });
    if (!ended) flush();
}

// ---- applying changed ranges to the destination device ------------------- //

class RangeApplier {
   public:
    RangeApplier(const std::string& src_dev, int dst_fd, uint64_t blocksize,
                 uint64_t volsize)
        : dst_fd_(dst_fd), bs_(blocksize), volsize_(volsize) {
        src_fd_ = open(src_dev.c_str(), O_RDONLY);
        if (src_fd_ < 0) throw MigrateError("cannot open source device " + src_dev);
    }
    ~RangeApplier() {
        if (src_fd_ >= 0) close(src_fd_);
    }

    void write(uint64_t offset, uint64_t length) {
        uint64_t end = offset + length;
        if (have_pending_ && pend_end_ == offset) {  // contiguous: extend
            pend_end_ = end;
        } else {
            flush();
            pend_start_ = offset;
            pend_end_ = end;
            have_pending_ = true;
        }
    }

    void flush() {
        if (!have_pending_) return;
        uint64_t pos = pend_start_, end = pend_end_;
        have_pending_ = false;
        while (pos < end) {
            size_t want = std::min<uint64_t>(CHUNK, end - pos);
            ssize_t got = pread(src_fd_, buf_.data(), want, pos);
            if (got <= 0) break;  // past end of source snapshot; rest are holes
            if (pwrite(dst_fd_, buf_.data(), got, pos) != got)
                throw MigrateError("short write to destination");
            bytes_written_ += static_cast<uint64_t>(got);
            pos += static_cast<uint64_t>(got);
        }
    }

    void free(uint64_t offset, int64_t length) {
        flush();
        int64_t len = length;
        if (len < 0 || offset + static_cast<uint64_t>(len) > volsize_)
            len = static_cast<int64_t>(volsize_) - static_cast<int64_t>(offset);
        if (len <= 0) return;
        uint64_t ulen = static_cast<uint64_t>(len);
        bytes_freed_ += ulen;
        uint64_t a = align_up(offset, bs_);
        uint64_t b = (offset + ulen) / bs_ * bs_;
        if (a < b) {
            discard(a, b - a);
            zero(offset, a - offset);
            zero(b, offset + ulen - b);
        } else {
            zero(offset, ulen);  // smaller than a dest block: zero it
        }
    }

    uint64_t bytes_written() const { return bytes_written_; }
    uint64_t bytes_freed() const { return bytes_freed_; }

   private:
    void discard(uint64_t offset, uint64_t length) {
        uint64_t range[2] = {offset, length};
        if (ioctl(dst_fd_, BLKDISCARD, &range) != 0)
            throw MigrateError("BLKDISCARD failed");
    }
    void zero(uint64_t offset, uint64_t length) {
        static const std::vector<char> zeros(CHUNK, 0);
        while (length > 0) {
            size_t n = std::min<uint64_t>(CHUNK, length);
            if (pwrite(dst_fd_, zeros.data(), n, offset) != static_cast<ssize_t>(n))
                throw MigrateError("short zero-write to destination");
            offset += n;
            length -= n;
        }
    }

    int src_fd_ = -1;
    int dst_fd_;
    uint64_t bs_;
    uint64_t volsize_;
    bool have_pending_ = false;
    uint64_t pend_start_ = 0, pend_end_ = 0;
    uint64_t bytes_written_ = 0, bytes_freed_ = 0;
    std::vector<char> buf_ = std::vector<char>(CHUNK);
};

// ---- migration ----------------------------------------------------------- //

struct Options {
    std::string source;
    std::string volblocksize;
    std::string dest;
    std::string backup_suffix = "-old";
    bool no_swap = false;
    bool keep_backup = true;
    bool force = false;
    bool dry_run = false;
    bool verbose = false;
};

Options g_opts;

void log(const std::string& msg) {
    if (g_opts.verbose)
        std::cerr << "[zvol-change-volblocksize] " << msg << std::endl;
}

void run_mutate(const std::vector<std::string>& args) {
    if (g_opts.dry_run) {
        std::cerr << "DRY-RUN: " << sp::join(args) << std::endl;
        return;
    }
    log("+ " + sp::join(args));
    sp::check_call(args);
}

uint64_t parse_size(const std::string& text) {
    std::string t = trim(text);
    if (t.empty()) throw MigrateError("empty size");
    char suffix = static_cast<char>(std::tolower(t.back()));
    if (std::isdigit(static_cast<unsigned char>(suffix)))
        return std::stoull(t);
    static const std::map<char, uint64_t> units = {
        {'b', 1}, {'k', 1024}, {'m', 1024ull * 1024}, {'g', 1024ull * 1024 * 1024}};
    auto it = units.find(suffix);
    if (it == units.end()) throw MigrateError("invalid size: " + text);
    return static_cast<uint64_t>(std::stod(t.substr(0, t.size() - 1)) * it->second);
}

void create_dest(const std::string& dest, uint64_t blocksize, uint64_t volsize) {
    std::vector<std::string> cmd = {"zfs",  "create",  "-s", "-V",
                                    std::to_string(volsize), "-b",
                                    std::to_string(blocksize), "-o",
                                    "snapdev=visible"};
    for (auto& [prop, val] : carried_properties(g_opts.source)) {
        cmd.push_back("-o");
        cmd.push_back(prop + "=" + val);
    }
    cmd.push_back(dest);
    log("creating destination: " + dest);
    run_mutate(cmd);
}

void replay(const std::string& dest, uint64_t blocksize,
            const std::vector<std::string>& snapshots,
            const std::vector<std::string>& zstream_cmd) {
    std::string dst_dev = "/dev/zvol/" + dest;
    std::string prev;
    for (const auto& snap : snapshots) {
        std::string shortname = snap.substr(snap.find('@') + 1);
        uint64_t snap_volsize = std::stoull(zfs_get(snap, "volsize"));
        uint64_t dst_volsize = align_up(snap_volsize, blocksize);
        log("snapshot " + shortname + ": volsize=" + std::to_string(snap_volsize) +
            " (dest " + std::to_string(dst_volsize) + ")");
        run_mutate({"zfs", "set", "volsize=" + std::to_string(dst_volsize), dest});

        if (g_opts.dry_run) {
            std::cerr << "DRY-RUN: replay "
                      << (prev.empty() ? "full" : "incremental from " + prev)
                      << " -> " << snap << ", snapshot " << dest << "@" << shortname
                      << std::endl;
            prev = snap;
            continue;
        }

        std::string src_dev = wait_for_device("/dev/zvol/" + snap);
        wait_for_device(dst_dev);
        int dst_fd = open(dst_dev.c_str(), O_RDWR);
        if (dst_fd < 0) throw MigrateError("cannot open destination " + dst_dev);

        {
            RangeApplier applier(src_dev, dst_fd, blocksize, dst_volsize);
            std::vector<std::string> send_cmd =
                prev.empty() ? std::vector<std::string>{"zfs", "send", snap}
                             : std::vector<std::string>{"zfs", "send", "-i", prev, snap};
            log("+ " + sp::join(send_cmd) + " | " + sp::join(zstream_cmd));
            for_each_change(send_cmd, zstream_cmd, [&](const Change& c) {
                if (c.op == Op::Write)
                    applier.write(c.offset, static_cast<uint64_t>(c.length));
                else
                    applier.free(c.offset, c.length);
            });
            applier.flush();
            fsync(dst_fd);
            log("  wrote " + std::to_string(applier.bytes_written()) +
                " bytes, freed " + std::to_string(applier.bytes_freed()) + " bytes");
        }
        close(dst_fd);
        run_mutate({"zfs", "snapshot", dest + "@" + shortname});
        prev = snap;
    }
}

void swap_names(const std::string& dest, const std::string& backup) {
    std::string ro = zfs_get(g_opts.source, "readonly");
    std::string snapdev = zfs_get(g_opts.source, "snapdev");
    log("renaming " + g_opts.source + " -> " + backup + ", " + dest + " -> " +
        g_opts.source);
    run_mutate({"zfs", "rename", g_opts.source, backup});
    run_mutate({"zfs", "rename", dest, g_opts.source});
    run_mutate({"zfs", "set", "readonly=" + ro, g_opts.source});
    run_mutate({"zfs", "set", "snapdev=" + snapdev, g_opts.source});
}

void migrate() {
    uint64_t blocksize = parse_size(g_opts.volblocksize);
    if (blocksize < 512u || (blocksize & (blocksize - 1)) != 0 ||
        blocksize > 128u * 1024)
        throw MigrateError("volblocksize must be a power of two in [512, 128K]");

    if (zfs_get(g_opts.source, "type") != "volume")
        throw MigrateError(g_opts.source + " is not a zvol");
    if (zfs_get(g_opts.source, "readonly") != "on" && !g_opts.force)
        throw MigrateError(g_opts.source +
                           " is not readonly=on; set it (so the newest snapshot is "
                           "the live head) or pass --force");

    std::vector<std::string> snapshots = list_snapshots(g_opts.source);
    if (snapshots.empty())
        throw MigrateError(g_opts.source +
                           " has no snapshots; nothing to retain");

    std::string dest = g_opts.dest.empty() ? g_opts.source + "-new" : g_opts.dest;
    std::string backup = g_opts.source + g_opts.backup_suffix;
    std::vector<std::string> zstream_cmd = which_zstream();
    zstream_cmd.push_back("-v");

    log("migrating " + g_opts.source + " (" + std::to_string(snapshots.size()) +
        " snapshots) to volblocksize=" + std::to_string(blocksize) +
        ", dest=" + dest);

    uint64_t first_volsize =
        align_up(std::stoull(zfs_get(snapshots.front(), "volsize")), blocksize);
    create_dest(dest, blocksize, first_volsize);
    replay(dest, blocksize, snapshots, zstream_cmd);

    if (g_opts.no_swap) {
        std::cout << "Done. New zvol left at " << dest << " (no swap requested)."
                  << std::endl;
        return;
    }
    swap_names(dest, backup);
    std::cout << "Done. " << g_opts.source
              << " now has volblocksize=" << blocksize << ". Original preserved as "
              << backup << "." << std::endl;
    if (!g_opts.keep_backup && !g_opts.dry_run) {
        log("destroying backup " + backup);
        sp::check_call({"zfs", "destroy", "-r", backup});
        std::cout << "Backup " << backup << " destroyed as requested." << std::endl;
    }
}

const char* USAGE =
    "usage: zvol-change-volblocksize <source-zvol> <new-volblocksize> [options]\n"
    "  --dest NAME           intermediate name (default: <source>-new)\n"
    "  --backup-suffix S     suffix for the preserved original (default: -old)\n"
    "  --no-swap             leave the result under --dest; do not rename\n"
    "  --keep-backup         keep the original as backup (default)\n"
    "  --destroy-backup      destroy the original after a successful swap\n"
    "  --force               bypass the readonly=on precondition check\n"
    "  --dry-run             print planned actions without changing anything\n"
    "  -v, --verbose\n";

bool parse_args(int argc, char** argv) {
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw MigrateError("missing value for " + a);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::cout << USAGE;
            return false;
        } else if (a == "--dest") {
            g_opts.dest = next();
        } else if (a == "--backup-suffix") {
            g_opts.backup_suffix = next();
        } else if (a == "--no-swap") {
            g_opts.no_swap = true;
        } else if (a == "--keep-backup") {
            g_opts.keep_backup = true;
        } else if (a == "--destroy-backup") {
            g_opts.keep_backup = false;
        } else if (a == "--force") {
            g_opts.force = true;
        } else if (a == "--dry-run") {
            g_opts.dry_run = true;
        } else if (a == "-v" || a == "--verbose") {
            g_opts.verbose = true;
        } else if (!a.empty() && a[0] == '-') {
            throw MigrateError("unknown option: " + a);
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() != 2) {
        std::cerr << USAGE;
        throw MigrateError("expected <source-zvol> and <new-volblocksize>");
    }
    g_opts.source = positional[0];
    g_opts.volblocksize = positional[1];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (!parse_args(argc, argv)) return 0;
        migrate();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
