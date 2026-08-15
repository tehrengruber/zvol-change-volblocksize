// GUID alignment: deterministic content-addressed GUIDs, aligning to a
// fingerprint file, and the standalone fingerprint/verify tasks.
//
// Model: references are produced *only* by --fingerprint-only (on a source zvol) and
// record, per snapshot, the full LtHash accumulator (the fingerprint) plus the source's
// own GUID.  --align-guids writes no file and forges GUIDs derived from the source GUIDs.
// --align-guids-to <ref> replays, checks each snapshot's replayed content against the
// reference's stored accumulator before forging the reference's GUID, and (on resume)
// seeds the accumulator from the reference rather than re-reading the base.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "lthash.hpp"
#include "subprocess.hpp"
#include "testing.hpp"

using namespace th;

static const uint64_t MiB = 1024 * 1024;

static std::string guid(const std::string& snap) { return get_prop(snap, "guid"); }

// Run the tool directly (the migrate() helper always injects a volblocksize; the
// fingerprint tasks take only <source>).
static int tool(const std::vector<std::string>& args) {
    std::vector<std::string> cmd = {testing::g_tool};
    cmd.insert(cmd.end(), args.begin(), args.end());
    return sp::call_status(cmd);
}

static void build(const std::string& src) {
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    write_pattern(src, 4 * MiB, 4 * MiB, 2);
    snapshot(src, "s1");
    write_pattern(src, 12 * MiB, 4 * MiB, 3);
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});
}

// Copy fingerprint file `in` to `out`, replacing the GUID (4th field) of the line
// for snapshot `snap` with a bogus value.
static void copy_bad_guid(const std::string& in, const std::string& out,
                          const std::string& snap) {
    std::ifstream i(in);
    std::ofstream o(out);
    std::string line;
    while (std::getline(i, line)) {
        if (!line.empty() && line[0] != '#') {
            std::istringstream is(line);
            std::string s, vs, fpr, g;
            is >> s >> vs >> fpr >> g;
            if (s == snap) line = s + " " + vs + " " + fpr + " 1";  // bogus GUID
        }
        o << line << "\n";
    }
}

// Copy fingerprint file `in` to `out`, corrupting the first snapshot's fingerprint (3rd
// field) so a content check against it must fail.
static void copy_bad_fingerprint(const std::string& in, const std::string& out) {
    std::ifstream i(in);
    std::ofstream o(out);
    std::string line;
    bool done = false;
    while (std::getline(i, line)) {
        if (!done && !line.empty() && line[0] != '#') {
            std::istringstream is(line);
            std::string s, vs, fpr, g;
            is >> s >> vs >> fpr >> g;
            line = s + " " + vs + " " + std::string(64, '0') + " " + g;  // wrong fingerprint
            done = true;
        }
        o << line << "\n";
    }
}

// Produce a reference fingerprint of `src`'s snapshots via --fingerprint-only.
static void fingerprint_only(const std::string& src, const std::string& out) {
    std::remove(out.c_str());  // start fresh (--fingerprint-only resumes from an existing file)
    EXPECT_EQ(tool({src, "--fingerprint-only", "--fingerprint-out", out}), 0);
}

// Parse a fingerprint file's cell size and its head (last) entry.
static void parse_fp_head(const std::string& path, uint64_t& cellsize,
                          std::string& head, uint64_t& headvs, std::string& fingerprint) {
    cellsize = 0;
    headvs = 0;
    head.clear();
    fingerprint.clear();
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        std::istringstream is(line);
        if (line.rfind("# cellsize", 0) == 0) {
            std::string hash, key;
            is >> hash >> key >> cellsize;
        } else if (!line.empty() && line[0] != '#') {
            std::string g;
            is >> head >> headvs >> fingerprint >> g;
        }
    }
}

// Independent one-shot LtHash of a snapshot's whole logical image, read from the
// device in `cellsize` cells (all-zero cells are no-ops, matching skipped holes),
// serialized -- the same form a fingerprint file records.
static std::string full_read_fingerprint(const std::string& snap, uint64_t volsize,
                                         uint64_t cellsize) {
    std::vector<uint8_t> img = read_range(snap, 0, volsize);
    LtHash h;
    std::vector<unsigned char> cellbuf(cellsize);
    for (uint64_t idx = 0; idx * cellsize < volsize; ++idx) {
        std::fill(cellbuf.begin(), cellbuf.end(), 0);
        uint64_t off = idx * cellsize;
        uint64_t n = std::min<uint64_t>(cellsize, volsize - off);
        std::copy(img.begin() + off, img.begin() + off + n, cellbuf.begin());
        h.add(idx, cellbuf.data(), cellsize);
    }
    return h.serialize();
}

