#!/usr/bin/env bash
# Build the harness image and run the test suite inside a Podman container that
# boots a QEMU/KVM guest.  Requires /dev/kvm on the host.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$HERE/.." && pwd)"
IMAGE="${IMAGE:-zvol-reblock-test}"

podman build -t "$IMAGE" "$HERE"

exec podman run --rm -it \
    --device /dev/kvm \
    -v "$REPO_DIR":/repo:ro,Z \
    -v zvol-reblock-cache:/cache \
    -e REPO_DIR=/repo \
    "$IMAGE"
