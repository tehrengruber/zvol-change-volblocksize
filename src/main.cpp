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
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
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

// Set by the SIGINT handler and checked at safe points so Ctrl-C unwinds through
// the normal exception path (cleanup note prints; pipeline children are reaped).
volatile std::sig_atomic_t g_interrupted = 0;
void on_sigint(int) { g_interrupted = 1; }
void throw_if_interrupted() {
    if (g_interrupted) throw MigrateError("interrupted");
}

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

// Parse an unsigned integer from ZFS/zstream output, with an actionable error
// (rather than a bare std::stoull "stoull" exception) if it isn't a number.
uint64_t to_u64(const std::string& s, const std::string& what) {
    uint64_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size())
        throw MigrateError(what + ": expected a number but got '" + s + "'");
    return v;
}

// Same, but for a possibly-signed value (FREE length can be -1); returns int64.
int64_t to_i64(const std::string& s, const std::string& what) {
    int64_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size())
        throw MigrateError(what + ": expected a number but got '" + s + "'");
    return v;
}

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
    throw MigrateError("device node did not appear: " + path +
                       " (is snapdev=visible set, and is udev running?)");
}

// ---- streaming parse of "zfs send | zstream dump -v" --------------------- //

enum class Op { None, Write, Free };
struct Change {
    Op op = Op::None;
    uint64_t offset = 0;
    // Length is signed only because of one case: a FREE can carry length == -1,
    // ZFS's DMU_OBJECT_END sentinel (UINT64_MAX, printed as -1 by zstream) meaning
    // "free from offset to the end of the volume" -- e.g. the trailing free of a
    // full send or the truncated tail of a volsize shrink.  RangeApplier::free()
    // normalizes it to volsize - offset.  WRITE lengths are always positive.
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
        uint64_t firstobj = to_u64(f.at("firstobj"), "FREEOBJECTS firstobj");
        uint64_t numobjs = to_u64(f.at("numobjs"), "FREEOBJECTS numobjs");
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
        uint64_t obj = to_u64(f.at("object"), rtype + " object");
        if (obj == ZVOL_ZAP_OBJ) return {};  // props; volsize handled separately
        if (obj != ZVOL_OBJ)
            throw MigrateError("unexpected " + rtype + " to object " +
                               std::to_string(obj) + " (only the zvol data object " +
                               std::to_string(ZVOL_OBJ) + " is expected)");
        uint64_t offset = to_u64(f.at("offset"), rtype + " offset");
        if (rtype == "FREE")
            return {Op::Free, offset, to_i64(f.at("length"), "FREE length")};
        // WRITE / WRITE_EMBEDDED / WRITE_BYREF: we only need the changed range;
        // the bytes come from the snapshot device, so the encoding is moot.
        for (const char* key : {"logical_size", "lsize", "length"}) {
            auto it = f.find(key);
            if (it != f.end())
                return {Op::Write, offset, to_i64(it->second, "WRITE size")};
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
    // Every record we act on (WRITE/FREE/FREEOBJECTS) is printed on a single line
    // as `KEYWORD field = value ...`.  We tell a record header from a field
    // continuation line (e.g. BEGIN's, printed as `field = value`) by grammar --
    // a continuation line's second token is '=' -- so parsing does not depend on
    // indentation.  classify() ignores/handles/rejects each keyword.
    sp::pipeline_for_each_line(send_cmd, zstream_cmd, [&](const std::string& line) {
        std::vector<std::string> toks = split_ws(line);
        if (toks.empty()) return;
        if (toks.size() > 1 && toks[1] == "=") return;  // field continuation line
        if (SUMMARY_TOKENS.count(toks[0])) return;       // trailing summary block
        Change c = classify(toks[0], kv_pairs(toks));
        if (c.op != Op::None) cb(c);
    });
}

// ---- progress reporting -------------------------------------------------- //

std::string human(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string human_time(double seconds) {
    long t = static_cast<long>(seconds + 0.5);
    if (t < 60) return std::to_string(t) + "s";
    if (t < 3600)
        return std::to_string(t / 60) + "m" + std::to_string(t % 60) + "s";
    return std::to_string(t / 3600) + "h" + std::to_string((t % 3600) / 60) + "m";
}

// A one-line progress display on stderr showing percentage, bytes, transfer speed
// (over the last ~10 seconds) and ETA.  Drawn only when stderr is a terminal (so
// scripts/logs stay clean) and redrawn at most once per ~10 MB of progress,
// expressed as a whole number of destination blocks.  `total` is an estimate
// (0 = unknown -> no % / ETA).
class Progress {
   public:
    Progress(std::string label, uint64_t total, uint64_t blocksize)
        : label_(std::move(label)),
          total_(total),
          tty_(isatty(STDERR_FILENO)),
          start_(std::chrono::steady_clock::now()) {
        uint64_t blocks = std::max<uint64_t>(1, (10ull * 1024 * 1024) / blocksize);
        redraw_bytes_ = blocks * blocksize;  // ~10 MB, rounded to a block boundary
    }

    void update(uint64_t done) {
        if (!tty_) return;
        auto now = std::chrono::steady_clock::now();
        // Keep a sliding window of samples so the displayed speed reflects the last
        // ~10 seconds rather than the whole-run average.
        window_.emplace_back(now, done);
        while (window_.size() > 1 &&
               now - window_.front().first > std::chrono::seconds(10))
            window_.pop_front();

        if (drawn_ && done < total_ && done - last_drawn_ < redraw_bytes_) return;
        drawn_ = true;
        last_drawn_ = done;

        double wsecs =
            std::chrono::duration<double>(now - window_.front().first).count();
        uint64_t rate =
            wsecs > 0
                ? static_cast<uint64_t>((done - window_.front().second) / wsecs)
                : 0;
        std::ostringstream l;
        l << '\r' << label_ << ' ';
        if (total_)
            l << std::min<uint64_t>(100, done * 100 / total_) << "% (" << human(done)
              << " / ~" << human(total_) << ')';
        else
            l << human(done);
        l << " at " << human(rate) << "/s";
        if (total_ && rate > 0 && done < total_)
            l << ", ETA " << human_time(static_cast<double>(total_ - done) / rate);
        l << "        ";
        std::cerr << l.str() << std::flush;
    }
    void finish(uint64_t done) {
        if (!tty_) return;
        double secs = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - start_).count();
        uint64_t rate = secs > 0 ? static_cast<uint64_t>(done / secs) : 0;
        std::cerr << '\r' << label_ << ": copied " << human(done) << " in "
                  << human_time(secs) << " (" << human(rate) << "/s)          \n"
                  << std::flush;
    }

   private:

    std::string label_;
    uint64_t total_;
    bool tty_;
    std::chrono::steady_clock::time_point start_;
    uint64_t redraw_bytes_ = 0;
    uint64_t last_drawn_ = 0;
    bool drawn_ = false;
    // (timestamp, cumulative bytes) samples within the trailing ~10s window.
    std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> window_;
};

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
            throw_if_interrupted();
            size_t want = std::min<uint64_t>(CHUNK, end - pos);
            ssize_t got = pread(src_fd_, buf_.data(), want, pos);
            if (got < 0)
                throw MigrateError("read error on source device at offset " +
                                   std::to_string(pos) + ": " +
                                   std::strerror(errno));
            if (got == 0)
                throw MigrateError(
                    "unexpected EOF on source device at offset " +
                    std::to_string(pos) +
                    " (a WRITE record lies past the end of the snapshot; the send "
                    "stream and the device disagree)");
            if (pwrite(dst_fd_, buf_.data(), got, pos) != got)
                throw MigrateError("short write to destination at offset " +
                                   std::to_string(pos));
            bytes_written_ += static_cast<uint64_t>(got);
            pos += static_cast<uint64_t>(got);
            if (on_progress_) on_progress_(bytes_written_);
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
    void set_progress(std::function<void(uint64_t)> cb) {
        on_progress_ = std::move(cb);
    }

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
    std::function<void(uint64_t)> on_progress_;
};

