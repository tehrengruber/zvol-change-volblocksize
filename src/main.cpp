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
#include <sys/file.h>
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
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "guidstream.hpp"
#include "lthash.hpp"
#include "sha256.hpp"
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
// scripts/logs stay clean) and redrawn at most once per second so the rate stays
// readable.  `total` is an estimate (0 = unknown -> no % / ETA).
class Progress {
   public:
    Progress(std::string label, uint64_t total)
        : label_(std::move(label)),
          total_(total),
          tty_(isatty(STDERR_FILENO)),
          start_(std::chrono::steady_clock::now()) {}
    ~Progress() {
        // If we're being torn down without finish() (e.g. an exception aborted the
        // work), wipe the half-drawn bar line so the error prints on a clean line.
        if (tty_ && drawn_ && !finished_) std::cerr << "\r\033[K" << std::flush;
    }

    void update(uint64_t done) {
        if (!tty_) return;
        auto now = std::chrono::steady_clock::now();
        // Repaint (and do all the work below) at most once per second, so a high tick
        // rate -- one call per write record -- costs only this clock read and compare,
        // never a window push, string format, or draw.  Always fall through once we've
        // reached the end so the final state is drawn.
        if (drawn_ && done < total_ && now - last_draw_time_ < std::chrono::seconds(1))
            return;
        drawn_ = true;
        last_draw_time_ = now;

        // Sample the sliding window only when we repaint, so it holds ~one entry per
        // second (a bounded ~10) and the shown speed reflects the last ~10 seconds.
        window_.emplace_back(now, done);
        while (window_.size() > 1 &&
               now - window_.front().first > std::chrono::seconds(10))
            window_.pop_front();

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
        finished_ = true;
        if (!tty_) return;
        double secs = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - start_).count();
        uint64_t rate = secs > 0 ? static_cast<uint64_t>(done / secs) : 0;
        std::cerr << '\r' << label_ << ": copied " << human(done) << " in "
                  << human_time(secs) << " (" << human(rate) << "/s)          \n"
                  << std::flush;
    }

    // Print a line that coexists with the bar: on a terminal, clear the current
    // bar line first and let the next update() redraw it underneath.
    void message(const std::string& msg) {
        if (tty_) std::cerr << "\r\033[K";
        std::cerr << msg << "\n" << std::flush;
        drawn_ = false;
    }

    // Change the leading label shown on the bar (e.g. the item being processed);
    // takes effect on the next update().
    void set_label(std::string label) { label_ = std::move(label); }

   private:
    std::string label_;
    uint64_t total_;
    bool tty_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_draw_time_{};
    bool drawn_ = false;
    bool finished_ = false;
    // (timestamp, cumulative bytes) samples within the trailing ~10s window.
    std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> window_;
};

// ---- device handles ------------------------------------------------------ //

// An owning file descriptor: closes on destruction, move-only.  `.get()` yields the raw
// fd for syscalls and for the non-owning Image view.
class Fd {
   public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    void reset() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

   private:
    int fd_ = -1;
};

// Open a device, throwing a clear error naming `what` if it cannot be opened.
Fd open_device(const std::string& path, int flags, const std::string& what) {
    int fd = open(path.c_str(), flags);
    if (fd < 0) throw MigrateError("cannot open " + what);
    return Fd(fd);
}

// ---- applying changed ranges to the destination device ------------------- //

class RangeApplier {
   public:
    RangeApplier(const std::string& src_dev, int dst_fd, uint64_t blocksize,
                 uint64_t volsize)
        : src_fd_(open_device(src_dev, O_RDONLY, "source device " + src_dev)),
          dst_fd_(dst_fd),
          bs_(blocksize),
          volsize_(volsize) {}

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
            ssize_t got = pread(src_fd_.get(), buf_.data(), want, pos);
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
            // Hand the just-read bytes to the data sink (the fingerprint) so it can hash
            // them without a second read of the source.
            if (on_written_) on_written_(pos, static_cast<uint64_t>(got), buf_.data());
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
        if (on_freed_) on_freed_(offset, ulen);  // tell the data sink this range is a hole now
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
    // Feed applied data to a sink (the fingerprint): `on_written(off, len, data)` for each
    // flushed write chunk, `on_freed(off, len)` for each freed range.
    void set_data_sink(std::function<void(uint64_t, uint64_t, const char*)> on_written,
                       std::function<void(uint64_t, uint64_t)> on_freed) {
        on_written_ = std::move(on_written);
        on_freed_ = std::move(on_freed);
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

    Fd src_fd_;
    int dst_fd_;
    uint64_t bs_;
    uint64_t volsize_;
    bool have_pending_ = false;
    uint64_t pend_start_ = 0, pend_end_ = 0;
    uint64_t bytes_written_ = 0, bytes_freed_ = 0;
    std::vector<char> buf_ = std::vector<char>(CHUNK);
    std::function<void(uint64_t)> on_progress_;
    std::function<void(uint64_t, uint64_t, const char*)> on_written_;
    std::function<void(uint64_t, uint64_t)> on_freed_;
};

// ---- migration ----------------------------------------------------------- //

// What to byte-compare between the migrated zvol and the original after copying.
enum class VerifyMode { None, Head, All, One };

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
    std::optional<std::string> verify_snap;  // set iff verify == One (short name)
    bool verify_only = false;  // just compare an existing dest; no transfer

    // GUID alignment (experimental).  At most one of align_guids / align_to is set.
    bool align_guids = false;                  // forge GUIDs derived from source GUIDs
    std::optional<std::string> align_to;       // forge the GUIDs from this file
    bool allow_missing_fingerprints = false;   // --align-guids-to: source-derive absentees
    std::optional<std::string> fingerprint_out;  // where --fingerprint-only writes
    bool fingerprint_only = false;             // standalone: just write a fingerprint
    std::optional<std::string> verify_fingerprint;  // standalone: check against a file

