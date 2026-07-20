# zvol-reblock

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
   `readonly`, `snapdev`, reservations) or that are creation-time/key material
   (encryption). Property *changes across the snapshot history are not replayed*;
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

## Requirements & preconditions

- OpenZFS with `zfs`, `zpool`, and `zstream` (or the older `zstreamdump`).
- **Run as root.**
- The source zvol must be **`readonly=on`** so that its newest snapshot equals
  the live head (this removes any race with a live writer). Set it before
  running, or pass `--force` to skip the check.
- The source must have at least one snapshot.

## Usage

```console
# make the source quiescent
zfs snapshot pool/vol@now          # if the current head isn't already a snapshot
zfs set readonly=on pool/vol

# migrate 8k -> 16k, keeping pool/vol-old as a backup
sudo ./zvol_reblock.py pool/vol 16k

# inspect the result, then drop the backup when satisfied
zfs destroy -r pool/vol-old
```

Options:

| flag | meaning |
| --- | --- |
| `--dest NAME` | intermediate name for the new zvol (default `<source>-new`) |
| `--backup-suffix S` | suffix for the preserved original (default `-old`) |
| `--no-swap` | build the new zvol but don't rename anything |
| `--destroy-backup` | destroy the original after a successful swap (default: keep) |
| `--force` | skip the `readonly=on` precondition check |
| `--dry-run` | print the planned actions without changing anything |
| `-v` | verbose progress on stderr |

## Limitations

- Encrypted datasets require their keys loaded (data is read as plaintext from
  the snapshot device); raw/`-w` sends are not used.
- Assumes the newest snapshot equals the head (enforced by `readonly=on`).
- `volsize` must be a multiple of the new `volblocksize`; if the source size
  isn't, the new volume is rounded up to the next block (the extra tail reads as
  zeros).

## Testing

ZFS needs a kernel module, which is awkward to install on the host (especially
Arch). The suite therefore runs inside a disposable Ubuntu QEMU/KVM guest.

```console
# Podman-wrapped (needs /dev/kvm):
./test-harness/run.sh

# or drive QEMU directly (needs qemu + xorriso + curl on the host):
./test-harness/run-qemu.sh

# convenience:
make test          # -> run.sh
make test-qemu     # -> run-qemu.sh
```

Either path downloads an Ubuntu cloud image (cached), boots it, installs
`zfsutils-linux`, mounts this repo over 9p read-only, and runs `pytest`. The
container/script exits with the guest's pytest exit code.

If you already have a working ZFS environment and just want to run the tests
directly there:

```console
sudo python3 -m pytest -v tests/
```

Tests cover: per-snapshot byte-for-byte content across smaller and larger target
block sizes, single-snapshot volumes, hole/`FREE` replication with sparseness,
and `volsize` increasing and decreasing across snapshots.
