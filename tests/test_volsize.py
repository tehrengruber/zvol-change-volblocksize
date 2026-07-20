"""volsize changing across snapshots must be reproduced in both directions."""

from __future__ import annotations

import helpers

MiB = 1024 * 1024


def _assert_snapshots_match(source: str, backup: str):
    assert helpers.snapshot_names(source) == helpers.snapshot_names(backup)
    for snap in helpers.snapshot_names(source):
        old_size = int(helpers.get_prop(f"{backup}@{snap}", "volsize"))
        new_size = int(helpers.get_prop(f"{source}@{snap}", "volsize"))
        assert new_size == old_size, f"volsize mismatch at {snap}"
        assert (helpers.sha256_device(f"{source}@{snap}", old_size)
                == helpers.sha256_device(f"{backup}@{snap}", old_size)), \
            f"content mismatch at {snap}"


def test_volsize_increases(make_zvol, migrate):
    source = make_zvol("8k", size="32M")
    helpers.write_pattern(source, 0, 16 * MiB, seed=1)
    helpers.snapshot(source, "small")

    helpers.set_volsize(source, 64 * MiB)
    helpers.write_pattern(source, 40 * MiB, 16 * MiB, seed=2)
    helpers.snapshot(source, "grown")

    helpers.zfs("set", "readonly=on", source)
    assert migrate(source, "32k", "-v") == 0
    _assert_snapshots_match(source, f"{source}-old")


def test_volsize_decreases(make_zvol, migrate):
    source = make_zvol("8k", size="64M")
    helpers.write_pattern(source, 0, 48 * MiB, seed=3)
    helpers.snapshot(source, "big")

    helpers.set_volsize(source, 32 * MiB)   # shrink; tail truncated
    helpers.write_pattern(source, 8 * MiB, 4 * MiB, seed=4)
    helpers.snapshot(source, "shrunk")

    helpers.zfs("set", "readonly=on", source)
    assert migrate(source, "32k", "-v") == 0
    _assert_snapshots_match(source, f"{source}-old")