// Write a reference fingerprint file computed *independently* from full reads of each
// source snapshot (not the tool's incremental path), at the source's own volblocksize.
// Aligning to it cross-checks the tool's replay hashing against a one-shot read: replay
// must reproduce these accumulators or --align-guids-to aborts.
static void write_full_read_ref(const std::string& src,
                                const std::vector<std::string>& snaps,
                                const std::string& out, uint64_t cellsize = 0) {
    if (cellsize == 0) cellsize = std::stoull(get_prop(src, "volblocksize"));
    std::ofstream o(out);
    o << "# zvol-change-volblocksize fingerprint v1\n"
      << "# dataset " << src << "\n"
      << "# cellsize " << cellsize << "\n"
      << "# columns: snapshot volsize fingerprint guid\n";
    for (const auto& s : snaps) {
        std::string snap = src + "@" + s;
        uint64_t vs = volsize(snap);
        o << s << " " << vs << " " << full_read_fingerprint(snap, vs, cellsize) << " "
          << guid(snap) << "\n";
    }
    o << "# dataset " << src << " complete (" << snaps.size() << " snapshots)\n";
}

// Two independent reblocks of the same source get identical (deterministic)
// GUIDs per snapshot, different from the source's own random GUIDs.
TEST(align_guids_deterministic) {
    auto src = make_zvol("8k", "32M");
    build(src);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids", "--verify",
                       "all"}),
              0);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-b", "--align-guids", "--verify",
                       "all"}),
              0);
    for (const std::string s : {"s0", "s1", "s2"}) {
        EXPECT_TRUE(guid(src + "-a@" + s) == guid(src + "-b@" + s));
        EXPECT_TRUE(guid(src + "-a@" + s) != guid(src + "@" + s));
        EXPECT_TRUE(devices_equal(src + "-a@" + s, src + "-b@" + s,
                                  volsize(src + "-a@" + s)));
    }
}

// --align-guids writes no fingerprint file, and combining it with --fingerprint-out is
// rejected (references are produced only by --fingerprint-only).
TEST(align_guids_no_fingerprint_file) {
    auto src = make_zvol("8k", "32M");
    build(src);
    // --fingerprint-out with an align mode is a usage error (no transfer happens).
    EXPECT_NE(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-x", "--align-guids",
                       "--fingerprint-out", "/tmp/zcvb-fp-nff", "--verify", "all"}),
              0);
    // Plain --align-guids works and writes nothing.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-b", "--align-guids", "--verify",
                       "all"}),
              0);
    for (const std::string s : {"s0", "s1", "s2"})
        EXPECT_TRUE(guid(src + "-b@" + s) != guid(src + "@" + s));
}

// --align-guids-to forges the GUIDs from a --fingerprint-only reference after the
// content matches; a tampered fingerprint makes it refuse.
TEST(align_guids_to_fingerprint) {
    auto src = make_zvol("8k", "32M");
    build(src);
    std::string ref = "/tmp/zcvb-fp-src";
    fingerprint_only(src, ref);
    // The tool's own reference matches an independent full read, so aligning to it is a
    // real content check rather than two incremental paths merely agreeing.
    uint64_t cellsize = 0, headvs = 0;
    std::string head, headfp;
    parse_fp_head(ref, cellsize, head, headvs, headfp);
    EXPECT_TRUE(head == "s2");
    EXPECT_EQ(full_read_fingerprint(src + "@" + head, headvs, cellsize), headfp);
    // Align a reblock to the reference: the dest snapshots carry the reference's (here
    // the source's own) GUIDs.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-b", "--align-guids-to", ref,
                       "--verify", "all"}),
              0);
    for (const std::string s : {"s0", "s1", "s2"})
        EXPECT_TRUE(guid(src + "-b@" + s) == guid(src + "@" + s));

    // Tamper with a recorded fingerprint; alignment must refuse (non-zero exit).
    std::string bad = "/tmp/zcvb-fp-bad";
    copy_bad_fingerprint(ref, bad);
    EXPECT_NE(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-c", "--align-guids-to", bad}),
              0);
}

