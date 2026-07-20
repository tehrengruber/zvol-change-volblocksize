"""Holes (FREE records) must be replicated: dest reads zeros and stays sparse."""

from __future__ import annotations

import helpers

MiB = 1024 * 1024
HOLE_OFF = 16 * MiB
HOLE_LEN = 16 * MiB


def test_hole_replicated_and_sparse(make_zvol, migrate):
    source = make_zvol("8k", size="64M")
    helpers.write_pattern(source, 0, 64 * MiB, seed=1)
    helpers.snapshot(source, "full")

    helpers.punch_hole(source, HOLE_OFF, HOLE_LEN)
    helpers.snapshot(source, "holed")

    helpers.zfs("set", "readonly=on", source)
    assert migrate(source, "32k", "-v") == 0

    backup = f"{source}-old"

    # 1. Per-snapshot content matches (incl. the hole reading back as zeros).
    assert helpers.snapshot_names(source) == ["full", "holed"]
    for snap in ("full", "holed"):
        assert (helpers.sha256_device(f"{source}@{snap}")
                == helpers.sha256_device(f"{backup}@{snap}")), snap

    # 2. The punched region reads back as zeros on the migrated volume.
    assert helpers.read_range(f"{source}@holed", HOLE_OFF, HOLE_LEN) == b"\0" * HOLE_LEN

    # 3. Sparseness: the migrated live volume must not have re-materialised the
    #    hole as allocated zeros.  With compression=off, a full 64M volume refers
    #    ~64M; after a 16M hole it should refer well under that.
    referred = int(helpers.get_prop(source, "referenced"))
    assert referred < 56 * MiB, f"migrated volume not sparse: refer={referred}"
