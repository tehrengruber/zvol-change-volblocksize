#!/usr/bin/env bash
# Build the harness image and run the test suite inside a Podman container that
# boots a QEMU/KVM guest.  Requires /dev/kvm on the host.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$HERE/.." && pwd)"
IMAGE="${IMAGE:-zvol-change-volblocksize-test}"

podman build -t "$IMAGE" "$HERE"

# Only allocate a TTY when stdin is one, so CI / piped invocations don't break.
TTY=()
[ -t 0 ] && TTY=(-t)

exec podman run --rm -i "${TTY[@]}" \
    --device /dev/kvm \
    -v "$REPO_DIR":/repo:ro,Z \
    -v zvol-change-volblocksize-cache:/cache \
    -e REPO_DIR=/repo \
    "$IMAGE"