// Alignment across a volsize change with a partial trailing fingerprint cell: replaying
// the grown, non-cell-aligned image must reproduce the reference (which --align-guids-to
// checks before forging), exercising grow + padding.
TEST(align_guids_volsize_change) {
    auto src = make_zvol("8k", "20M");
    write_pattern(src, 0, 20 * MiB, 1);
    snapshot(src, "s0");
    set_volsize(src, 30 * MiB);  // grow; 30M is not a multiple of the reblock size
    write_pattern(src, 20 * MiB, 8 * MiB, 2);
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});

    // Independent full-read reference; replaying against it checks the tool's inline
    // hashing across the grow + partial-cell padding.
    std::string ref = "/tmp/zcvb-fp-vs";
    write_full_read_ref(src, {"s0", "s1"}, ref);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", ref,
                       "--verify", "all"}),
              0);
}

// Standalone fingerprint + verify tasks (no conversion).
TEST(fingerprint_standalone) {
    auto src = make_zvol("8k", "32M");
    build(src);
    std::string fp = "/tmp/zcvb-fp-standalone";
    fingerprint_only(src, fp);
    EXPECT_EQ(tool({src, "--verify-fingerprint", fp}), 0);

    // A fingerprint of different content must not verify against this source.
    auto other = make_zvol("8k", "32M", "other");
    write_pattern(other, 0, 8 * MiB, 99);
    snapshot(other, "s0");
    std::string ofp = "/tmp/zcvb-fp-other";
    fingerprint_only(other, ofp);
    EXPECT_NE(tool({src, "--verify-fingerprint", ofp}), 0);
}

// ---- corner cases -------------------------------------------------------- //

// Two consecutive snapshots with identical content must still get DISTINCT forged
// GUIDs (derived from the source snapshot GUIDs, not the content) -- the collision
// a content-addressed GUID would have produced.
TEST(align_guids_no_collision) {
    auto src = make_zvol("8k", "16M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    snapshot(src, "s1");  // no writes between -> identical content to s0
    write_pattern(src, 8 * MiB, 4 * MiB, 2);
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids", "--verify",
                       "all"}),
              0);
    EXPECT_TRUE(guid(src + "-a@s0") != guid(src + "-a@s1"));  // identical content!
    EXPECT_TRUE(guid(src + "-a@s0") != guid(src + "-a@s2"));
    EXPECT_TRUE(guid(src + "-a@s0") != guid(src + "@s0"));    // != source's own GUID
}

// volsize SHRINK across snapshots under alignment: the replayed content (with the
// dropped tail cells removed) must reproduce the reference.
TEST(align_guids_volsize_shrink) {
    auto src = make_zvol("8k", "40M");
    write_pattern(src, 0, 40 * MiB, 1);
    snapshot(src, "s0");
    set_volsize(src, 22 * MiB);  // shrink; 22M is not a multiple of the reblock size
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});
    std::string ref = "/tmp/zcvb-fp-shrink";
    write_full_read_ref(src, {"s0", "s1"}, ref);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", ref,
                       "--verify", "all"}),
              0);
}

// FREE/holes under alignment: a punched hole produces hole cells; the replayed content
// must reproduce the reference.
TEST(align_guids_holes) {
    auto src = make_zvol("8k", "32M");
    write_pattern(src, 0, 32 * MiB, 1);
    snapshot(src, "s0");
    punch_hole(src, 8 * MiB, 8 * MiB);  // FREE a mid region
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});
    std::string ref = "/tmp/zcvb-fp-holes";
    write_full_read_ref(src, {"s0", "s1"}, ref);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", ref,
                       "--verify", "all"}),
              0);
}

