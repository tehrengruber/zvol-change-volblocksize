#!/usr/bin/env bash
# Run the test suite by booting QEMU directly (no Podman). Requires qemu,
# qemu-img, xorriso and curl on the host, plus /dev/kvm for acceleration.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$HERE/entrypoint.sh"