    bool aligning() const { return align_guids || align_to.has_value(); }
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

// An advisory, non-blocking, whole-run lock keyed by the destination, so two
// invocations can't clobber the same destination.  Released automatically when the
// process exits (even on SIGKILL -- the kernel drops the flock when the fd closes).
class FileLock {
   public:
    explicit FileLock(const std::string& dataset) {
        std::string key = dataset;
        for (char& c : key)
            if (c == '/' || c == '@') c = '_';
        path_ = "/run/lock/zvol-change-volblocksize-" + key + ".lock";
        fd_ = open(path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0)  // /run/lock should exist on Linux; fall back to /tmp if not
            fd_ = open((path_ = "/tmp/zvol-change-volblocksize-" + key + ".lock").c_str(),
                       O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0) throw MigrateError("cannot open lock file " + path_);
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            int e = errno;
            close(fd_);
            fd_ = -1;
            if (e == EWOULDBLOCK)
                throw MigrateError("another migration is already in progress for " +
                                   dataset + " (lock " + path_ + ")");
            throw MigrateError("cannot lock " + path_ + ": " + std::strerror(e));
        }
    }
    ~FileLock() {
        if (fd_ >= 0) close(fd_);  // drops the flock
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

   private:
    int fd_ = -1;
    std::string path_;
};

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

// Copy any user holds from the source snapshot onto the freshly created dest one,
// so the migrated snapshots keep their destroy protection.
void copy_holds(const std::string& src_snap, const std::string& dst_snap) {
    std::string out;
    try {
        out = sp::check_output({"zfs", "holds", "-H", src_snap});
    } catch (const std::exception&) {
        return;  // holds unsupported / query failed -- not fatal
    }
    for (const auto& line : split_lines(out)) {
        // `zfs holds -H` prints "<snapshot>\t<tag>\t<timestamp>".
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        std::string tag =
            line.substr(t1 + 1, (t2 == std::string::npos ? line.size() : t2) - t1 - 1);
        if (!tag.empty()) run_mutate({"zfs", "hold", tag, dst_snap});
    }
}

// ---- GUID alignment (experimental) --------------------------------------- //
//
// Two independently-reblocked copies of a dataset on different pools get fresh,
// non-matching snapshot GUIDs and so can't be `zfs send -i`-replicated to each
// other, even though their data is identical.  Alignment forges each migrated
// snapshot's GUID via a data-less `zfs recv` so the copies share snapshot
// identity.  See README "GUID alignment" for the full rationale and caveats.

// A snapshot's content fingerprint is an LtHash (see lthash.hpp) over cells of the
// zvol's own volblocksize: H(next) is H(prev) with each changed cell's old
// contribution removed and its new one added, so it updates in time proportional to
// the change, never re-reading holes.  The value recorded in a file is the full
// accumulator (serialized), so a resume can reload it and continue.  Cell = volblocksize
// means the fingerprint is comparable only between zvols at the same volblocksize --
// exactly the ones that can replicate to each other -- so the file records the cell size
// and mismatches are rejected on read.

// One snapshot's entry in a fingerprint file.
struct Fingerprint {
    std::string snap;     // short name
    uint64_t volsize;
    std::string state;    // the full LtHash accumulator after this snapshot, serialized as
                          // hex -- the content fingerprint itself, not its digest, so a
                          // resume can reload it and continue the incremental computation
    uint64_t guid;
};

// A parsed fingerprint file: its cell size and the per-snapshot entries.  Each entry
// carries the full accumulator after that snapshot, so resuming needs only the last
// entry -- there is no separate whole-file state.
struct FingerprintFile {
    uint64_t cellsize;
    std::map<std::string, Fingerprint> entries;
};

// A cellular image to read from: an open device and its size.  Always a real device;
// "no image" (all holes) is represented by an unset std::optional<Image>, not a
// sentinel fd.
struct Image {
    int fd;
    uint64_t volsize;
};

// An incremental fingerprinter for one snapshot.  It holds the running LtHash
// accumulator (`h`) and every cell's cached seed (LtHash::HOLE for a hole;
// LtHash::UNKNOWN when resuming from a persisted accumulator, before the cell's old
// content is read), together with the snapshot being hashed (`cur`) and its predecessor
// (`prev`; an unset image reads as all holes, e.g. the very first snapshot).  A cell's
// old content is read from `prev` only when its seed is UNKNOWN, so `prev` need only be
// set when a resume might have left UNKNOWN seeds.  Rather than re-point one long-lived
// object at each snapshot, callers derive the next snapshot's fingerprinter from the
// previous one (the accumulated state moves forward; the images are new).
struct Fingerprinter {
    LtHash h;
    std::vector<uint64_t> seeds;
    uint64_t cellsize;
    std::optional<Image> cur, prev;  // hashed snapshot + predecessor; unset = all holes
    std::vector<uint8_t> seen;       // per-snapshot dedup of already-hashed cells
    std::vector<char> ob, nb;        // scratch: old cell, and a read/straddle cell

    // An empty accumulator with no images yet -- used to seed resume state (load `h`,
    // mark base cells UNKNOWN) before the first snapshot's devices are opened.
    explicit Fingerprinter(uint64_t cellsize)
        : cellsize(cellsize), ob(cellsize), nb(cellsize) {}

    // The next snapshot: carry `prev_fp`'s accumulated state (h + cached seeds) forward
    // onto fresh images, sized for the `ncells` this snapshot spans.
    Fingerprinter(Fingerprinter&& prev_fp, Image cur, std::optional<Image> prev,
                  uint64_t ncells)
        : h(std::move(prev_fp.h)), seeds(std::move(prev_fp.seeds)),
          cellsize(prev_fp.cellsize), cur(cur), prev(prev), seen(ncells, 0),
          ob(cellsize), nb(cellsize) {
        if (seeds.size() < ncells) seeds.resize(ncells, LtHash::HOLE);
    }

    // Hash the cells overlapping [offset, offset+len), deduping repeats within the
    // snapshot.  A cell straddling an edge of the range keeps bytes outside it, so it is
    // always read whole from `cur`; the three entry points differ only in where a
    // fully-covered cell's new content comes from:
    //   update_data  -- it is `data` (the applier's just-written buffer -- no re-read)
    //   update_read  -- read it from `cur` (no buffer available)
    //   update_freed -- the range was freed, so the cell becomes a hole
    void update_data(uint64_t offset, uint64_t len, const char* data) {
        hash_span(offset, len, New::Data, data);
    }
    void update_read(uint64_t offset, uint64_t len) {
        hash_span(offset, len, New::Read, nullptr);
    }
    void update_freed(uint64_t offset, uint64_t len) {
        hash_span(offset, len, New::Freed, nullptr);
    }