// --resume-from under alignment (--align-guids): resuming produces the SAME forged GUIDs
// as an uninterrupted full reblock (GUIDs are source-derived, so no state is carried).
TEST(align_resume_works) {
    auto src = make_zvol("8k", "32M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    write_pattern(src, 8 * MiB, 8 * MiB, 2);
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});

    std::string dest = src + "-a";
    // "prior run": align s0,s1 into dest.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids", "--verify", "all"}),
              0);

    // Source gains s2; resume the alignment from it onto the existing dest.
    zfs({"set", "readonly=off", src});
    write_pattern(src, 16 * MiB, 8 * MiB, 3);
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids", "--resume-from",
                       "s2", "--verify", "all"}),
              0);
    EXPECT_TRUE(snapshot_names(dest) == (std::vector<std::string>{"s0", "s1", "s2"}));

    // Resuming yields the same forged GUIDs as an uninterrupted full reblock.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-b", "--align-guids", "--verify",
                       "all"}),
              0);
    for (const std::string s : {"s0", "s1", "s2"})
        EXPECT_TRUE(guid(dest + "@" + s) == guid(src + "-b@" + s));
}

// Realistic replica case: a dataset replicated to another pool with `send -R`
// (which preserves snapshot GUIDs), then pruned differently on each side.
// Independently reblocking both with --align-guids must give the surviving common
// snapshots matching forged GUIDs -- because they derive from the (shared) source
// snapshot GUIDs.
TEST(align_guids_across_replica) {
    auto srcA = make_zvol("8k", "24M");
    write_pattern(srcA, 0, 8 * MiB, 1);
    snapshot(srcA, "s0");
    write_pattern(srcA, 8 * MiB, 4 * MiB, 2);
    snapshot(srcA, "s1");
    write_pattern(srcA, 12 * MiB, 4 * MiB, 3);
    snapshot(srcA, "s2");
    write_pattern(srcA, 16 * MiB, 4 * MiB, 4);
    snapshot(srcA, "s3");

    // Replicate to a second pool; send -R preserves the snapshot GUIDs.
    std::string img = "/tmp/zcvb-replpool.img";
    sp::call_status({"zpool", "destroy", "zcvbrepl"});  // clear any leftover
    // Destroy the pool and its backing file on any exit (incl. a failed EXPECT,
    // which throws) so the test never leaks an imported pool / loop file.
    struct ReplPool {
        std::string img;
        ~ReplPool() {
            sp::call_status({"zpool", "destroy", "zcvbrepl"});
            sp::call_status({"sh", "-c", "rm -f " + img});
        }
    } repl_pool{img};
    sp::check_call({"truncate", "-s", "256M", img});
    sp::check_call({"zpool", "create", "-f", "zcvbrepl", img});
    sp::check_call({"sh", "-c", "zfs send -R " + srcA + "@s3 | zfs recv zcvbrepl/vol"});

    // Prune differently: drop s2 on A, s1 on the replica.  Common survivors: s0, s3.
    zfs({"destroy", srcA + "@s2"});
    zfs({"destroy", "zcvbrepl/vol@s1"});
    zfs({"set", "readonly=on", srcA});
    zfs({"set", "readonly=on", "zcvbrepl/vol"});

    EXPECT_EQ(migrate(srcA, "16k",
                      {"--no-swap", "--dest", srcA + "-a", "--align-guids", "--verify",
                       "all"}),
              0);
    EXPECT_EQ(migrate("zcvbrepl/vol", "16k",
                      {"--no-swap", "--dest", "zcvbrepl/vol-a", "--align-guids",
                       "--verify", "all"}),
              0);

    EXPECT_TRUE(guid(srcA + "-a@s0") == guid("zcvbrepl/vol-a@s0"));
    EXPECT_TRUE(guid(srcA + "-a@s3") == guid("zcvbrepl/vol-a@s3"));
    // Cleanup happens in ReplPool's destructor above.
}

