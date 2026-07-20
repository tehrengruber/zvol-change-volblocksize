#!/usr/bin/env python3
"""Change a zvol's volblocksize while retaining all of its snapshots.

volblocksize is fixed at creation time, and a plain ``zfs send | zfs recv``
recreates the volume with the *source's* block size.  The only way to change it
is to write the data through a freshly created zvol so ZFS re-blocks it.

This tool recreates the full snapshot history on a new, correctly-blocked zvol
by replaying each snapshot in creation order.  For every step it asks ZFS which
byte ranges changed (``zfs send [-i] | zstream dump -v``) and copies only those
ranges from the source snapshot's block device to the destination device, then
takes a matching snapshot.  Finally it swaps the new zvol into the original name
(keeping the original as ``<name>-old``).

Preconditions on the source zvol:
  * ``readonly=on`` (so the newest snapshot equals the live head), and
  * at least one snapshot.

See README.md for the full rationale and limitations.
"""

from __future__ import annotations

import argparse
import fcntl
import os
import struct
import subprocess
import sys
import time

# A zvol objset stores the volume data in object 1 and its properties (including
# volsize) in a ZAP at object 2.  We reproduce object 1's data and handle volsize
# out of band, so writes/frees to object 2 are expected and ignored.
ZVOL_OBJ = 1
ZVOL_ZAP_OBJ = 2

# dnode geometry from OpenZFS include/sys/dnode.h: a 512-byte dnode slot
# (DNODE_SHIFT = 9) packed into a 16 KiB meta-dnode block (DNODE_BLOCK_SHIFT = 14)
# yields a fixed number of object-number slots per meta-dnode block.
DNODE_SHIFT = 9
DNODE_BLOCK_SHIFT = 14
DNODES_PER_BLOCK = 1 << (DNODE_BLOCK_SHIFT - DNODE_SHIFT)   # = 32
# A zvol objset occupies slots 0 (meta-dnode), 1 (data), 2 (props); a send frees
# the remaining slots of that first (and only) meta-dnode block, i.e. [3, 32).
FREEOBJECTS_FIRST = ZVOL_ZAP_OBJ + 1                        # = 3

# ioctl request codes (Linux, asm-generic).  BLKDISCARD frees a byte range on a
# block device; on a zvol the freed range becomes a hole that reads back as zero.
BLKDISCARD = 0x1277

# All record header keywords "zstream dump -v" can emit at column 0 (field
# continuation lines are indented).  Knowing the full set lets us detect a header
# for an *unknown* record type and fail loudly rather than silently mis-parse it.
KNOWN_RECORDS = {
    "BEGIN", "END", "OBJECT", "OBJECT_RANGE", "FREEOBJECTS", "FREE",
    "WRITE", "WRITE_BYREF", "WRITE_EMBEDDED", "SPILL", "REDACT",
}
# Records that carry no data change to the zvol data object and are safely
# skipped.  FREEOBJECTS is handled separately (validated, not blindly skipped).
# OBJECT_RANGE (raw sends only) and SPILL (SA overflow, never used by a zvol) are
# intentionally NOT here: if they appear the stream is not a plain zvol send, so
# they fall through to a hard failure.
IGNORED_RECORDS = {"BEGIN", "END", "OBJECT"}
# Column-0 lines from the trailing summary block (after END), not records.
SUMMARY_TOKENS = {"SUMMARY:", "SUMMARY", "Total", "Estimated"}

# Properties we set/manage ourselves or that are creation-time / key material and
# must NOT be copied from the source.  Everything else the source has *explicitly*
# set is carried over (see carried_properties) -- a denylist is safer than an
# allowlist because it cannot silently drop a property (including user properties)
# the source relied on.
UNSAFE_TO_COPY = {
    "volsize", "volblocksize",           # set explicitly (volblocksize is the point)
    "readonly", "snapdev",               # managed during migration / swap
    "reservation", "refreservation",     # destination is created sparse (-s)
    # encryption is creation-time and needs key material we do not reproduce:
    "encryption", "keyformat", "keylocation", "keystatus", "pbkdf2iters",
    # filesystem-only creation-time props (never local on a volume; listed for safety):
    "casesensitivity", "normalization", "utf8only",
}

CHUNK = 4 * 1024 * 1024
_ZEROS = bytes(CHUNK)


class MigrateError(Exception):
    pass