   private:
    enum class New { Data, Read, Freed };
    void hash_span(uint64_t offset, uint64_t len, New src, const char* data);
    // Advance cell `idx` to `cell` (cellsize bytes, or nullptr for a hole): remove the old
    // cell via its cached seed (or, if UNKNOWN, by reading `prev`), then add the new.
    void update_cell(uint64_t idx, const char* cell);
};

uint64_t volsize_of(const std::string& dataset) {
    return to_u64(zfs_get(dataset, "volsize"), dataset + " volsize");
}

// Deterministic 64-bit GUID for a forged snapshot, derived from the *source*
// snapshot's own GUID.  Two pools holding the same dataset (replicas synced with
// zfs send/recv) share identical source-snapshot GUIDs, so both independently
// derive the same forged GUID -- and because source GUIDs are unique per snapshot,
// the derived GUIDs are unique too.  (Deriving from the source GUID, rather than
// from a content hash, avoids collisions between snapshots with identical content
// -- e.g. two consecutive no-change snapshots -- which a content hash would map to
// the same GUID.)  It is a hash of the source GUID, not the GUID itself, so it
// can't clash with the preserved original ("-old") snapshots that keep the source
// GUIDs.  For the case where the two sides' source GUIDs do *not* match but their
// content does, use the fingerprint-file path (--fingerprint-only / --align-guids-to).
uint64_t derive_guid(uint64_t source_guid) {
    unsigned char le[8];  // hash the little-endian bytes, so it's arch-independent
    for (int i = 0; i < 8; ++i) le[i] = static_cast<unsigned char>(source_guid >> (8 * i));
    sha256::Digest d = sha256::raw_of(le, sizeof le);
    uint64_t g = 0;
    for (int i = 0; i < 8; ++i) g = (g << 8) | d[i];
    return g ? g : 1;  // ZFS rejects a zero GUID
}

// Create dest@shortname carrying a chosen GUID: snapshot twice (identical), emit
// their empty incremental, rewrite its toguid, and receive it back over the base.
void forge_snapshot(const std::string& dest, const std::string& shortname,
                    uint64_t guid) {
    std::string target = dest + "@" + shortname;
    std::string t1 = dest + "@" + shortname + "_zcvbtmp1";
    std::string t2 = dest + "@" + shortname + "_zcvbtmp2";
    // Best-effort removal of both temp snapshots (each independently, so one missing
    // doesn't skip the other); ignores "no such snapshot".  A forge that failed or was
    // interrupted after creating a temp would otherwise leave it behind and block a
    // later --resume-from (which re-forges this snapshot) at the `zfs snapshot` below,
    // so we clear any stale temps up front and unwind the ones we create on any error.
    auto destroy_temps = [&] {
        sp::run_quiet({"zfs", "destroy", t1});
        sp::run_quiet({"zfs", "destroy", t2});
    };
    destroy_temps();
    try {
        run_mutate({"zfs", "snapshot", t1});
        run_mutate({"zfs", "snapshot", t2});  // no writes between -> empty delta
        std::string stream = sp::check_output({"zfs", "send", "-i", t1, t2});
        std::string forged = guidstream::rewrite_toguid(stream, guid);
        run_mutate({"zfs", "destroy", t2});
        // recv the empty delta over the base (t1) so @shortname inherits t1's content
        // but the forged GUID.  No -F: the volume is already at t1 (its latest
        // snapshot, nothing written since), so a clean incremental applies -- and if
        // it somehow diverged we want a hard error here, not a forced rollback that
        // discards data.
        sp::check_call_input({"zfs", "recv", target}, forged);
        run_mutate({"zfs", "destroy", t1});
    } catch (...) {
        destroy_temps();  // don't leave temps behind to block a resume
        throw;
    }
}

// Write the fingerprint of `fps` to `path` ("-" = stdout), tagged with `dataset`.
// While the standalone --fingerprint-only task is in progress this is called repeatedly
// with `complete=false`; the final call (`complete=true`) appends a trailing
// "# dataset ... complete" header so a reader can tell the file was fully written.
void write_fingerprint(const std::string& path, const std::string& dataset,
                       uint64_t cellsize, const std::vector<Fingerprint>& fps,
                       bool complete) {
    std::ostringstream body;
    body << "# zvol-change-volblocksize fingerprint v1\n"
         << "# dataset " << dataset << (complete ? "" : " (in progress)") << "\n"
         << "# cellsize " << cellsize << "\n";
    // Each row's fingerprint is the full LtHash accumulator after that snapshot (hex), so
    // a resume can reload the last row and continue -- no separate whole-file state line.
    body << "# columns: snapshot volsize fingerprint guid\n";
    for (const auto& f : fps)
        body << f.snap << ' ' << f.volsize << ' ' << f.state << ' ' << f.guid << '\n';
    if (complete)
        body << "# dataset " << dataset << " complete (" << fps.size() << " snapshots)\n";
    if (path == "-") {
        std::cout << body.str();
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw MigrateError("cannot write fingerprint file: " + path);
    out << body.str();
    if (!out) throw MigrateError("error writing fingerprint file: " + path);
}

FingerprintFile read_fingerprint(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw MigrateError("cannot open fingerprint file: " + path);
    FingerprintFile out{0, {}};
    bool saw_header = false;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '#') {
            std::vector<std::string> w = split_ws(t);
            // "# zvol-change-volblocksize fingerprint v1"
            if (w.size() >= 4 && w[2] == "fingerprint")
                saw_header = true;
            else if (w.size() >= 3 && w[1] == "cellsize")
                out.cellsize = to_u64(w[2], "fingerprint cellsize");
            continue;
        }
        std::vector<std::string> f = split_ws(t);
        if (f.size() != 4)
            throw MigrateError("malformed fingerprint line: " + t);
        Fingerprint fp{f[0], to_u64(f[1], "fingerprint volsize"), f[2],
                       to_u64(f[3], "fingerprint guid")};
        out.entries[fp.snap] = fp;
    }
    if (!saw_header)
        throw MigrateError("not a zvol-change-volblocksize fingerprint file: " + path);
    if (out.cellsize == 0)
        throw MigrateError("fingerprint file has no cell size (from an older, "
                           "incompatible build?): " + path);
    if (out.entries.empty())
        throw MigrateError("fingerprint file has no entries: " + path);
    return out;
}

// The send command that reproduces `snapshots[i]` (full for the first, else
// incremental from the previous snapshot).
std::vector<std::string> send_command(const std::vector<std::string>& snapshots,
                                      size_t i) {
    return i == 0 ? std::vector<std::string>{"zfs", "send", snapshots[i]}
                  : std::vector<std::string>{"zfs", "send", "-i", snapshots[i - 1],
                                             snapshots[i]};
}

// Read cell `idx` (cellsize bytes) of `img` into `buf`, zero-padding the tail past the
// volume.  An unset image is all-zero (a hole).  Returns the valid byte count.
uint64_t read_cell(const std::optional<Image>& img, uint64_t cellsize, uint64_t idx,
                   std::vector<char>& buf) {
    std::fill(buf.begin(), buf.end(), 0);
    if (!img) return 0;
    uint64_t off = idx * cellsize;
    uint64_t n = off < img->volsize ? std::min<uint64_t>(cellsize, img->volsize - off) : 0;
    if (n && !sp::pread_full(img->fd, buf.data(), n, off))
        throw MigrateError("read error hashing image at offset " + std::to_string(off));
    return n;
}

void Fingerprinter::update_cell(uint64_t idx, const char* cell) {
    uint64_t& seed = seeds[idx];
    if (seed == LtHash::UNKNOWN) {  // resume: read the old block and subtract by content
        read_cell(prev, cellsize, idx, ob);  // unset prev -> old cell was a hole
        h.remove(idx, ob.data(), cellsize);
    } else {
        h.sub_seed(seed);  // out with the old cell (no-op if a hole)
    }
    seed = cell ? h.add(idx, cell, cellsize) : LtHash::HOLE;  // nullptr -> hole (add is a no-op)
}

void Fingerprinter::hash_span(uint64_t offset, uint64_t len, New src, const char* data) {
    if (len == 0) return;
    for (uint64_t idx = offset / cellsize; idx <= (offset + len - 1) / cellsize; ++idx) {
        if (idx >= seen.size() || seen[idx]) continue;
        seen[idx] = 1;
        uint64_t cs = idx * cellsize;
        bool covered = cs >= offset && cs + cellsize <= offset + len;
        if (!covered) {  // straddle: the cell keeps bytes outside the range -> read it whole
            read_cell(cur, cellsize, idx, nb);
            update_cell(idx, nb.data());
        } else if (src == New::Data) {
            update_cell(idx, data + (cs - offset));  // reuse the applier's written bytes
        } else if (src == New::Read) {
            read_cell(cur, cellsize, idx, nb);
            update_cell(idx, nb.data());
        } else {  // New::Freed
            update_cell(idx, nullptr);  // a fully-freed cell is a hole
        }
    }
}

