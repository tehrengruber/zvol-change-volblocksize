# zvol-change-volblocksize

Change a ZFS zvol's `volblocksize` **while retaining its entire snapshot
history**.

`volblocksize` is fixed when a zvol is created and cannot be changed in place. A
plain `zfs send | zfs recv` doesn't help either — the receive recreates the
volume with the *source's* block size, which is baked into the stream. The only
way to actually change it is to rewrite the data through a freshly created zvol
so ZFS re-blocks it at the new size.

Doing that for the current state is a one-line `dd`. The hard part — and what
this tool does — is recreating **every snapshot** on the new, correctly-blocked
zvol, efficiently.

## How it works

1. Enumerate the source's snapshots in creation order.
2. Create a new sparse zvol with the requested `volblocksize`, carrying over
   **every explicitly-set (local/received) property** of the source at its
   *current* value — except the few we manage (`volsize`, `volblocksize`,
   `readonly`, `snapdev`, and reservations, which are re-derived after the copy)
   or that are creation-time/key material (encryption). Property *changes across
   the snapshot history are not replayed*;
   the new zvol is created once with the final property values and all its
   snapshots share them (this matches ZFS, where properties are live dataset
   attributes, not per-snapshot content). User properties are preserved.
3. Replay each snapshot in order. For each step it asks ZFS which byte ranges
   changed with `zfs send [-i] <prev> <snap> | zstream dump -v`, and copies only
   those ranges:
   - `WRITE` records → read the bytes from the source snapshot's block device
     (`/dev/zvol/pool/vol@snap`) and write them at the same offset on the new
     device (which re-blocks them at the new `volblocksize`);
   - `FREE` records → punch the same hole on the new device (`BLKDISCARD`).
   Then it takes a matching snapshot and moves on. `volsize` is matched to each
   snapshot (grow **or** shrink), so non-monotonic size histories replicate too.
4. Swap names: the original is renamed to `pool/vol-old` (kept as a backup) and
   the new zvol takes the original name.

The send stream and its `zstream dump` output are consumed **streaming**, line
by line — nothing is spilled to disk or held whole in memory, so arbitrarily
large deltas are fine.

## Building

A single C++20 binary with no third-party dependencies; built with CMake:

```console
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# -> build/zvol-change-volblocksize
sudo cmake --install build   # optional: installs to <prefix>/bin
```

**Prebuilt binaries** are attached to GitHub releases:

- a **`.deb`** for Debian trixie (also buildable locally with `cpack -G DEB` from
  the build directory), and
- a **fully static binary** (`zvol-change-volblocksize-static-linux-x86_64`) that
  needs no installation — download it, `chmod +x`, and run. It still needs the
  `zfs`/`zpool`/`zstream` userland present at runtime, but has no library
  dependencies of its own.

To build the static binary yourself, add `-DZCVB_STATIC=ON` (best with a musl
toolchain, e.g. inside an Alpine container, for maximum portability):

```console
cmake -S . -B build-static -DZCVB_STATIC=ON && cmake --build build-static
```

## Requirements & preconditions

- **Linux only** (uses `/dev/zvol` device nodes, the `BLKDISCARD` ioctl and
  `<linux/fs.h>`). It does not run on FreeBSD.
- OpenZFS with `zfs`, `zpool`, and `zstream` (or the older `zstreamdump`).
- **Run as root.**
- The source zvol must be **`readonly=on`** (removes any race with a live writer
  during the migration). Set it before running, or pass `--force` to skip.
- The **newest snapshot must equal the live head** — i.e. nothing has been written
  since the newest snapshot (`written@<newest>` is 0). Otherwise the tool refuses,
  because it would silently drop those post-snapshot writes; snapshot the head
  first, or pass `--force` to migrate only up to the newest snapshot.
- The source must have at least one snapshot.

## Usage

```console
# make the source quiescent
zfs snapshot pool/vol@now          # if the current head isn't already a snapshot
zfs set readonly=on pool/vol

# migrate 8k -> 16k, keeping pool/vol-old as a backup
sudo zvol-change-volblocksize pool/vol 16k

# the tool preserves the source's readonly state, so re-enable writes when ready
zfs set readonly=off pool/vol

# inspect the result, then drop the backup when satisfied
zfs destroy -r pool/vol-old
```

