"""The migrated zvol is created directly with the source's *final* properties
(property changes across snapshots are not replayed), and all explicitly-set
properties -- including user properties -- are carried over."""

from __future__ import annotations

import helpers

MiB = 1024 * 1024


def test_final_properties_are_carried(make_zvol, migrate):
    source = make_zvol("8k", size="32M")

    # A property that CHANGES between snapshots: only the final value should end
    # up on the migrated zvol, since it is created once with the current props.
    helpers.zfs("set", "compression=off", source)
    helpers.write_pattern(source, 0, 8 * MiB, seed=1)
    helpers.snapshot(source, "s0")

    helpers.zfs("set", "compression=lz4", source)
    helpers.zfs("set", "checksum=sha256", source)
    helpers.zfs("set", "logbias=throughput", source)
    helpers.zfs("set", "com.example:role=database", source)   # user property
    helpers.write_pattern(source, 8 * MiB, 8 * MiB, seed=2)
    helpers.snapshot(source, "s1")

    helpers.zfs("set", "readonly=on", source)
    assert migrate(source, "16k") == 0

    # After the swap the original name refers to the migrated zvol.
    assert int(helpers.get_prop(source, "volblocksize")) == 16 * 1024
    assert helpers.get_prop(source, "compression") == "lz4"       # final, not "off"
    assert helpers.get_prop(source, "checksum") == "sha256"
    assert helpers.get_prop(source, "logbias") == "throughput"
    # User properties must survive too (an allowlist would silently drop these).
    assert helpers.get_prop(source, "com.example:role") == "database"

    # Managed properties are set by the tool, not blindly copied.
    assert int(helpers.get_prop(source, "volblocksize")) != 8 * 1024

    # Sanity: content is still correct per snapshot after the property changes.
    for snap in ("s0", "s1"):
        assert (helpers.sha256_device(f"{source}@{snap}")
                == helpers.sha256_device(f"{source}-old@{snap}")), snap