// Fingerprint `snaps[start..]` (full names, creation order) into `fp`, incrementally
// off each snapshot's `zfs send -i` change stream, so the work is O(change).  With
// `start > 0`, `fp` must already reflect `snaps[start-1]` -- and a seed of
// LtHash::UNKNOWN means "not loaded, read the old block from the previous snapshot" (the
// resume path, which avoids re-reading the base).  `out`, when given, collects each
// snapshot's Fingerprint; `on_fingerprint` is invoked after each so a caller can
// persist.  With `show_progress`, draws a bar measured by logical change bytes.
void fingerprint_snapshots(
    const std::vector<std::string>& snaps,
    const std::vector<std::string>& zstream_cmd, size_t start, Fingerprinter& fp,
    std::optional<std::reference_wrapper<std::vector<Fingerprint>>> out = std::nullopt,
    bool show_progress = false,
    const std::function<void(const std::vector<Fingerprint>&)>& on_fingerprint = {}) {
    uint64_t cellsize = fp.cellsize;
    std::optional<Progress> progress;
    uint64_t done = 0;
    if (show_progress && isatty(STDERR_FILENO)) {
        uint64_t total = 0;
        for (size_t i = start; i < snaps.size(); ++i)
            total += estimate_change(send_command(snaps, i));
        progress.emplace("fingerprinting " + std::to_string(snaps.size() - start) +
                             " snapshots", total);
    }
    auto tick = [&](uint64_t n) {
        if (progress) {
            done += n;
            progress->update(done);
        }
    };

    // The previous snapshot's device -- read only for UNKNOWN (resume) cells, and chained
    // as each snapshot's predecessor -- is opened here and owned by `prev_fd` (RAII).
    std::optional<Image> prev;
    Fd prev_fd;
    if (start > 0) {
        prev_fd = open_device(wait_for_device("/dev/zvol/" + snaps[start - 1]), O_RDONLY,
                              "snapshot device for " + snaps[start - 1]);
        prev = Image{prev_fd.get(), volsize_of(snaps[start - 1])};
    }
    for (size_t i = start; i < snaps.size(); ++i) {
        throw_if_interrupted();
        const std::string& snap = snaps[i];
        std::string sh = snap.substr(snap.find('@') + 1);
        if (progress)
            progress->set_label("fingerprinting @" + sh + " (" +
                                std::to_string(i - start + 1) + "/" +
                                std::to_string(snaps.size() - start) + ")");
        uint64_t vs = volsize_of(snap);
        uint64_t prev_vs = prev ? prev->volsize : 0;
        // The map of cells spans both volsizes so a shrink's dropped tail cells have a
        // slot.  This path has no applier buffer, so it reads each written cell from the
        // snapshot device (`cur`); a freed cell becomes a hole (straddle-safe).
        uint64_t ncells = (std::max(vs, prev_vs) + cellsize - 1) / cellsize;
        Fd cur_fd = open_device(wait_for_device("/dev/zvol/" + snap), O_RDONLY,
                                "snapshot device for " + snap);
        fp = Fingerprinter{std::move(fp), Image{cur_fd.get(), vs}, prev, ncells};
        for_each_change(send_command(snaps, i), zstream_cmd, [&](const Change& c) {
            throw_if_interrupted();
            uint64_t len = c.length < 0 ? (vs > c.offset ? vs - c.offset : 0)
                                        : static_cast<uint64_t>(c.length);
            if (len == 0 || c.offset >= vs) return;
            len = std::min<uint64_t>(len, vs - c.offset);
            if (c.op == Op::Write) {
                fp.update_read(c.offset, len);
                // Progress by logical bytes written, so it tracks the `zfs send -nP`
                // total and never counts holes.
                tick(len);
            } else {
                fp.update_freed(c.offset, len);
            }
        });
        // A shrink drops the previous snapshot's cells past the new volsize.
        if (prev_vs > vs) fp.update_freed(vs, prev_vs - vs);
        if (out)
            out->get().push_back(
                {sh, vs, fp.h.serialize(), to_u64(zfs_get(snap, "guid"), "guid")});
        if (out && on_fingerprint) on_fingerprint(out->get());
        // This snapshot's still-open device becomes the next one's predecessor.
        prev = Image{cur_fd.get(), vs};
        prev_fd = std::move(cur_fd);
    }
    if (progress) progress->finish(done);
}

