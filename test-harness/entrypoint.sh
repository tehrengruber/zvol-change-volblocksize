#!/usr/bin/env bash
# Boot a disposable Ubuntu guest with ZFS and run the CTest suite inside it.
# Works both as a Podman container entrypoint and run directly on a host that
# has qemu + xorriso + curl.  Exits with the guest's ctest exit code.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="${REPO_DIR:-$(cd "$HERE/.." && pwd)}"
CACHE_DIR="${CACHE_DIR:-/var/cache/zvol-change-volblocksize}"
WORK_DIR="${WORK_DIR:-$(mktemp -d /tmp/zvol-change-volblocksize.XXXXXX)}"
IMG_URL="${IMG_URL:-https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img}"
TIMEOUT="${TIMEOUT:-1800}"
MEM="${MEM:-4096}"
SMP="${SMP:-4}"

cleanup() { [ -n "${KEEP_WORK:-}" ] || rm -rf "$WORK_DIR"; }
trap cleanup EXIT

mkdir -p "$CACHE_DIR"
BASE="$CACHE_DIR/$(basename "$IMG_URL")"
if [ ! -f "$BASE" ]; then
    echo ">> downloading base image: $IMG_URL"
    curl -L --fail -o "$BASE.part" "$IMG_URL"
    mv "$BASE.part" "$BASE"
fi

echo ">> preparing overlay + cloud-init seed"
OVERLAY="$WORK_DIR/overlay.qcow2"
qemu-img create -q -f qcow2 -F qcow2 -b "$BASE" "$OVERLAY" 12G

SEED="$WORK_DIR/seed.iso"
xorriso -as mkisofs -quiet -o "$SEED" -V CIDATA -J -r \
    "$HERE/cloud-init/user-data" "$HERE/cloud-init/meta-data"

LOG="$WORK_DIR/console.log"
OUTDIR="$WORK_DIR/guestout"
mkdir -p "$OUTDIR"
echo ">> booting guest (timeout ${TIMEOUT}s), console -> $LOG"

ACCEL=()
[ -e /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)

set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
    "${ACCEL[@]}" -m "$MEM" -smp "$SMP" -nographic -no-reboot \
    -drive file="$OVERLAY",if=virtio,format=qcow2 \
    -drive file="$SEED",if=virtio,format=raw \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -fsdev local,id=repofs,path="$REPO_DIR",security_model=none,readonly=on \
    -device virtio-9p-pci,fsdev=repofs,mount_tag=repo \
    -fsdev local,id=outfs,path="$OUTDIR",security_model=none \
    -device virtio-9p-pci,fsdev=outfs,mount_tag=out \
    2>&1 | tee "$LOG"
qemu_rc=${PIPESTATUS[0]}
set -e

# Prefer the result written to the output share; fall back to the console.
result=""
[ -f "$OUTDIR/result" ] && result="$(tr -dc '0-9' < "$OUTDIR/result")"
if [ -z "$result" ]; then
    result="$(grep -aoE 'ZVOL_CVB_RESULT:[0-9]+' "$LOG" | tail -1 | cut -d: -f2 || true)"
fi
if [ -z "$result" ]; then
    echo ">> FAILURE: no result sentinel (qemu rc=$qemu_rc, likely timeout/boot failure)"
    exit 1
fi
[ -f "$OUTDIR/ctest.log" ] && echo ">> guest ctest.log saved at $OUTDIR/ctest.log"
echo ">> guest ctest exit code: $result"
exit "$result"