# --------------------------------------------------------------------------- #
# Small subprocess helpers
# --------------------------------------------------------------------------- #

def log(verbose: bool, *args) -> None:
    if verbose:
        print("[zvol-reblock]", *args, file=sys.stderr, flush=True)


def run(cmd: list[str], *, dry_run: bool = False, capture: bool = False) -> str:
    """Run a command, raising MigrateError on failure."""
    if dry_run:
        print("DRY-RUN:", " ".join(cmd), file=sys.stderr)
        return ""
    try:
        proc = subprocess.run(
            cmd, check=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE, text=True,
        )
    except FileNotFoundError as exc:
        raise MigrateError(f"command not found: {cmd[0]}") from exc
    except subprocess.CalledProcessError as exc:
        raise MigrateError(
            f"command failed ({exc.returncode}): {' '.join(cmd)}\n{exc.stderr}"
        ) from exc
    return proc.stdout or ""


def zfs_get(dataset: str, prop: str) -> str:
    """Return a single property value (parseable, i.e. bytes not human units)."""
    return run(["zfs", "get", "-Hp", "-o", "value", prop, dataset],
               capture=True).strip()


def carried_properties(source: str) -> dict[str, str]:
    """Every explicitly-set (local/received) property of the source that is safe
    to recreate on the new zvol.  Copying all user-set properties minus the few we
    manage (UNSAFE_TO_COPY) is safer than a hand-maintained allowlist: it cannot
    silently drop a property -- including user properties -- the source relied on.
    Properties left at their default/inherited value are omitted so the new zvol
    inherits them naturally from the same parent."""
    out = run(["zfs", "get", "-Hp", "-o", "property,value,source", "all", source],
              capture=True)
    props: dict[str, str] = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) != 3:
            continue
        prop, value, src = parts
        if src in ("local", "received") and prop not in UNSAFE_TO_COPY:
            props[prop] = value
    return props