// --align-guids-to + --resume-from: resuming forges the remaining snapshots from
// the reference and confirms the already-migrated GUIDs match it; a reference whose
// recorded GUID disagrees with an already-migrated snapshot aborts the resume.
TEST(align_guids_to_resume) {
    auto src = make_zvol("8k", "24M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    write_pattern(src, 8 * MiB, 4 * MiB, 2);
    snapshot(src, "s1");
    write_pattern(src, 12 * MiB, 4 * MiB, 3);
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});

    // Reference fingerprint of the source's own snapshots (s0..s2).  It records the
    // source's cell size; --align-guids-to hashes at that cell size (taken from the
    // file), so it stays comparable even though the reblock target here is 16k.
    std::string ref = "/tmp/zcvb-fp-tr-ref";
    fingerprint_only(src, ref);

    // Align the whole history to it, then drop the last snapshot to mimic a run that
    // was interrupted after s1.
    std::string dest = src + "-a";
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids-to", ref, "--verify",
                       "all"}),
              0);
    // Mimic a run interrupted after s1: drop s2 and reset the live volume to s1
    // (so it's exactly at its newest snapshot, as a clean interruption leaves it).
    zfs({"rollback", "-r", dest + "@s1"});

    // Resume: aligns s2 and checks s0,s1 already match the reference.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids-to", ref,
                       "--resume-from", "s2", "--verify", "all"}),
              0);
    EXPECT_TRUE(snapshot_names(dest) == (std::vector<std::string>{"s0", "s1", "s2"}));
    // The dest snapshots carry the reference (here: the source's own) GUIDs.
    for (const std::string s : {"s0", "s1", "s2"})
        EXPECT_TRUE(guid(dest + "@" + s) == guid(src + "@" + s));

    // Mismatch: a reference whose GUID for any already-migrated snapshot (here the
    // non-base s0) disagrees with the dest must make the resume abort.
    zfs({"rollback", "-r", dest + "@s1"});
    std::string bad = "/tmp/zcvb-fp-tr-bad";
    copy_bad_guid(ref, bad, "s0");
    EXPECT_NE(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids-to", bad,
                       "--resume-from", "s2"}),
              0);
}

// Resume across a volsize change under --align-guids-to: the accumulator seeded from the
// reference's base entry must be re-based correctly at the new (grown, partial-cell)
// volsize so the resumed snapshot still matches the reference.
TEST(align_resume_volsize_change) {
    auto src = make_zvol("8k", "20M");
    write_pattern(src, 0, 20 * MiB, 1);
    snapshot(src, "s0");
    zfs({"set", "readonly=off", src});
    set_volsize(src, 30 * MiB);  // grow to a non-cell-multiple
    write_pattern(src, 20 * MiB, 8 * MiB, 2);
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});

    std::string ref = "/tmp/zcvb-fp-rvc";
    write_full_read_ref(src, {"s0", "s1"}, ref);

    std::string dest = src + "-a";
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids-to", ref, "--verify",
                       "all"}),
              0);
    // Interrupt after s0, then resume: s1 grows the volume, seeded from ref's s0 entry.
    zfs({"rollback", "-r", dest + "@s0"});
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids-to", ref,
                       "--resume-from", "s1", "--verify", "all"}),
              0);
    EXPECT_TRUE(snapshot_names(dest) == (std::vector<std::string>{"s0", "s1"}));
}

// Cross-check the incremental fingerprint (built by --fingerprint-only off each change
// stream) against an *independent* one-shot LtHash of the whole final image read back
// from the device, then confirm --align-guids-to reproduces it during replay.  This is
// the check that would catch a change stream under-reporting a snapshot's touched cells.
TEST(align_fingerprint_matches_full_read) {
    auto src = make_zvol("8k", "40M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    write_pattern(src, 24 * MiB, 4 * MiB, 2);  // a big hole between the two regions
    snapshot(src, "s1");
    write_pattern(src, 4 * MiB, 4 * MiB, 3);  // overwrite inside an earlier region
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});

    // The reference is built from independent full reads; replaying against it must
    // reproduce those accumulators (else --align-guids-to aborts), so this directly
    // cross-checks the inline source hashing done during replay against a one-shot read.
    std::string ref = "/tmp/zcvb-fp-xcheck";
    write_full_read_ref(src, {"s0", "s1", "s2"}, ref);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", ref,
                       "--verify", "all"}),
              0);
}

// The same cross-check across a volsize *shrink*: the cells the previous snapshot had
// past the new (smaller) volsize must be removed from the accumulator, so the head's
// incremental fingerprint still equals an independent full read, and replay reproduces it.
TEST(align_fingerprint_shrink_matches_full_read) {
    auto src = make_zvol("8k", "40M");
    write_pattern(src, 0, 40 * MiB, 1);  // fully allocated
    snapshot(src, "s0");
    set_volsize(src, 20 * MiB);  // shrink: drop the whole second half
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});

    // Independent full-read reference over the shrink: replay must remove s0's cells past
    // the new 20M volsize to reproduce it, else --align-guids-to aborts.
    std::string ref = "/tmp/zcvb-fp-shrink-x";
    write_full_read_ref(src, {"s0", "s1"}, ref);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", ref,
                       "--verify", "all"}),
              0);
}