// Replays snapshots onto `dest`.  When aligning, forges each snapshot's GUID: with
// `align_to` set it is the reference fingerprint, and each snapshot's GUID is forged only
// after its replayed content matches the reference's stored accumulator; otherwise
// (--align-guids) the GUID is derived from the source snapshot's GUID and no content
// fingerprint is computed.
void replay(
    const std::string& dest, uint64_t blocksize, uint64_t cellsize,
    const std::vector<std::string>& snapshots,
    const std::vector<std::string>& zstream_cmd, size_t start,
    const std::optional<std::map<std::string, Fingerprint>>& align_to) {
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
    Progress progress(std::to_string(snapshots.size() - start) + " snapshots", total);
    uint64_t done = 0;  // bytes written so far across all replayed snapshots
    // Verbose lines are routed through the bar so they don't collide with it.
    auto vlog = [&](const std::string& m) {
        if (g_opts.verbose) progress.message("[zvol-change-volblocksize] " + m);
    };

    bool aligning = g_opts.aligning();
    bool fingerprinting = align_to.has_value();  // only --align-guids-to computes a fingerprint
    Fingerprinter fp(cellsize);               // running image fingerprint, align-to only

    // Resuming an --align-guids-to run: seed the accumulator from the reference file's
    // stored fingerprint for the resume base (start-1), so incremental hashing continues
    // without re-reading the base.  Every already-migrated snapshot must still be in the
    // reference with a matching dest GUID (that is what makes reusing the reference's
    // accumulator sound); the base is required.  --align-guids has no fingerprint to seed.
    // `resumed_lazy` records that base cells were marked UNKNOWN, so update_cell may need
    // the previous source snapshot -- only then does the main loop open it.
    bool resumed_lazy = false;
    if (fingerprinting && start > 0) {
        auto short_of = [](const std::string& s) { return s.substr(s.find('@') + 1); };
        std::string base_sh = short_of(snapshots[start - 1]);
        for (size_t i = 0; i < start; ++i) {
            std::string sh = short_of(snapshots[i]);
            auto it = align_to->find(sh);
            if (it == align_to->end()) {
                if (sh == base_sh)
                    throw MigrateError(
                        "--resume-from with --align-guids-to: the resume base @" + sh +
                        " is not in the fingerprint");
                continue;
            }
            if (it->second.guid != to_u64(zfs_get(dest + "@" + sh, "guid"), "guid"))
                throw MigrateError("--resume-from with --align-guids-to: @" + sh +
                                   " does not match the fingerprint's GUID");
        }
        // The base matched above; seed `h` from its stored accumulator and mark its cells
        // UNKNOWN so the main loop lazily reads only the blocks each snapshot changes (from
        // the previous source snapshot) rather than re-reading the whole base.
        const Fingerprint& base = align_to->at(base_sh);
        if (!fp.h.deserialize(base.state))
            throw MigrateError("--align-guids-to: the fingerprint for the resume base @" +
                               base_sh + " is unreadable");
        fp.seeds.assign((base.volsize + cellsize - 1) / cellsize, LtHash::UNKNOWN);
        resumed_lazy = true;
    }

    for (size_t i = start; i < snapshots.size(); ++i) {
        const std::string& snap = snapshots[i];
        std::string shortname = snap.substr(snap.find('@') + 1);
        uint64_t snap_volsize = to_u64(zfs_get(snap, "volsize"), "snapshot volsize");
        uint64_t dst_volsize = align_up(snap_volsize, blocksize);
        vlog("snapshot " + shortname + ": volsize=" + std::to_string(snap_volsize) +
             " (dest " + std::to_string(dst_volsize) + ")");
        run_mutate({"zfs", "set", "volsize=" + std::to_string(dst_volsize), dest});

        std::vector<std::string> send_cmd = send_command(snapshots, i);
        uint64_t estimate = estimates[i];
        vlog("+ " + sp::join(send_cmd) + " | " + sp::join(zstream_cmd) + "  (~" +
             std::to_string(estimate) + " bytes)");

        std::string src_dev = wait_for_device("/dev/zvol/" + snap);
        wait_for_device(dst_dev);
        Fd dst_fd = open_device(dst_dev, O_RDWR, "destination " + dst_dev);

        // The fingerprint is computed inline as we replay: each applied change also hashes
        // the cells it touches by reading their new content from the *source* snapshot --
        // the same bytes we write to the dest -- so no second pass over the flushed
        // destination is needed.  The previous *source* snapshot's volsize bounds the cells
        // that could have existed (a shrink drops the tail) and supplies old content for
        // any UNKNOWN (lazy-resume) seeds.
        uint64_t prev_vs = 0;
        if (fingerprinting && i > 0)
            prev_vs = to_u64(zfs_get(snapshots[i - 1], "volsize"),
                             "previous snapshot volsize");

        Fd src_fp_fd, prev_fp_fd;  // owned source handles; read only for straddle cells
        uint64_t ncells = 0;
        if (fingerprinting) {
            src_fp_fd = open_device(src_dev, O_RDONLY, "source device " + src_dev);
            // Read the previous source snapshot only for the UNKNOWN seeds a lazy resume
            // left behind; a fresh run's seeds are all known, so it stays unset.
            std::optional<Image> prev;
            if (i > 0 && resumed_lazy) {
                prev_fp_fd = open_device(wait_for_device("/dev/zvol/" + snapshots[i - 1]),
                                         O_RDONLY,
                                         "previous snapshot device for " + snapshots[i - 1]);
                prev = Image{prev_fp_fd.get(), prev_vs};
            }
            // Derive this snapshot's fingerprinter from the previous one; `cur` is the
            // source, read only for cells straddling a write range's edge.
            ncells = (std::max(snap_volsize, prev_vs) + cellsize - 1) / cellsize;
            fp = Fingerprinter{std::move(fp), Image{src_fp_fd.get(), snap_volsize}, prev,
                               ncells};
        }

        std::optional<std::string> fingerprint;  // the replayed accumulator, if computed
        uint64_t wrote = 0, freed = 0;
        {
            RangeApplier applier(src_dev, dst_fd.get(), blocksize, dst_volsize);
            applier.set_progress([&](uint64_t w) { progress.update(done + w); });
            // Hash the bytes the applier just read as it flushes them -- no second read of
            // the source -- and turn a freed range into holes.
            if (fingerprinting)
                applier.set_data_sink(
                    [&](uint64_t o, uint64_t l, const char* d) { fp.update_data(o, l, d); },
                    [&](uint64_t o, uint64_t l) { fp.update_freed(o, l); });
            for_each_change(send_cmd, zstream_cmd, [&](const Change& c) {
                throw_if_interrupted();
                if (c.op == Op::Write)
                    applier.write(c.offset, static_cast<uint64_t>(c.length));
                else
                    applier.free(c.offset, c.length);
                // Keep the bar repainting on a steady ~1s cadence even between flushes
                // (the applier coalesces contiguous writes, so its per-chunk callback
                // fires only in bursts): otherwise the rate window samples unevenly and
                // the shown speed jumps.  Progress itself throttles this to once per sec.
                progress.update(done + applier.bytes_written());
            });
            applier.flush();
            fsync(dst_fd.get());
            wrote = applier.bytes_written();
            freed = applier.bytes_freed();
        }
        if (fingerprinting) {
            // A shrink drops the previous source snapshot's cells past the new volsize.
            if (prev_vs > snap_volsize)
                fp.update_freed(snap_volsize, prev_vs - snap_volsize);
            fingerprint = fp.h.serialize();
        }
        // Release the device handles before forging: forge_snapshot creates and destroys
        // temporary dest snapshots, which an open dest device can make "busy".
        dst_fd.reset();
        src_fp_fd.reset();
        prev_fp_fd.reset();
        done += wrote;
        vlog("  wrote " + std::to_string(wrote) + " bytes, freed " +
             std::to_string(freed) + " bytes (estimate " + std::to_string(estimate) +
             ")");

        // Sanity-check against the estimate: the dry-run size is not exact (stream
        // overhead), so this is only a warning, and only past 10% (the small
        // absolute floor keeps tiny/free-only increments from tripping it).  Routed
        // through the bar so it doesn't collide with the in-place progress line.
        if (estimate > 0) {
            uint64_t diff = wrote > estimate ? wrote - estimate : estimate - wrote;
            if (diff > 128 * 1024 && diff * 10 > estimate)
                progress.message("warning: size drift for " + shortname +
                                 ": ZFS estimated ~" + human(estimate) +
                                 " of changes but " + human(wrote) +
                                 " were written (>10%)");
        }

        std::string dst_snap = dest + "@" + shortname;
        if (aligning) {
            // Decide the GUID; for --align-guids-to, confirm the replayed content matches
            // the reference's stored fingerprint *before* forging.  `fingerprint` is the
            // LtHash accumulator computed inline above (present exactly when aligning to).
            uint64_t guid;
            if (align_to) {  // --align-guids-to: verify content, then use its GUID
                auto it = align_to->find(shortname);
                if (it != align_to->end()) {
                    // align_to implies fingerprinting, so `fingerprint` is present here.
                    // The reference records each snapshot's *source* volsize (from
                    // --fingerprint-only), which is what `snap_volsize` is.
                    if (it->second.volsize != snap_volsize ||
                        it->second.state != *fingerprint)
                        throw MigrateError(
                            "--align-guids-to: replayed content of @" + shortname +
                            " does not match the fingerprint; refusing to align");
                    guid = it->second.guid;
                } else if (g_opts.allow_missing_fingerprints) {
                    // Not in the reference (e.g. an older backup snapshot): there is
                    // nothing to align to, so derive from the source GUID, as
                    // --align-guids would.  (The pre-transfer check enforced the flag.)
                    guid = derive_guid(to_u64(zfs_get(snap, "guid"), "source guid"));
                } else {
                    throw MigrateError(
                        "--align-guids-to: fingerprint has no entry for @" + shortname);
                }
            } else {  // --align-guids: derive deterministically from the source GUID
                guid = derive_guid(to_u64(zfs_get(snap, "guid"), "source snapshot guid"));
            }
            forge_snapshot(dest, shortname, guid);
            copy_holds(snap, dst_snap);
            vlog("aligned @" + shortname + " guid=" + std::to_string(guid) +
                 " fingerprint=" +
                 (fingerprint ? fingerprint->substr(0, 12) + ".." : "(none)"));
        } else {
            run_mutate({"zfs", "snapshot", dst_snap});
            copy_holds(snap, dst_snap);
        }
    }
    progress.finish(done);
}

// ---- verification -------------------------------------------------------- //


// Byte-compare the first `size` bytes of two devices, reporting cumulative bytes
// read via `on_progress`.  Returns the offset of the first differing byte, or nullopt if
// the two ranges are identical.  Throws on a read error -- a different fact from a content
// difference (which just yields the offset).
std::optional<uint64_t> first_difference(
    const std::string& dev_a, const std::string& dev_b, uint64_t size,
    const std::function<void(uint64_t)>& on_progress = {}) {
    Fd fa = open_device(dev_a, O_RDONLY, "devices to verify: " + dev_a + ", " + dev_b);
    Fd fb = open_device(dev_b, O_RDONLY, "devices to verify: " + dev_a + ", " + dev_b);
    std::vector<char> ba(CHUNK), bb(CHUNK);
    for (uint64_t pos = 0; pos < size;) {
        throw_if_interrupted();
        size_t n = std::min<uint64_t>(CHUNK, size - pos);
        if (!sp::pread_full(fa.get(), ba.data(), n, pos) ||
            !sp::pread_full(fb.get(), bb.data(), n, pos))
            throw MigrateError("read error while verifying at offset " +
                               std::to_string(pos));
        if (std::memcmp(ba.data(), bb.data(), n) != 0)  // pin down the exact byte
            for (size_t k = 0; k < n; ++k)
                if (ba[k] != bb[k]) return pos + k;
        pos += n;
        if (on_progress) on_progress(pos);
    }
    return std::nullopt;
}