def which_zstream() -> list[str]:
    """Locate the stream inspector: modern 'zstream dump' or old 'zstreamdump'."""
    for candidate in (["zstream", "dump"], ["zstreamdump"]):
        try:
            subprocess.run(candidate + ["-h"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            return candidate
        except FileNotFoundError:
            continue
    raise MigrateError("neither 'zstream' nor 'zstreamdump' found in PATH")


def wait_for_device(path: str, timeout: float = 15.0) -> str:
    """Wait for a zvol device node to appear (udev can lag behind zfs)."""
    subprocess.run(["udevadm", "settle"], stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return path
        time.sleep(0.1)
    raise MigrateError(f"device node did not appear: {path}")


# --------------------------------------------------------------------------- #
# Streaming parse of "zfs send | zstream dump -v"
# --------------------------------------------------------------------------- #

def _kv_pairs(tokens: list[str]) -> dict[str, str]:
    """Extract 'key = value' triples from a token list."""
    out: dict[str, str] = {}
    for i, tok in enumerate(tokens):
        if tok == "=" and 0 < i < len(tokens) - 1:
            out[tokens[i - 1]] = tokens[i + 1]
    return out


def _classify(rtype: str | None, fields: dict[str, str]):
    """Turn a completed record into ('WRITE'|'FREE', offset, length) for the zvol
    data object, None if it is safe to ignore, or raise MigrateError if it is
    something unexpected that we must not silently skip (it could mean data loss).
    """
    if rtype is None or rtype in IGNORED_RECORDS:
        return None
    if rtype == "REDACT":
        raise MigrateError("redacted send streams are not supported")

    if rtype == "FREEOBJECTS":
        # A zvol send frees exactly the unused tail of the first meta-dnode block,
        # objects [FREEOBJECTS_FIRST, DNODES_PER_BLOCK) = [3, 32), leaving our data
        # (1) and props (2) objects intact.  Assert that precisely rather than
        # trusting the stream: any other range means an object layout we do not
        # understand, so we must stop instead of silently dropping the record.
        firstobj = int(fields["firstobj"])
        numobjs = int(fields["numobjs"])
        if (firstobj, firstobj + numobjs) != (FREEOBJECTS_FIRST, DNODES_PER_BLOCK):
            raise MigrateError(
                f"unexpected FREEOBJECTS firstobj={firstobj} numobjs={numobjs}: "
                f"a plain zvol send must free exactly objects "
                f"[{FREEOBJECTS_FIRST}, {DNODES_PER_BLOCK})")
        return None

    if rtype == "FREE" or rtype.startswith("WRITE"):
        obj = int(fields["object"])
        if obj == ZVOL_ZAP_OBJ:
            return None  # volume properties; volsize is handled separately
        if obj != ZVOL_OBJ:
            raise MigrateError(
                f"unexpected {rtype} to object {obj} (only the zvol data object "
                f"{ZVOL_OBJ} is expected)")
        offset = int(fields["offset"])
        if rtype == "FREE":
            return ("FREE", offset, int(fields["length"]))
        # WRITE / WRITE_EMBEDDED / WRITE_BYREF: we only need the changed range;
        # the bytes are read from the snapshot device, so the encoding is moot.
        length = fields.get("logical_size") or fields.get("lsize") or fields.get("length")
        if length is None:
            raise MigrateError(f"{rtype} record without a length field: {fields}")
        return ("WRITE", offset, int(length))

    raise MigrateError(
        f"unsupported send record {rtype!r}: this does not look like a plain "
        f"zvol send (OBJECT_RANGE implies a raw send; SPILL implies SA overflow)")


def iter_change_records(send_cmd: list[str], zstream_cmd: list[str]):
    """Yield ('WRITE'|'FREE', offset, length) for the zvol data object.

    The send stream is piped straight into the dumper and consumed line by line;
    neither the stream nor the dump text is ever fully held in memory or spilled
    to disk, so arbitrarily large deltas are fine.  Any record we do not
    understand raises MigrateError instead of being ignored.
    """
    send = subprocess.Popen(send_cmd, stdout=subprocess.PIPE)
    assert send.stdout is not None
    dump = subprocess.Popen(zstream_cmd + ["-v"], stdin=send.stdout,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    send.stdout.close()  # let send get SIGPIPE if dump dies

    cur_type: str | None = None
    cur: dict[str, str] = {}
    ended = False

    try:
        assert dump.stdout is not None
        for line in dump.stdout:
            if not line.strip():
                continue
            if line[0].isspace():          # indented continuation line
                if not ended and cur_type is not None:
                    cur.update(_kv_pairs(line.split()))
                continue
            # Column-0 line: a record header (or the trailing summary block).
            tokens = line.split()
            rtype = tokens[0]
            if ended:
                continue                   # summary lines after END: ignore
            if rtype not in KNOWN_RECORDS:
                if rtype in SUMMARY_TOKENS:
                    ended = True
                    continue
                raise MigrateError(f"unexpected send output line: {line.strip()[:100]}")
            rec = _classify(cur_type, cur)
            if rec is not None:
                yield rec
            cur_type = rtype
            cur = _kv_pairs(tokens[1:])
            if rtype == "END":
                ended = True
        if not ended:
            rec = _classify(cur_type, cur)
            if rec is not None:
                yield rec
        # Normal completion: validate exit statuses.  (Skipped if the body
        # raised or the consumer stopped early; the finally still cleans up.)
        dump_err = dump.stderr.read() if dump.stderr else ""
        dump.wait()
        send.wait()
        if dump.returncode != 0:
            raise MigrateError(f"zstream dump failed: {dump_err}")
        if send.returncode not in (0, -13):  # -13 == SIGPIPE, fine
            raise MigrateError(f"zfs send failed (rc={send.returncode})")
    finally:
        for proc in (dump, send):
            if proc.poll() is None:
                proc.kill()
                proc.wait()


# --------------------------------------------------------------------------- #
# Applying changed ranges to the destination device
# --------------------------------------------------------------------------- #

class RangeApplier:
    """Copy WRITE ranges from a source snapshot device to the dest device and
    punch FREE ranges as holes.  Adjacent WRITE ranges are coalesced on the fly
    so at most one pending range plus one 4 MiB buffer is ever held."""

    def __init__(self, src_dev: str, dst_fd: int, dst_blocksize: int,
                 volsize: int, verbose: bool):
        self.src_fd = os.open(src_dev, os.O_RDONLY)
        self.dst_fd = dst_fd
        self.bs = dst_blocksize
        self.volsize = volsize
        self.verbose = verbose
        self._pend_start = -1
        self._pend_end = -1
        self.bytes_written = 0
        self.bytes_freed = 0

    def close(self):
        os.close(self.src_fd)

    # -- WRITE handling ---------------------------------------------------- #
    def write(self, offset: int, length: int) -> None:
        end = offset + length
        if self._pend_end == offset:            # contiguous, extend
            self._pend_end = end
        else:
            self.flush()
            self._pend_start, self._pend_end = offset, end

    def flush(self) -> None:
        if self._pend_start < 0:
            return
        pos, end = self._pend_start, self._pend_end
        self._pend_start = self._pend_end = -1
        while pos < end:
            n = min(CHUNK, end - pos)
            buf = os.pread(self.src_fd, n, pos)
            if not buf:
                break  # reading past end of source snapshot; rest is holes
            os.pwrite(self.dst_fd, buf, pos)
            self.bytes_written += len(buf)
            pos += len(buf)

    # -- FREE handling ----------------------------------------------------- #
    def free(self, offset: int, length: int) -> None:
        self.flush()
        # Clamp DMU_OBJECT_END / oversized frees to the volume size.
        if length < 0 or offset + length > self.volsize:
            length = max(0, self.volsize - offset)
        if length == 0:
            return
        self.bytes_freed += length
        a = (offset + self.bs - 1) // self.bs * self.bs   # align up
        b = (offset + length) // self.bs * self.bs        # align down
        if a < b:
            self._discard(a, b - a)
            self._zero(offset, a - offset)
            self._zero(b, offset + length - b)
        else:
            # Range smaller than one dest block: just zero it (content-correct).
            self._zero(offset, length)

    def _discard(self, offset: int, length: int) -> None:
        fcntl.ioctl(self.dst_fd, BLKDISCARD, struct.pack("QQ", offset, length))

    def _zero(self, offset: int, length: int) -> None:
        while length > 0:
            n = min(CHUNK, length)
            os.pwrite(self.dst_fd, _ZEROS[:n], offset)
            offset += n
            length -= n


# --------------------------------------------------------------------------- #
# Migration
# --------------------------------------------------------------------------- #

def list_snapshots(source: str) -> list[str]:
    out = run(["zfs", "list", "-H", "-d", "1", "-t", "snapshot",
               "-o", "name", "-s", "creation", source], capture=True)
    return [line for line in out.splitlines() if line]


def align_up(value: int, multiple: int) -> int:
    return (value + multiple - 1) // multiple * multiple


def create_dest(source: str, dest: str, blocksize: int, volsize: int,
                dry_run: bool, verbose: bool) -> None:
    cmd = ["zfs", "create", "-s", "-V", str(volsize), "-b", str(blocksize),
           "-o", "snapdev=visible"]
    for prop, val in carried_properties(source).items():
        cmd += ["-o", f"{prop}={val}"]
    cmd.append(dest)
    log(verbose, "creating destination:", dest)
    run(cmd, dry_run=dry_run)


def replay(source: str, dest: str, blocksize: int, snapshots: list[str],
           zstream_cmd: list[str], dry_run: bool, verbose: bool) -> None:
    dst_dev = f"/dev/zvol/{dest}"
    prev: str | None = None
    for snap in snapshots:
        short = snap.split("@", 1)[1]
        snap_volsize = int(zfs_get(snap, "volsize"))
        dst_volsize = align_up(snap_volsize, blocksize)

        log(verbose, f"snapshot {short}: volsize={snap_volsize} "
                     f"(dest {dst_volsize})")
        run(["zfs", "set", f"volsize={dst_volsize}", dest], dry_run=dry_run)

        if dry_run:
            kind = "full" if prev is None else f"incremental from {prev}"
            print(f"DRY-RUN: replay {kind} -> {snap}, snapshot {dest}@{short}",
                  file=sys.stderr)
            prev = snap
            continue

        src_dev = wait_for_device(f"/dev/zvol/{snap}")
        wait_for_device(dst_dev)
        dst_fd = os.open(dst_dev, os.O_RDWR)
        applier = RangeApplier(src_dev, dst_fd, blocksize, dst_volsize, verbose)
        try:
            if prev is None:
                send_cmd = ["zfs", "send", snap]
            else:
                send_cmd = ["zfs", "send", "-i", prev, snap]
            for kind, offset, length in iter_change_records(send_cmd,
                                                            zstream_cmd):
                if kind == "WRITE":
                    applier.write(offset, length)
                else:
                    applier.free(offset, length)
            applier.flush()
        finally:
            applier.close()
            os.fsync(dst_fd)
            os.close(dst_fd)

        log(verbose, f"  wrote {applier.bytes_written} bytes, "
                     f"freed {applier.bytes_freed} bytes")
        run(["zfs", "snapshot", f"{dest}@{short}"])
        prev = snap


def swap(source: str, dest: str, backup: str, dry_run: bool,
         verbose: bool) -> None:
    src_readonly = zfs_get(source, "readonly")
    src_snapdev = zfs_get(source, "snapdev")
    log(verbose, f"renaming {source} -> {backup}, {dest} -> {source}")
    run(["zfs", "rename", source, backup], dry_run=dry_run)
    run(["zfs", "rename", dest, source], dry_run=dry_run)
    # Match the source's original readonly / snapdev on the new volume.
    run(["zfs", "set", f"readonly={src_readonly}", source], dry_run=dry_run)
    run(["zfs", "set", f"snapdev={src_snapdev}", source], dry_run=dry_run)


def migrate(args) -> None:
    source = args.source
    blocksize = parse_size(args.volblocksize)
    if blocksize < 512 or (blocksize & (blocksize - 1)) != 0 or blocksize > 128 * 1024:
        raise MigrateError("volblocksize must be a power of two in [512, 128K]")

    if zfs_get(source, "type") != "volume":
        raise MigrateError(f"{source} is not a zvol")
    readonly = zfs_get(source, "readonly")
    if readonly != "on" and not args.force:
        raise MigrateError(
            f"{source} is not readonly=on; set it (so the newest snapshot is "
            f"the live head) or pass --force")

    snapshots = list_snapshots(source)
    if not snapshots:
        raise MigrateError(
            f"{source} has no snapshots; nothing to retain. Recreate the zvol "
            f"with a plain copy instead.")

    dest = args.dest or f"{source}-new"
    backup = f"{source}{args.backup_suffix}"
    zstream_cmd = which_zstream()

    log(args.verbose, f"migrating {source} ({len(snapshots)} snapshots) to "
                      f"volblocksize={blocksize}, dest={dest}")

    first_volsize = align_up(int(zfs_get(snapshots[0], "volsize")), blocksize)
    create_dest(source, dest, blocksize, first_volsize, args.dry_run,
                args.verbose)
    replay(source, dest, blocksize, snapshots, zstream_cmd, args.dry_run,
           args.verbose)

    if args.no_swap:
        print(f"Done. New zvol left at {dest} (no swap requested).")
        return

    swap(source, dest, backup, args.dry_run, args.verbose)
    print(f"Done. {source} now has volblocksize={blocksize}. "
          f"Original preserved as {backup}.")
    if not args.keep_backup and not args.dry_run:
        log(args.verbose, f"destroying backup {backup}")
        run(["zfs", "destroy", "-r", backup])
        print(f"Backup {backup} destroyed as requested.")


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def parse_size(text: str) -> int:
    """Parse a size like '16k', '128K', '4096' into bytes."""
    text = text.strip()
    units = {"": 1, "b": 1, "k": 1024, "m": 1024**2, "g": 1024**3}
    suffix = text[-1].lower()
    if suffix.isdigit():
        return int(text)
    if suffix not in units:
        raise MigrateError(f"invalid size: {text}")
    return int(float(text[:-1]) * units[suffix])


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zvol_reblock.py",
        description="Change a zvol's volblocksize while retaining all snapshots.")
    p.add_argument("source", help="source zvol (e.g. pool/vol)")
    p.add_argument("volblocksize", help="new volblocksize (e.g. 16k)")
    p.add_argument("--dest", help="intermediate name (default: <source>-new)")
    p.add_argument("--backup-suffix", default="-old",
                   help="suffix for the preserved original (default: -old)")
    p.add_argument("--no-swap", action="store_true",
                   help="leave the result under --dest; do not rename")
    p.add_argument("--keep-backup", dest="keep_backup", action="store_true",
                   default=True, help="keep the original as backup (default)")
    p.add_argument("--destroy-backup", dest="keep_backup", action="store_false",
                   help="destroy the original after a successful swap")
    p.add_argument("--force", action="store_true",
                   help="bypass the readonly=on precondition check")
    p.add_argument("--dry-run", action="store_true",
                   help="print planned actions without changing anything")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        migrate(args)
    except MigrateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