// A block that becomes zero (a punched hole, or dropped by a shrink) must have its
// cached seed reset to the hole value, so a LATER snapshot that rewrites that block
// subtracts nothing stale.  A stale seed would double-subtract and diverge from the
// independent full read.  Exercises free->rewrite and shrink->regrow->rewrite.
TEST(align_fingerprint_free_regrow_matches_full_read) {
    auto src = make_zvol("8k", "32M");
    write_pattern(src, 0, 32 * MiB, 1);         // s0: full 0-32M
    snapshot(src, "s0");
    punch_hole(src, 8 * MiB, 8 * MiB);          // s1: free 8-16M -> holes
    snapshot(src, "s1");
    write_pattern(src, 8 * MiB, 8 * MiB, 2);    // s2: rewrite the freed 8-16M
    snapshot(src, "s2");
    set_volsize(src, 16 * MiB);                 // s3: shrink to 16M (drop 16-32M)
    snapshot(src, "s3");
    set_volsize(src, 32 * MiB);                 // s4: grow back to 32M ...
    write_pattern(src, 16 * MiB, 16 * MiB, 3);  // ... and rewrite the regrown 16-32M
    snapshot(src, "s4");
    zfs({"set", "readonly=on", src});

    // fingerprint_snapshots path vs an independent full read of the head.
    std::string fp = "/tmp/zcvb-fp-freeregrow";
    fingerprint_only(src, fp);
    uint64_t cellsize = 0, headvs = 0;
    std::string head, fingerprint;
    parse_fp_head(fp, cellsize, head, headvs, fingerprint);
    EXPECT_TRUE(head == "s4");
    EXPECT_EQ(full_read_fingerprint(src + "@" + head, headvs, cellsize), fingerprint);

    // replay path vs an independent full-read reference (aborts on any divergence).
    std::string ref = "/tmp/zcvb-fp-freeregrow-ref";
    write_full_read_ref(src, {"s0", "s1", "s2", "s3", "s4"}, ref);
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", ref,
                       "--verify", "all"}),
              0);
}

// --verify-fingerprint at a cell size LARGER than the source's volblocksize: a FREE can
// then cover only part of a cell, whose surviving bytes must still be hashed.  The
// incremental fingerprint (fingerprint_snapshots) must read the cell's real content, not
// assume a freed cell is all-hole, so it still equals an independent full read at that
// larger cell size.
TEST(fingerprint_verify_larger_cellsize_partial_free) {
    auto src = make_zvol("8k", "32M");
    write_pattern(src, 0, 32 * MiB, 1);   // s0: fully allocated
    snapshot(src, "s0");
    punch_hole(src, 8 * 1024, 8 * 1024);  // s1: free 8k-16k -> upper half of the 16k cell 0
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});

    // Reference at 16k cells (> the source's 8k volblocksize), from an independent full
    // read; verifying the source against it recomputes at 16k and must match.
    std::string ref = "/tmp/zcvb-fp-partialfree";
    write_full_read_ref(src, {"s0", "s1"}, ref, /*cellsize=*/16384);
    EXPECT_EQ(tool({src, "--verify-fingerprint", ref}), 0);
}

// Resume when an intermediate source snapshot was deleted after the interrupted
// run: the resume replays a `send -i` that spans the gap.
TEST(align_resume_deleted_intermediate) {
    auto src = make_zvol("8k", "32M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    write_pattern(src, 8 * MiB, 4 * MiB, 2);
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});

    std::string dest = src + "-a";
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids", "--verify", "all"}),
              0);

    // Add s2, s3, then delete the intermediate s2 before resuming from s3.
    zfs({"set", "readonly=off", src});
    write_pattern(src, 12 * MiB, 4 * MiB, 3);
    snapshot(src, "s2");
    write_pattern(src, 16 * MiB, 4 * MiB, 4);
    snapshot(src, "s3");
    zfs({"destroy", src + "@s2"});  // gap: source is now s0, s1, s3
    zfs({"set", "readonly=on", src});

    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids", "--resume-from",
                       "s3", "--verify", "all"}),
              0);
    EXPECT_TRUE(snapshot_names(dest) == (std::vector<std::string>{"s0", "s1", "s3"}));
    // Same forged GUIDs as an uninterrupted reblock of the (gapped) source.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-b", "--align-guids", "--verify",
                       "all"}),
              0);
    for (const std::string s : {"s0", "s1", "s3"})
        EXPECT_TRUE(guid(dest + "@" + s) == guid(src + "-b@" + s));
}

