"""Per-snapshot content correctness across a volblocksize change."""

from __future__ import annotations

import helpers
import pytest

MiB = 1024 * 1024


def build_history(dataset: str) -> None:
    """Create a few snapshots with overlapping writes and overwrites."""
    helpers.write_pattern(dataset, 0, 4 * MiB, seed=1)
    helpers.write_pattern(dataset, 20 * MiB, 4 * MiB, seed=2)
    helpers.snapshot(dataset, "s0")

    helpers.write_pattern(dataset, 2 * MiB, 3 * MiB, seed=3)      # overwrite
    helpers.write_pattern(dataset, 40 * MiB, 5 * MiB, seed=4)     # new region
    helpers.snapshot(dataset, "s1")

    helpers.write_pattern(dataset, 0, 1 * MiB, seed=5)           # overwrite head
    helpers.write_pattern(dataset, 60 * MiB, 2 * MiB, seed=6)     # near end
    helpers.snapshot(dataset, "s2")


def assert_migrated_equal(source: str, backup: str, target_bytes: int):
    assert helpers.snapshot_names(source) == helpers.snapshot_names(backup)
    assert int(helpers.get_prop(source, "volblocksize")) == target_bytes
    for snap in helpers.snapshot_names(source):
        new = helpers.sha256_device(f"{source}@{snap}")
        old = helpers.sha256_device(f"{backup}@{snap}")
        assert new == old, f"content mismatch at snapshot {snap}"
    # Live head must match too.
    assert helpers.sha256_device(source) == helpers.sha256_device(backup)


@pytest.mark.parametrize("src_bs,dst_bs", [("8k", "32k"), ("64k", "8k")])
def test_blocksize_change_preserves_content(make_zvol, migrate, src_bs, dst_bs):
    source = make_zvol(src_bs, size="64M")
    build_history(source)
    helpers.zfs("set", "readonly=on", source)

    rc = migrate(source, dst_bs, "-v")
    assert rc == 0

    backup = f"{source}-old"
    target_bytes = int(dst_bs[:-1]) * 1024
    assert_migrated_equal(source, backup, target_bytes)


def test_single_snapshot(make_zvol, migrate):
    source = make_zvol("16k", size="32M")
    helpers.write_pattern(source, 0, 8 * MiB, seed=10)
    helpers.write_pattern(source, 16 * MiB, 4 * MiB, seed=11)
    helpers.snapshot(source, "only")
    helpers.zfs("set", "readonly=on", source)

    rc = migrate(source, "64k", "-v")
    assert rc == 0
    assert_migrated_equal(source, f"{source}-old", 64 * 1024)


def test_no_swap_leaves_new(make_zvol, migrate):
    source = make_zvol("8k", size="16M")
    helpers.write_pattern(source, 0, 4 * MiB, seed=20)
    helpers.snapshot(source, "s0")
    helpers.zfs("set", "readonly=on", source)

    rc = migrate(source, "16k", "--no-swap")
    assert rc == 0
    dest = f"{source}-new"
    assert int(helpers.get_prop(dest, "volblocksize")) == 16 * 1024
    assert helpers.sha256_device(f"{dest}@s0") == helpers.sha256_device(f"{source}@s0")