Options:

| flag | meaning |
| --- | --- |
| `--dest NAME` | intermediate name for the new zvol (default `<source>-new`) |
| `--backup-suffix S` | suffix for the preserved original (default `-old`) |
| `--no-swap` | build the new zvol but don't rename anything |
| `--resume-from SNAP` | continue an interrupted run: replay from source snapshot `SNAP` onto an existing `--dest` that already holds the earlier snapshots |
| `--verify MODE` | byte-compare the result against the original before swapping — `head` (the live head only) or `all` (every snapshot + head); aborts on any mismatch |
| `--verify-only` | only compare an existing `--dest` against the source (no transfer); uses `--verify MODE` (default `all`). Handy to re-check a `--no-swap` result |
| `--destroy-backup` | destroy the original after a successful swap (default: keep) |
| `--force` | skip the precondition checks (`readonly=on`, newest-snapshot-is-head) |
| `--allow-decrypt` | allow an encrypted source to be written as plaintext (destination parent not encrypted) |
| `--dry-run` | print the planned actions without changing anything |
| `-v` | verbose progress on stderr |

## Limitations

- **Encryption.** Data is read as plaintext from the snapshot device (keys must
  be loaded); raw/`-w` sends are not used. The new zvol inherits encryption from
  its **destination parent**, so the outcome is:

  | source | destination parent | result |
  | --- | --- | --- |
  | encrypted | encrypted | encrypted **under the parent's key** (warning printed) |
  | encrypted | not encrypted | refused unless `--allow-decrypt` → **plaintext** |
  | not encrypted | either | not encrypted |

- **Reservations.** The destination is created sparse; a thick source
  (`refreservation`) has its reservation **re-derived** (`refreservation=auto`)
  after the copy, and a plain `reservation` is copied by value — both applied
  before verify/swap, so the guarantee is never missing under the original name.
- Assumes the newest snapshot equals the head (checked via `written@<newest>`).
- `volsize` must be a multiple of the new `volblocksize`; if the source size
  isn't, the new volume is rounded up to the next block (the extra tail reads as
  zeros, which `--verify` confirms).
- Blocks larger than 128K require the pool's `large_blocks` feature.

## Testing

The tests are C++ (registered with CTest) and drive real ZFS pools on loop files.
ZFS needs a kernel module, which is awkward to install on the host (especially
Arch), so the suite runs inside a disposable Ubuntu QEMU/KVM guest.

**In a disposable guest (recommended).** Both entry points download an Ubuntu
cloud image (cached), boot it, install ZFS and a C++ toolchain, mount this repo
over 9p read-only, build with CMake, run CTest, and exit with its status:

```console
# Podman-wrapped (needs /dev/kvm on the host):
./test-harness/run.sh

# or drive QEMU directly (needs qemu + qemu-img + xorriso + curl on the host):
./test-harness/run-qemu.sh
```

Useful environment overrides for the QEMU path: `CACHE_DIR` (where the cloud
image is cached), `TIMEOUT`, `MEM`, `SMP`.

**Directly, if you already have a working ZFS (needs root).** Each test creates a
throwaway pool on a loop file; without root/ZFS a test reports SKIP:

```console
cmake -S . -B build && cmake --build build
sudo ctest --test-dir build --output-on-failure
```

Tests cover: per-snapshot byte-for-byte content across smaller and larger target
block sizes, single-snapshot volumes, hole/`FREE` replication with sparseness,
`volsize` increasing and decreasing across snapshots, and property carry-over.

## License & disclaimer

Licensed under the [MIT License](LICENSE).

This tool was written partially with the help of AI tools. It manipulates real
storage and, when swapping names, the original dataset. **Use at your own risk** —
there is no warranty of any kind (see the LICENSE). Review the code, keep backups,
and try it on non-critical data first; `--no-swap` and `--verify all` are there to
let you inspect and validate the result before committing to it.