// Malformed / empty fingerprint files are rejected cleanly (non-zero, no crash).
TEST(fingerprint_malformed) {
    auto src = make_zvol("8k", "16M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    zfs({"set", "readonly=on", src});

    std::string bad = "/tmp/zcvb-fp-malformed";
    { std::ofstream o(bad); o << "# header\ns0 123 deadbeef\n"; }  // 3 fields, not 4
    EXPECT_NE(tool({src, "--verify-fingerprint", bad}), 0);
    EXPECT_NE(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", bad}),
              0);

    std::string empty = "/tmp/zcvb-fp-empty";
    { std::ofstream o(empty); o << "# only a comment\n"; }
    EXPECT_NE(tool({src, "--verify-fingerprint", empty}), 0);
}

// --align-guids-to with a reference that doesn't cover every snapshot: by default it
// refuses up front (a partial reference is caught before any transfer), and
// --allow-missing-fingerprints instead migrates the uncovered snapshots with a
// source-derived GUID.  Use case: converting a backup that still holds older snapshots
// the reference (from a since-pruned primary) never recorded.
TEST(align_guids_to_allow_missing) {
    auto src = make_zvol("8k", "32M");
    build(src);  // s0, s1, s2

    // A full reference over all snapshots (records the source's own GUIDs).
    std::string full = "/tmp/zcvb-fp-am-full";
    fingerprint_only(src, full);

    // A --align-guids reblock (source-derived GUIDs), for comparing the uncovered s0.
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-der", "--align-guids", "--verify",
                       "all"}),
              0);

    // Drop s0's data line to simulate a primary that pruned its oldest snapshot.
    std::string partial = "/tmp/zcvb-fp-am-partial";
    {
        std::ifstream in(full);
        std::ofstream out(partial);
        for (std::string line; std::getline(in, line);) {
            std::istringstream is(line);
            std::string first;
            is >> first;
            if (!line.empty() && line[0] != '#' && first == "s0") continue;  // drop s0
            out << line << "\n";
        }
    }

    // Default: refuse, because s0 has no entry (fails before creating the dest).
    EXPECT_NE(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-strict", "--align-guids-to",
                       partial}),
              0);

    // With the flag: s0 gets a source-derived GUID (== the --align-guids reblock's), and
    // s1/s2 the reference GUIDs (== the source's own).
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", src + "-a", "--align-guids-to", partial,
                       "--allow-missing-fingerprints", "--verify", "all"}),
              0);
    EXPECT_TRUE(guid(src + "-a@s0") == guid(src + "-der@s0"));  // source-derived
    EXPECT_TRUE(guid(src + "-a@s1") == guid(src + "@s1"));      // reference GUID
    EXPECT_TRUE(guid(src + "-a@s2") == guid(src + "@s2"));
}

// --fingerprint-only (the fingerprint_snapshots path) cross-checked against an
// independent full read, exercising all three per-cell cases: a block appended into
// empty space (old read skipped), an overwrite of existing data (old read + subtracted),
// and a punched hole freeing existing data (old read + subtracted, cell zeroed).
TEST(fingerprint_only_matches_full_read) {
    auto src = make_zvol("8k", "40M");
    write_pattern(src, 0, 12 * MiB, 1);        // s0: data 0-12M
    snapshot(src, "s0");
    write_pattern(src, 24 * MiB, 4 * MiB, 2);  // s1: append at 24M (hole before it)
    snapshot(src, "s1");
    write_pattern(src, 4 * MiB, 4 * MiB, 3);   // s2: overwrite 4-8M (was s0 data)
    snapshot(src, "s2");
    punch_hole(src, 0, 4 * MiB);               // s3: free 0-4M (was s0 data) -> hole
    snapshot(src, "s3");
    zfs({"set", "readonly=on", src});

    std::string fp = "/tmp/zcvb-fp-only-fr";
    fingerprint_only(src, fp);
    uint64_t cellsize = 0, headvs = 0;
    std::string head, fingerprint;
    parse_fp_head(fp, cellsize, head, headvs, fingerprint);
    EXPECT_TRUE(head == "s3");
    EXPECT_EQ(full_read_fingerprint(src + "@" + head, headvs, cellsize), fingerprint);
}