// True if [offset, offset+size) of the device reads back as all zeros.
bool device_is_zero(const std::string& dev, uint64_t offset, uint64_t size,
                    const std::function<void(uint64_t)>& on_progress = {}) {
    Fd fd = open_device(dev, O_RDONLY, "device to verify: " + dev);
    std::vector<char> buf(CHUNK);
    static const std::vector<char> zeros(CHUNK, 0);
    bool zero = true;
    for (uint64_t pos = 0; pos < size && zero;) {
        throw_if_interrupted();
        size_t n = std::min<uint64_t>(CHUNK, size - pos);
        if (!sp::pread_full(fd.get(), buf.data(), n, offset + pos))
            throw MigrateError("read error while verifying tail at offset " +
                               std::to_string(offset + pos));
        if (std::memcmp(buf.data(), zeros.data(), n) != 0) zero = false;
        pos += n;
        if (on_progress) on_progress(pos);
    }
    return zero;
}

// Byte-compare the migrated volume against the original.  All compares every
// snapshot plus the live head; Head compares only the head; One compares only the
// single named snapshot (verify_snap).  Shows one progress bar across all
// comparisons.  Runs before the swap so a failure leaves the original untouched.
void verify(const std::string& new_ds, const std::string& orig_ds,
            VerifyMode mode) {
    struct Item {
        std::string a, b, what;
        uint64_t sa, sb;
    };
    std::vector<Item> items;
    auto add = [&](const std::string& a, const std::string& b,
                   const std::string& what) {
        items.push_back({a, b, what, volsize_of(a), volsize_of(b)});
    };
    if (mode == VerifyMode::All)
        for (const auto& snap : list_snapshots(orig_ds)) {
            std::string sh = snap.substr(snap.find('@') + 1);
            add(new_ds + "@" + sh, orig_ds + "@" + sh, "snapshot " + sh);
        }
    if (mode == VerifyMode::One) {
        const std::string& sh = g_opts.verify_snap.value();
        if (sp::run_quiet({"zfs", "list", orig_ds + "@" + sh}) != 0)
            throw MigrateError("--verify " + sh + ": " + orig_ds + "@" + sh +
                               " is not a snapshot of the source");
        if (sp::run_quiet({"zfs", "list", new_ds + "@" + sh}) != 0)
            throw MigrateError("--verify " + sh + ": " + new_ds + "@" + sh +
                               " is missing on the target");
        add(new_ds + "@" + sh, orig_ds + "@" + sh, "snapshot " + sh);
    } else {
        add(new_ds, orig_ds, "head");
    }

    uint64_t total = 0;
    for (const auto& it : items) total += std::max(it.sa, it.sb);
    Progress progress("verifying " + std::to_string(items.size()) + " items", total);
    uint64_t done = 0;

    for (const auto& it : items) {
        uint64_t common = std::min(it.sa, it.sb);
        std::string da = wait_for_device("/dev/zvol/" + it.a);
        std::string db = wait_for_device("/dev/zvol/" + it.b);
        if (auto at = first_difference(
                da, db, common, [&](uint64_t n) { progress.update(done + n); }))
            throw MigrateError("verification FAILED: " + it.what +
                               " differs from the original at offset " +
                               std::to_string(*at) + " (" + human(*at) + ")");
        // If the destination was rounded up to the new blocksize, the extra tail
        // must read as zeros (what the README promises for that region).
        if (it.sa != it.sb) {
            const std::string& bigger = it.sa > it.sb ? da : db;
            if (!device_is_zero(bigger, common, std::max(it.sa, it.sb) - common,
                                [&](uint64_t n) { progress.update(done + common + n); }))
                throw MigrateError("verification FAILED: rounded-up tail of " +
                                   it.what + " is not all zeros");
        }
        done += std::max(it.sa, it.sb);
        if (g_opts.verbose)
            progress.message("[zvol-change-volblocksize] verified " + it.what + " (" +
                             human(common) + " identical)");
    }
    progress.finish(done);
    std::string what = mode == VerifyMode::All ? "all snapshots + head"
                       : mode == VerifyMode::One
                           ? "snapshot " + g_opts.verify_snap.value()
                           : "head";
    std::cout << "Verification passed (" << what << " byte-identical)."
              << std::endl;
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

// Compare an existing destination against the source, no transfer.  Useful to
// re-check a --no-swap result, or a post-swap pair (source = the -old backup,
// --dest = the migrated volume at the original name).
void verify_only() {
    std::string dest = g_opts.dest.empty() ? g_opts.source + "-new" : g_opts.dest;
    if (sp::run_quiet({"zfs", "list", dest}) != 0)
        throw MigrateError(
            dest + " does not exist; nothing to verify (pass --dest, or run a "
            "migration with --no-swap first)");
    if (zfs_get(g_opts.source, "type") != "volume")
        throw MigrateError(g_opts.source + " is not a zvol");
    VerifyMode mode =
        g_opts.verify != VerifyMode::None ? g_opts.verify : VerifyMode::All;
    // Make both sides' snapshot device nodes visible for the comparison.
    std::optional<ScopedProp> gsrc, gdst;
    gsrc.emplace(g_opts.source, "snapdev", "visible");
    gdst.emplace(dest, "snapdev", "visible");
    verify(dest, g_opts.source, mode);
}

// Fingerprint every snapshot of a zvol, in creation order, recording each
// snapshot's own current GUID.  Uses the zvol's own volblocksize as the cell size
// (returned alongside the entries) and hashes incrementally off the `zfs send -i`
// change streams, so the scan is O(total changes) not O(volsize x snapshots).
// Shared by both the --fingerprint-only and --verify-fingerprint tasks.
std::pair<uint64_t, std::vector<Fingerprint>> compute_fingerprints(
    const std::string& zvol, std::optional<uint64_t> cellsize_override = std::nullopt) {
    if (zfs_get(zvol, "type") != "volume")
        throw MigrateError(zvol + " is not a zvol");
    // Use the zvol's own volblocksize as the cell size, unless a caller pins it (when
    // verifying against a file, hash at that file's cell size so they are comparable).
    uint64_t cellsize = cellsize_override.value_or(
        to_u64(zfs_get(zvol, "volblocksize"), zvol + " volblocksize"));
    std::vector<std::string> snaps = list_snapshots(zvol);
    if (snaps.empty()) throw MigrateError(zvol + " has no snapshots");
    std::optional<ScopedProp> guard;
    if (!g_opts.dry_run) guard.emplace(zvol, "snapdev", "visible");
    std::vector<std::string> zstream_cmd = which_zstream();
    zstream_cmd.push_back("-v");

    std::vector<Fingerprint> fps;
    Fingerprinter fp(cellsize);
    fingerprint_snapshots(snaps, zstream_cmd, /*start=*/0, fp, std::ref(fps),
                          /*show_progress=*/true);
    return {cellsize, fps};
}

// Standalone: write a fingerprint of the source's snapshots (no conversion).  The file
// is rewritten after every snapshot so an interrupted run loses nothing, and if it
// already exists the snapshots it already covers are kept and computation resumes after
// them.
void fingerprint_task() {
    const std::string& zvol = g_opts.source;
    if (zfs_get(zvol, "type") != "volume") throw MigrateError(zvol + " is not a zvol");
    uint64_t cellsize = to_u64(zfs_get(zvol, "volblocksize"), zvol + " volblocksize");
    std::vector<std::string> snaps = list_snapshots(zvol);
    if (snaps.empty()) throw MigrateError(zvol + " has no snapshots");
    std::string path = g_opts.fingerprint_out.value_or("-");
    auto short_of = [](const std::string& s) { return s.substr(s.find('@') + 1); };

    // Resume from an existing file: keep the leading snapshots it already covers and
    // continue after them.  Instead of re-reading the last covered snapshot to rebuild
    // the running state, load the accumulator from that snapshot's own fingerprint row and
    // mark all its cells' seeds UNKNOWN -- so only cells actually changed after the resume
    // point read their old block (lazily), never the whole base.
    std::vector<Fingerprint> done;
    size_t start = 0;
    Fingerprinter fp(cellsize);
    if (path != "-" && std::ifstream(path).good()) {
        FingerprintFile ref = read_fingerprint(path);
        if (ref.cellsize != cellsize)
            throw MigrateError(
                "existing fingerprint file " + path + " was made at volblocksize " +
                std::to_string(ref.cellsize) + " but " + zvol + " is " +
                std::to_string(cellsize) + "; delete it to recompute");
        for (; start < snaps.size(); ++start) {
            auto it = ref.entries.find(short_of(snaps[start]));
            if (it == ref.entries.end()) break;
            done.push_back(it->second);
        }
        std::cerr << "info: " << path << " already exists; resuming from it ("
                  << done.size() << " of " << snaps.size()
                  << " snapshots already fingerprinted) -- delete the file first if you "
                     "want to recompute from scratch.\n";
        if (start == snaps.size()) {
            std::cerr << "Nothing to do; " << path << " already covers all snapshots.\n";
            return;
        }
        if (start > 0) {
            if (!fp.h.deserialize(done.back().state))
                throw MigrateError(path + " has an unreadable fingerprint for @" +
                                   done.back().snap + "; delete it to recompute");
            uint64_t base_vs = volsize_of(snaps[start - 1]);
            fp.seeds.assign((base_vs + cellsize - 1) / cellsize, LtHash::UNKNOWN);
        }
    }

    std::optional<ScopedProp> guard;
    if (!g_opts.dry_run) guard.emplace(zvol, "snapdev", "visible");
    std::vector<std::string> zstream_cmd = which_zstream();
    zstream_cmd.push_back("-v");

    std::vector<Fingerprint> computed;  // snaps[start..], appended to the kept prefix
    auto build_all = [&](const std::vector<Fingerprint>& partial) {
        std::vector<Fingerprint> all(done.begin(), done.end());
        all.insert(all.end(), partial.begin(), partial.end());
        return all;
    };
    auto persist = [&](const std::vector<Fingerprint>& partial) {
        if (path != "-" && !g_opts.dry_run)
            write_fingerprint(path, zvol, cellsize, build_all(partial), /*complete=*/false);
    };

    fingerprint_snapshots(snaps, zstream_cmd, start, fp, std::ref(computed),
                          /*show_progress=*/true, persist);

    std::vector<Fingerprint> all = build_all(computed);
    write_fingerprint(path, zvol, cellsize, all, /*complete=*/true);
    if (path != "-")
        std::cerr << "Wrote fingerprint of " << all.size() << " snapshots to " << path
                  << ".\n";
}

// Standalone: recompute the source's fingerprints (incrementally) and compare them
// to a file.  Exits non-zero if any snapshot differs or is missing.
void verify_fingerprint_task() {
    FingerprintFile ref = read_fingerprint(*g_opts.verify_fingerprint);
    // Recompute at the reference's cell size so the two are directly comparable.
    auto [cellsize, computed] = compute_fingerprints(g_opts.source, ref.cellsize);
    (void)cellsize;
    const std::map<std::string, Fingerprint>& want = ref.entries;
    std::map<std::string, Fingerprint> have;
    for (const auto& f : computed) have[f.snap] = f;

    size_t ok = 0, bad = 0, missing = 0;
    for (const auto& [sh, want_fp] : want) {
        auto it = have.find(sh);
        if (it == have.end()) {
            std::cout << "MISSING @" << sh << "\n";
            ++missing;
            continue;
        }
        const Fingerprint& got = it->second;
        bool match = (got.volsize == want_fp.volsize && got.state == want_fp.state);
        std::cout << (match ? "OK     " : "DIFFER ") << "@" << sh
                  << (got.guid == want_fp.guid ? "" : "  (guid differs)") << "\n";
        match ? ++ok : ++bad;
    }
    std::cout << ok << " matched, " << bad << " differ, " << missing << " missing\n";
    if (bad || missing) throw MigrateError("fingerprint verification failed");
}

void migrate() {
    if (g_opts.fingerprint_only) {
        fingerprint_task();
        return;
    }
    if (g_opts.verify_fingerprint) {
        verify_fingerprint_task();
        return;
    }
    if (g_opts.verify_only) {
        verify_only();
        return;
    }

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

    // Hold a whole-run advisory lock on the destination so a second invocation
    // can't clobber it concurrently.  Released on any exit.
    std::optional<FileLock> lock;
    if (!g_opts.dry_run) lock.emplace(dest);

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
        // Accept "pool/vol@snap", "@snap" or a bare short name (short_of
        // normalizes all three to the short name).
        std::string want = short_of(g_opts.resume_from);
        size_t r = snapshots.size();
        for (size_t i = 0; i < snapshots.size(); ++i)
            if (short_of(snapshots[i]) == want) {
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
        // The earlier snapshots must already exist on the target.
        for (size_t i = 0; i < r; ++i)
            if (!exists(dest + "@" + short_of(snapshots[i])))
                throw MigrateError("--resume-from " + g_opts.resume_from +
                                   ": expected " + dest + "@" + short_of(snapshots[i]) +
                                   " on the target but it is missing");

        // Nothing must have been written to the destination since its newest
        // migrated snapshot: `written@<newest>` must be 0.  Otherwise the live volume
        // has diverged from the base the resume replays onto (an external write, or
        // an interrupted mid-write), which would corrupt the resumed snapshot, so
        // refuse unless --force.
        std::string dest_newest = short_of(snapshots[r - 1]);
        uint64_t dest_written =
            to_u64(zfs_get(dest, "written@" + dest_newest), "written@" + dest_newest);
        if (dest_written > 0 && !g_opts.force)
            throw MigrateError(
                dest + ": " + human(dest_written) + " written since its newest "
                "migrated snapshot @" + dest_newest +
                " -- the destination has diverged from the resume base. Inspect it "
                "(or `zfs rollback " + dest + "@" + dest_newest +
                "` to discard the change), or pass --force to replay over it.");

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

    // GUID alignment (experimental): announce, recommend verification, and load
    // the target fingerprint for --align-guids-to (nullopt => source-derived GUIDs).
    // The fingerprint's cell size is the target volblocksize when we produce one, and
    // taken from the reference file when we align to one (the two migrations use the
    // same volblocksize, so the file already carries the right value).
    std::optional<std::map<std::string, Fingerprint>> align_to;
    uint64_t fp_cellsize = blocksize;
    if (g_opts.aligning()) {
        std::cerr
            << "warning: GUID alignment is EXPERIMENTAL and not thoroughly tested; "
               "it forges snapshot identity and, if the two sides ever diverge, can "
               "make replication misbehave -- use at your own risk.\n"
               "  how it works: after replaying each snapshot its GUID is forged via a "
               "data-less `zfs recv` so independently-reblocked copies on different "
               "pools share snapshot identity and can be incrementally replicated.\n";
        if (g_opts.align_to)
            std::cerr << "  each snapshot's replayed content is checked against the "
                         "fingerprint's checksum before its GUID is forged; a mismatch "
                         "aborts the run.\n";
        if (g_opts.verify != VerifyMode::All)
            std::cerr << "  strongly recommended: add `--verify all` -- a full "
                         "byte-compare of every migrated snapshot against the source, "
                         "run after replay and before the name swap (note: GUIDs are "
                         "already forged by then; a failed verify aborts before the "
                         "swap but leaves the un-swapped --dest for inspection).\n";
        if (g_opts.align_to) {
            FingerprintFile ref = read_fingerprint(*g_opts.align_to);
            fp_cellsize = ref.cellsize;  // hash at the reference's cell size to compare
            align_to = std::move(ref.entries);
        }
    }

    // Pre-transfer check: every snapshot we are about to migrate must have a
    // fingerprint entry, so a partial reference fails immediately rather than part-way
    // through the transfer.  --allow-missing-fingerprints instead lets the absent ones
    // be migrated with a source-derived GUID (as --align-guids would).
    if (align_to) {
        auto short_of = [](const std::string& s) { return s.substr(s.find('@') + 1); };
        std::vector<std::string> missing;
        for (size_t i = start; i < snapshots.size(); ++i) {
            std::string sh = short_of(snapshots[i]);
            if (align_to->find(sh) == align_to->end()) missing.push_back(sh);
        }
        if (!missing.empty()) {
            std::string list;  // cap the list so a long backup prefix stays readable
            for (size_t k = 0; k < missing.size() && k < 8; ++k)
                list += (k ? ", @" : "@") + missing[k];
            if (missing.size() > 8)
                list += ", ... (+" + std::to_string(missing.size() - 8) + " more)";
            if (!g_opts.allow_missing_fingerprints)
                throw MigrateError(
                    "--align-guids-to: the fingerprint has no entry for " +
                    std::to_string(missing.size()) + " snapshot(s) being migrated (" +
                    list +
                    "); pass --allow-missing-fingerprints to migrate those with a "
                    "source-derived GUID instead");
            std::cerr << "note: " << missing.size()
                      << " snapshot(s) are not in the fingerprint (" << list
                      << "); their GUIDs will be derived from the source snapshots "
                         "(--allow-missing-fingerprints).\n";
        }
    }

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
        replay(dest, blocksize, fp_cellsize, snapshots, zstream_cmd, start, align_to);

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
    "                        + head; @<snapshot> = only that one snapshot).  Runs\n"
    "                        before the swap; fails on any mismatch.\n"
    "  --verify-only         only byte-compare an existing --dest against the\n"
    "                        source (no transfer); uses --verify MODE, default all\n"
    "  --align-guids         (experimental) forge deterministic GUIDs (derived from\n"
    "                        the source snapshot GUIDs) on the migrated snapshots so\n"
    "                        independently-reblocked copies on different pools stay\n"
    "                        replication-compatible (see --verify all)\n"
    "  --align-guids-to FILE (experimental) forge the GUIDs recorded in a fingerprint\n"
    "                        FILE (made by --fingerprint-only), but only after each\n"
    "                        snapshot's replayed content matches that file's fingerprint;\n"
    "                        a resume seeds its state from FILE\n"
    "  --allow-missing-fingerprints  with --align-guids-to, migrate snapshots absent\n"
    "                        from the fingerprint using a source-derived GUID instead\n"
    "                        of failing -- e.g. converting a backup that still has older\n"
    "                        snapshots the reference doesn't cover\n"
    "  --fingerprint-only    just write a fingerprint of <source>'s snapshots and\n"
    "                        exit (no conversion); writes to --fingerprint-out\n"
    "  --fingerprint-out F   destination for --fingerprint-only ('-' for stdout);\n"
    "                        not valid with the align modes\n"
    "  --verify-fingerprint F  recompute <source>'s snapshot fingerprints and compare\n"
    "                        them to FILE (no conversion); non-zero exit on mismatch\n"
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
            else if (m.find('@') != std::string::npos) {
                // A single snapshot to compare on its own; must be @-qualified
                // ("vol@snap" or "@snap") so it can't be confused with a mode.
                std::string sh = m.substr(m.find('@') + 1);
                if (sh.empty())
                    throw MigrateError(
                        "--verify expects: none | head | all | @<snapshot>");
                g_opts.verify = VerifyMode::One;
                g_opts.verify_snap = sh;
            } else {
                throw MigrateError(
                    "--verify expects: none | head | all | @<snapshot>");
            }
        } else if (a == "--verify-only") {
            g_opts.verify_only = true;
        } else if (a == "--align-guids") {
            g_opts.align_guids = true;
        } else if (a == "--align-guids-to") {
            g_opts.align_to = next();
        } else if (a == "--allow-missing-fingerprints") {
            g_opts.allow_missing_fingerprints = true;
        } else if (a == "--fingerprint-out") {
            g_opts.fingerprint_out = next();
        } else if (a == "--fingerprint-only") {
            g_opts.fingerprint_only = true;
        } else if (a == "--verify-fingerprint") {
            g_opts.verify_fingerprint = next();
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
    if (g_opts.align_guids && g_opts.align_to)
        throw MigrateError("--align-guids and --align-guids-to are mutually exclusive");
    if (g_opts.allow_missing_fingerprints && !g_opts.align_to)
        throw MigrateError("--allow-missing-fingerprints only applies with --align-guids-to");
    // Fingerprint references are produced solely by --fingerprint-only.  --align-guids
    // needs no fingerprint at all, and --align-guids-to reads its reference (and seeds a
    // resume from it) via --align-guids-to <file> -- neither writes one.
    if (g_opts.fingerprint_out && !g_opts.fingerprint_only)
        throw MigrateError("--fingerprint-out is only valid with --fingerprint-only "
                           "(alignment reads its reference via --align-guids-to <file>)");
    // Standalone fingerprint tasks operate on <source-zvol> alone (no conversion).
    if (g_opts.fingerprint_only || g_opts.verify_fingerprint) {
        if (g_opts.fingerprint_only && g_opts.verify_fingerprint)
            throw MigrateError(
                "--fingerprint-only and --verify-fingerprint are mutually exclusive");
        // The standalone fingerprint tasks don't convert anything, so an align mode
        // alongside them is meaningless -- reject it rather than silently dropping it.
        if (g_opts.aligning())
            throw MigrateError("--fingerprint-only / --verify-fingerprint cannot be "
                               "combined with --align-guids or --align-guids-to");
        // These tasks read the snapshot devices for real (they make snapdev visible
        // and scan every snapshot); --dry-run would just skip the snapdev guard and
        // then stall waiting for device nodes that never appear, so reject the combo.
        if (g_opts.dry_run)
            throw MigrateError(
                "--dry-run cannot be combined with --fingerprint-only or "
                "--verify-fingerprint (these tasks only read; run them without it)");
        if (positional.size() != 1) {
            std::cerr << USAGE;
            throw MigrateError("expected a single <source-zvol> for the fingerprint task");
        }
        g_opts.source = positional[0];
        return true;
    }
    if (g_opts.verify_only) {
        if (positional.empty() || positional.size() > 2) {
            std::cerr << USAGE;
            throw MigrateError("with --verify-only, expected <source-zvol> "
                               "[volblocksize (ignored)]");
        }
        g_opts.source = positional[0];
        if (positional.size() == 2) g_opts.volblocksize = positional[1];
        return true;
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