// ---- migration ----------------------------------------------------------- //

// What to byte-compare between the migrated zvol and the original after copying.
enum class VerifyMode { None, Head, All };

struct Options {
    std::string source;
    std::string volblocksize;
    std::string dest;
    std::string backup_suffix = "-old";
    std::string resume_from;  // resume replay from this source snapshot
    bool no_swap = false;
    bool keep_backup = true;
    bool force = false;
    bool allow_decrypt = false;
    bool dry_run = false;
    bool verbose = false;
    VerifyMode verify = VerifyMode::None;
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

// Strict size parse: an integer with an optional b/k/m/g suffix, rejecting any
// trailing junk (std::stod would silently accept "12x3k").
uint64_t parse_size(const std::string& text) {
    std::string t = trim(text);
    uint64_t mult = 1;
    if (!t.empty() && !std::isdigit(static_cast<unsigned char>(t.back()))) {
        switch (std::tolower(static_cast<unsigned char>(t.back()))) {
            case 'b': mult = 1; break;
            case 'k': mult = 1024; break;
            case 'm': mult = 1024ull * 1024; break;
            case 'g': mult = 1024ull * 1024 * 1024; break;
            default: throw MigrateError("invalid size suffix in: " + text);
        }
        t.pop_back();
    }
    uint64_t v = 0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc() || p != t.data() + t.size())
        throw MigrateError("invalid size: " + text + " (expected e.g. 8192, 16k, 1m)");
    return v * mult;
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

// Cheap up-front estimate of the change size via a dry-run send (no data is
// streamed).  `zfs send -nP` prints a "size <bytes>" line; being derived from the
// plain (decompressed) stream it tracks the logical bytes we copy to within a
// couple of percent, regardless of on-disk compression.  Returns 0 if unknown.
uint64_t estimate_change(const std::vector<std::string>& send_cmd) {
    std::vector<std::string> cmd = {send_cmd[0], send_cmd[1], "-nP"};
    cmd.insert(cmd.end(), send_cmd.begin() + 2, send_cmd.end());
    try {
        for (const auto& line : split_lines(sp::check_output(cmd))) {
            std::vector<std::string> t = split_ws(line);
            if (t.size() >= 2 && t[0] == "size") return to_u64(t[1], "send -nP size");
        }
    } catch (const std::exception&) {
    }
    return 0;
}

// The send command that reproduces `snapshots[i]` (full for the first, else
// incremental from the previous snapshot).
std::vector<std::string> send_command(const std::vector<std::string>& snapshots,
                                      size_t i) {
    return i == 0 ? std::vector<std::string>{"zfs", "send", snapshots[i]}
                  : std::vector<std::string>{"zfs", "send", "-i", snapshots[i - 1],
                                             snapshots[i]};
}

void replay(const std::string& dest, uint64_t blocksize,
            const std::vector<std::string>& snapshots,
            const std::vector<std::string>& zstream_cmd, size_t start) {
    std::string dst_dev = "/dev/zvol/" + dest;

    if (g_opts.dry_run) {
        for (size_t i = start; i < snapshots.size(); ++i)
            std::cerr << "DRY-RUN: replay "
                      << (i == 0 ? "full" : "incremental from " + snapshots[i - 1])
                      << " -> " << snapshots[i] << std::endl;
        return;
    }

    // Estimate each replayed snapshot's change size up front (cheap dry-run sends)
    // so the progress bar can show one total across the whole run, and each step
    // still has its own figure for the drift check.
    std::vector<uint64_t> estimates(snapshots.size());
    uint64_t total = 0;
    for (size_t i = start; i < snapshots.size(); ++i) {
        estimates[i] = estimate_change(send_command(snapshots, i));
        total += estimates[i];
    }
    Progress progress(std::to_string(snapshots.size() - start) + " snapshots", total,
                      blocksize);
    uint64_t done = 0;  // bytes written so far across all replayed snapshots

    for (size_t i = start; i < snapshots.size(); ++i) {
        const std::string& snap = snapshots[i];
        std::string shortname = snap.substr(snap.find('@') + 1);
        uint64_t snap_volsize = to_u64(zfs_get(snap, "volsize"), "snapshot volsize");
        uint64_t dst_volsize = align_up(snap_volsize, blocksize);
        log("snapshot " + shortname + ": volsize=" + std::to_string(snap_volsize) +
            " (dest " + std::to_string(dst_volsize) + ")");
        run_mutate({"zfs", "set", "volsize=" + std::to_string(dst_volsize), dest});

        std::vector<std::string> send_cmd = send_command(snapshots, i);
        uint64_t estimate = estimates[i];
        log("+ " + sp::join(send_cmd) + " | " + sp::join(zstream_cmd) + "  (~" +
            std::to_string(estimate) + " bytes)");

        std::string src_dev = wait_for_device("/dev/zvol/" + snap);
        wait_for_device(dst_dev);
        int dst_fd = open(dst_dev.c_str(), O_RDWR);
        if (dst_fd < 0) throw MigrateError("cannot open destination " + dst_dev);

        uint64_t wrote = 0, freed = 0;
        {
            RangeApplier applier(src_dev, dst_fd, blocksize, dst_volsize);
            applier.set_progress([&](uint64_t w) { progress.update(done + w); });
            for_each_change(send_cmd, zstream_cmd, [&](const Change& c) {
                throw_if_interrupted();
                if (c.op == Op::Write)
                    applier.write(c.offset, static_cast<uint64_t>(c.length));
                else
                    applier.free(c.offset, c.length);
            });
            applier.flush();
            fsync(dst_fd);
            wrote = applier.bytes_written();
            freed = applier.bytes_freed();
        }
        close(dst_fd);
        done += wrote;
        log("  wrote " + std::to_string(wrote) + " bytes, freed " +
            std::to_string(freed) + " bytes (estimate " + std::to_string(estimate) +
            ")");

        // Sanity-check against the estimate: the dry-run size is not exact (stream
        // overhead), so this is only a warning, and only past 10% (the small
        // absolute floor keeps tiny/free-only increments from tripping it).
        if (estimate > 0) {
            uint64_t diff = wrote > estimate ? wrote - estimate : estimate - wrote;
            if (diff > 128 * 1024 && diff * 10 > estimate)
                std::cerr << "warning: size drift for " << shortname
                          << ": ZFS estimated ~" << human(estimate)
                          << " of changes but " << human(wrote)
                          << " were written (>10%)\n";
        }

        run_mutate({"zfs", "snapshot", dest + "@" + shortname});
    }
    progress.finish(done);
}

// ---- verification -------------------------------------------------------- //

uint64_t volsize_of(const std::string& dataset) {
    return to_u64(zfs_get(dataset, "volsize"), dataset + " volsize");
}

// Byte-compare the first `size` bytes of two devices.  Throws on a read error --
// which is a different fact from a content difference (the latter returns false).
bool devices_identical(const std::string& dev_a, const std::string& dev_b,
                       uint64_t size) {
    int fa = open(dev_a.c_str(), O_RDONLY);
    int fb = open(dev_b.c_str(), O_RDONLY);
    if (fa < 0 || fb < 0) {
        if (fa >= 0) close(fa);
        if (fb >= 0) close(fb);
        throw MigrateError("cannot open devices to verify: " + dev_a + ", " + dev_b);
    }
    std::vector<char> ba(CHUNK), bb(CHUNK);
    bool equal = true;
    try {
        for (uint64_t pos = 0; pos < size && equal;) {
            size_t n = std::min<uint64_t>(CHUNK, size - pos);
            if (!sp::pread_full(fa, ba.data(), n, pos) ||
                !sp::pread_full(fb, bb.data(), n, pos))
                throw MigrateError("read error while verifying at offset " +
                                   std::to_string(pos));
            if (std::memcmp(ba.data(), bb.data(), n) != 0) equal = false;
            pos += n;
        }
    } catch (...) {
        close(fa);
        close(fb);
        throw;
    }
    close(fa);
    close(fb);
    return equal;
}

// True if [offset, offset+size) of the device reads back as all zeros.
bool device_is_zero(const std::string& dev, uint64_t offset, uint64_t size) {
    int fd = open(dev.c_str(), O_RDONLY);
    if (fd < 0) throw MigrateError("cannot open device to verify: " + dev);
    std::vector<char> buf(CHUNK);
    static const std::vector<char> zeros(CHUNK, 0);
    bool zero = true;
    try {
        for (uint64_t pos = 0; pos < size && zero;) {
            size_t n = std::min<uint64_t>(CHUNK, size - pos);
            if (!sp::pread_full(fd, buf.data(), n, offset + pos))
                throw MigrateError("read error while verifying tail at offset " +
                                   std::to_string(offset + pos));
            if (std::memcmp(buf.data(), zeros.data(), n) != 0) zero = false;
            pos += n;
        }
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
    return zero;
}

// Byte-compare the migrated volume against the original.  With All, every snapshot
// is compared; the live head (the result) is always compared.  Runs before the
// swap so a failure leaves the original untouched.
void verify(const std::string& new_ds, const std::string& orig_ds,
            VerifyMode mode) {
    auto check = [&](const std::string& a, const std::string& b,
                     const std::string& what) {
        uint64_t sa = volsize_of(a), sb = volsize_of(b);
        uint64_t common = std::min(sa, sb);
        std::string da = wait_for_device("/dev/zvol/" + a);
        std::string db = wait_for_device("/dev/zvol/" + b);
        if (!devices_identical(da, db, common))
            throw MigrateError("verification FAILED: " + what +
                               " is not byte-identical to the original");
        // If the destination was rounded up to the new blocksize, the extra tail
        // must read as zeros (what the README promises for that region).
        if (sa != sb) {
            const std::string& bigger = sa > sb ? da : db;
            if (!device_is_zero(bigger, common, std::max(sa, sb) - common))
                throw MigrateError("verification FAILED: rounded-up tail of " +
                                   what + " is not all zeros");
        }
        log("verified " + what + " (" + human(common) + " identical)");
    };
    if (mode == VerifyMode::All)
        for (const auto& snap : list_snapshots(orig_ds)) {
            std::string sh = snap.substr(snap.find('@') + 1);
            check(new_ds + "@" + sh, orig_ds + "@" + sh, "snapshot " + sh);
        }
    check(new_ds, orig_ds, "head");
    std::cout << "Verification passed ("
              << (mode == VerifyMode::All ? "all snapshots + head" : "head")
              << " byte-identical)." << std::endl;
}

// A property temporarily forced to a value on a dataset, restored on destruction
// (best-effort).  Used to make the source's snapshot device nodes appear.
class ScopedProp {
   public:
    ScopedProp(std::string ds, std::string prop, const std::string& value)
        : ds_(std::move(ds)), prop_(std::move(prop)) {
        saved_ = zfs_get(ds_, prop_);
        if (saved_ != value)
            sp::check_call({"zfs", "set", prop_ + "=" + value, ds_});
        else
            saved_.clear();  // already the desired value; nothing to restore
    }
    ~ScopedProp() { reset(); }
    void reset() {
        if (!saved_.empty()) {
            sp::run_quiet({"zfs", "set", prop_ + "=" + saved_, ds_});
            saved_.clear();
        }
    }
    ScopedProp(const ScopedProp&) = delete;
    ScopedProp& operator=(const ScopedProp&) = delete;

   private:
    std::string ds_, prop_, saved_;
};

void swap_names(const std::string& dest, const std::string& backup,
                const std::string& orig_readonly, const std::string& orig_snapdev) {
    log("renaming " + g_opts.source + " -> " + backup + ", " + dest + " -> " +
        g_opts.source);
    run_mutate({"zfs", "rename", g_opts.source, backup});
    try {
        run_mutate({"zfs", "rename", dest, g_opts.source});
    } catch (...) {
        // Undo the first rename so the volume doesn't vanish from its name.
        if (!g_opts.dry_run &&
            sp::run_quiet({"zfs", "rename", backup, g_opts.source}) != 0)
            std::cerr << "error: rename of " << dest << " to " << g_opts.source
                      << " failed AND the rollback failed; the original is now at "
                      << backup << " and the new volume at " << dest
                      << " -- rename them by hand.\n";
        throw;
    }
    run_mutate({"zfs", "set", "readonly=" + orig_readonly, g_opts.source});
    run_mutate({"zfs", "set", "snapdev=" + orig_snapdev, g_opts.source});
}

// A reservation/refreservation value from `zfs get -Hp` that means "set".
bool prop_is_set(const std::string& v) {
    return !v.empty() && v != "0" && v != "none" && v != "-";
}

void migrate() {
    uint64_t blocksize = parse_size(g_opts.volblocksize);
    if (blocksize < 512u || (blocksize & (blocksize - 1)) != 0)
        throw MigrateError(
            "volblocksize must be a power of two >= 512 (e.g. 8k, 32k, 128k)");

    std::string pool = g_opts.source.substr(0, g_opts.source.find('/'));

    // Blocks above 128K need the large_blocks pool feature; check for a clear
    // message rather than letting `zfs create` fail cryptically after pre-flight.
    if (blocksize > 128u * 1024) {
        std::string feat = trim(sp::check_output(
            {"zpool", "get", "-H", "-o", "value", "feature@large_blocks", pool}));
        if (feat != "active" && feat != "enabled")
            throw MigrateError(
                "volblocksize " + g_opts.volblocksize +
                " needs pool feature large_blocks (currently: " + feat +
                "); enable it with `zpool set feature@large_blocks=enabled " + pool +
                "`");
    }

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

    // The newest snapshot must equal the live head, otherwise migrating would
    // silently drop everything written since it.  readonly=on prevents new writes
    // going forward but does not guarantee the head was in sync when it was set,
    // so check directly: written@<newest> is the space written since the newest
    // snapshot -- nonzero means the head has diverged.
    std::string newest = snapshots.back().substr(snapshots.back().find('@') + 1);
    uint64_t since_newest =
        to_u64(zfs_get(g_opts.source, "written@" + newest), "written@" + newest);
    if (since_newest > 0 && !g_opts.force)
        throw MigrateError(
            g_opts.source + ": " + human(since_newest) +
            " written since the newest snapshot @" + newest +
            " -- the live head is not captured in a snapshot. Snapshot it first, or "
            "pass --force to migrate only up to @" + newest +
            " (dropping later writes).");

    std::string dest = g_opts.dest.empty() ? g_opts.source + "-new" : g_opts.dest;
    std::string backup = g_opts.source + g_opts.backup_suffix;

    auto exists = [](const std::string& ds) {
        return sp::run_quiet({"zfs", "list", ds}) == 0;
    };
    auto short_of = [](const std::string& s) {
        return s.substr(s.find('@') + 1);
    };

    // --resume-from: continue an interrupted run.  `start` is the index of the
    // first snapshot to replay; everything before it must already be on the target.
    size_t start = 0;
    bool resuming = !g_opts.resume_from.empty();
    if (resuming) {
        size_t r = snapshots.size();
        for (size_t i = 0; i < snapshots.size(); ++i)
            if (snapshots[i] == g_opts.resume_from ||
                short_of(snapshots[i]) == g_opts.resume_from) {
                r = i;
                break;
            }
        if (r == snapshots.size())
            throw MigrateError("--resume-from " + g_opts.resume_from +
                               " is not a snapshot of " + g_opts.source);
        start = r;

        if (!exists(dest))
            throw MigrateError(
                "--resume-from needs the destination " + dest +
                " from the interrupted run, but it does not exist");
        // The only check: the earlier snapshots already exist on the target.
        for (size_t i = 0; i < r; ++i)
            if (!exists(dest + "@" + short_of(snapshots[i])))
                throw MigrateError("--resume-from " + g_opts.resume_from +
                                   ": expected " + dest + "@" + short_of(snapshots[i]) +
                                   " on the target but it is missing");

        // Show the size of the snapshot just before the resume point, so the base
        // the incremental builds on can be eyeballed on both sides.
        if (r >= 1) {
            std::string p = short_of(snapshots[r - 1]);
            std::cout << "resuming at @" << short_of(snapshots[r]) << " (from @" << p
                      << "): source referenced "
                      << human(to_u64(zfs_get(snapshots[r - 1], "referenced"), "ref"))
                      << ", target referenced "
                      << human(to_u64(zfs_get(dest + "@" + p, "referenced"), "ref"))
                      << std::endl;
        }
    } else if (exists(dest)) {  // pre-flight name check (skip when resuming)
        throw MigrateError(
            dest + " already exists (leftover from a previous run? resume with "
            "`--resume-from <snapshot>`, destroy it with `zfs destroy -r " + dest +
            "`, or choose another --dest)");
    }
    if (!g_opts.no_swap && exists(backup))
        throw MigrateError(backup +
                           " already exists; destroy it or choose another "
                           "--backup-suffix");

    // Encryption: the new zvol inherits encryption from its parent, so migrating an
    // encrypted source under a plaintext parent would silently produce PLAINTEXT.
    std::string enc = zfs_get(g_opts.source, "encryption");
    if (enc != "off") {
        std::string dest_parent = dest.substr(0, dest.rfind('/'));
        std::string parent_enc = zfs_get(dest_parent, "encryption");
        if (parent_enc == "off" && !g_opts.allow_decrypt)
            throw MigrateError(
                g_opts.source + " is encrypted (" + enc + ") but the destination "
                "parent " + dest_parent + " is not; the migrated copy would be "
                "PLAINTEXT. Pass --allow-decrypt to accept this, or use --dest under "
                "an encrypted parent.");
        if (parent_enc != "off")
            std::cerr << "warning: result will be encrypted under the parent's key ("
                      << dest_parent << "), not the source's own key\n";
    }

    std::vector<std::string> zstream_cmd = which_zstream();
    zstream_cmd.push_back("-v");

    // Stash the source's original readonly/snapdev; snapdev is forced visible below
    // (via the guard) so its snapshot device nodes exist for replay and verify.
    std::string orig_readonly = zfs_get(g_opts.source, "readonly");
    std::string orig_snapdev = zfs_get(g_opts.source, "snapdev");
    bool src_thick = prop_is_set(zfs_get(g_opts.source, "refreservation"));
    std::string src_reservation = zfs_get(g_opts.source, "reservation");

    log("migrating " + g_opts.source + " (" + std::to_string(snapshots.size()) +
        " snapshots) to volblocksize=" + std::to_string(blocksize) +
        ", dest=" + dest);

    std::optional<ScopedProp> snapdev_guard;
    if (!g_opts.dry_run)
        snapdev_guard.emplace(g_opts.source, "snapdev", "visible");

    bool dest_created = false;
    try {
        if (!resuming) {
            uint64_t first_volsize = align_up(
                to_u64(zfs_get(snapshots.front(), "volsize"), "volsize"), blocksize);
            create_dest(dest, blocksize, first_volsize);
            dest_created = true;
        }
        replay(dest, blocksize, snapshots, zstream_cmd, start);

        // Re-derive reservations the sparse create (-s) dropped, after the final
        // volsize and before verify/swap -- so the thick guarantee is never missing
        // from the volume under the original name, and if the pool lacks space the
        // failure happens while the original is still intact.
        if (src_thick) run_mutate({"zfs", "set", "refreservation=auto", dest});
        if (prop_is_set(src_reservation))
            run_mutate({"zfs", "set", "reservation=" + src_reservation, dest});

        if (g_opts.verify != VerifyMode::None && !g_opts.dry_run)
            verify(dest, g_opts.source, g_opts.verify);

        if (g_opts.no_swap) {
            std::cout << "Done. New zvol left at " << dest << " (no swap requested)."
                      << std::endl;
            return;
        }

        snapdev_guard.reset();  // restore source snapdev before it is renamed away
        swap_names(dest, backup, orig_readonly, orig_snapdev);
    } catch (...) {
        if (dest_created && !g_opts.dry_run)
            std::cerr << "note: an incomplete destination may be left at " << dest
                      << "; remove it with: zfs destroy -r " << dest << "\n"
                      << "the original " << g_opts.source << " was not modified.\n";
        throw;
    }

    std::cout << "Done. " << g_opts.source
              << " now has volblocksize=" << blocksize << ". Original preserved as "
              << backup << "." << std::endl;
    if (orig_readonly == "on")
        std::cout << "Note: " << g_opts.source
                  << " is still readonly=on; run `zfs set readonly=off "
                  << g_opts.source << "` when ready to use it.\n";
    if (!g_opts.keep_backup) {
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
    "  --resume-from SNAP    continue an interrupted run: replay from source\n"
    "                        snapshot SNAP onward onto an existing --dest (whose\n"
    "                        earlier snapshots must already be present)\n"
    "  --keep-backup         keep the original as backup (default)\n"
    "  --destroy-backup      destroy the original after a successful swap\n"
    "  --force               bypass precondition checks (readonly=on, and that the\n"
    "                        newest snapshot equals the live head)\n"
    "  --allow-decrypt       allow an encrypted source to be written as plaintext\n"
    "                        (when the destination parent is not encrypted)\n"
    "  --verify MODE         after copying, byte-compare against the original\n"
    "                        (MODE: head = the result only; all = every snapshot\n"
    "                        + head).  Runs before the swap; fails on any mismatch.\n"
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
        } else if (a == "--resume-from") {
            g_opts.resume_from = next();
        } else if (a == "--no-swap") {
            g_opts.no_swap = true;
        } else if (a == "--keep-backup") {
            g_opts.keep_backup = true;
        } else if (a == "--destroy-backup") {
            g_opts.keep_backup = false;
        } else if (a == "--force") {
            g_opts.force = true;
        } else if (a == "--allow-decrypt") {
            g_opts.allow_decrypt = true;
        } else if (a == "--verify") {
            std::string m = next();
            if (m == "all") g_opts.verify = VerifyMode::All;
            else if (m == "head") g_opts.verify = VerifyMode::Head;
            else if (m == "none") g_opts.verify = VerifyMode::None;
            else throw MigrateError("--verify expects: none | head | all");
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
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);
    try {
        if (!parse_args(argc, argv)) return 0;
        migrate();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