// --align-guids-to resume where the resumed snapshot overwrites an already-migrated
// block: the accumulator seeded from the reference must let that old block be read and
// subtracted so the resumed snapshot still matches the reference (which an independent
// full read validates).
TEST(align_resume_overwrite_matches_full_read) {
    auto src = make_zvol("8k", "32M");
    write_pattern(src, 0, 16 * MiB, 1);        // s0: data 0-16M
    snapshot(src, "s0");
    write_pattern(src, 16 * MiB, 8 * MiB, 2);  // s1: append 16-24M
    snapshot(src, "s1");
    write_pattern(src, 4 * MiB, 4 * MiB, 3);   // s2: overwrite 4-8M (was s0 data)
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});

    // Independent full-read reference; resuming against it makes the seeded accumulator
    // read+subtract s0's overwritten block to reproduce s2's full-read fingerprint.
    std::string ref = "/tmp/zcvb-fp-ro";
    write_full_read_ref(src, {"s0", "s1", "s2"}, ref);

    std::string dest = src + "-a";
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids-to", ref, "--verify",
                       "all"}),
              0);
    // Interrupt after s1, then resume: s2 overwrites s0's region, so the seeded
    // accumulator must read+subtract that old block for the check to pass.
    zfs({"rollback", "-r", dest + "@s1"});
    EXPECT_EQ(migrate(src, "16k",
                      {"--no-swap", "--dest", dest, "--align-guids-to", ref,
                       "--resume-from", "s2", "--verify", "all"}),
              0);
    EXPECT_TRUE(snapshot_names(dest) == (std::vector<std::string>{"s0", "s1", "s2"}));
}

// --fingerprint-only resumes from an existing (partial) file: it keeps the snapshots
// already recorded and computes only the rest, and the result matches a from-scratch
// run.  Re-running once complete is a no-op.
TEST(fingerprint_only_resume) {
    auto data_lines = [](const std::string& p) {
        std::ifstream in(p);
        std::string line, out;
        while (std::getline(in, line))
            if (!line.empty() && line[0] != '#') out += line + "\n";
        return out;
    };

    // Fingerprint the first two snapshots -> file covers s0, s1 (each row is the
    // accumulator after that snapshot, which the resume reloads).
    auto src = make_zvol("8k", "32M");
    write_pattern(src, 0, 8 * MiB, 1);
    snapshot(src, "s0");
    write_pattern(src, 4 * MiB, 4 * MiB, 2);  // s1 overwrites 4-8M
    snapshot(src, "s1");
    zfs({"set", "readonly=on", src});
    std::string fp = "/tmp/zcvb-fp-res";
    std::remove(fp.c_str());  // must start absent, else the run below resumes to a no-op
    EXPECT_EQ(tool({src, "--fingerprint-only", "--fingerprint-out", fp}), 0);

    // A third snapshot appears (overwriting s0's 0-4M); re-running resumes from the
    // last recorded row -- it never re-reads s1 -- and lazily reads only s2's block.
    zfs({"set", "readonly=off", src});
    write_pattern(src, 0, 4 * MiB, 3);  // overwrite 0-4M (s0's, untouched by s1)
    snapshot(src, "s2");
    zfs({"set", "readonly=on", src});
    EXPECT_EQ(tool({src, "--fingerprint-only", "--fingerprint-out", fp}), 0);

    // It must equal a full from-scratch computation over s0, s1, s2.
    std::string full = "/tmp/zcvb-fp-res-full";
    std::remove(full.c_str());
    EXPECT_EQ(tool({src, "--fingerprint-only", "--fingerprint-out", full}), 0);
    EXPECT_TRUE(data_lines(fp) == data_lines(full));

    // Running again when already complete changes nothing.
    std::string before = data_lines(fp);
    EXPECT_EQ(tool({src, "--fingerprint-only", "--fingerprint-out", fp}), 0);
    EXPECT_TRUE(data_lines(fp) == before);
}
