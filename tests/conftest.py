"""Pytest fixtures: a file-backed test pool and a source-zvol factory.

These tests require root and a working ZFS (they create a real pool on a loop
file).  Run them inside the QEMU guest via the test harness, not on the host.
"""

from __future__ import annotations

import os
import subprocess
import uuid

import pytest

import helpers

POOL = "testpool"
VDEV = "/var/tmp/zvol-reblock-vdev.img"
VDEV_SIZE = "4G"


def _require_root_and_zfs():
    if os.geteuid() != 0:
        pytest.skip("ZFS tests require root")
    if subprocess.run(["zpool", "version"], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        pytest.skip("ZFS not available")


@pytest.fixture(scope="session")
def pool():
    _require_root_and_zfs()
    subprocess.run(["zpool", "destroy", "-f", POOL],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["truncate", "-s", VDEV_SIZE, VDEV], check=True)
    # compression=off so that "holes" (discard) are distinguishable from
    # zero-filled blocks when we assert sparseness.
    subprocess.run(["zpool", "create", "-f", "-O", "compression=off",
                    POOL, VDEV], check=True)
    try:
        yield POOL
    finally:
        subprocess.run(["zpool", "destroy", "-f", POOL],
                       stderr=subprocess.DEVNULL)
        try:
            os.unlink(VDEV)
        except FileNotFoundError:
            pass


@pytest.fixture
def make_zvol(pool):
    """Factory: create a readonly-capable source zvol and clean it (+ its
    migration artifacts) up afterwards."""
    created: list[str] = []

    def _make(volblocksize: str, size: str = "64M", *, name: str | None = None):
        name = name or f"src{uuid.uuid4().hex[:8]}"
        dataset = f"{pool}/{name}"
        helpers.zfs("create", "-V", size, "-b", volblocksize,
                    "-o", "snapdev=visible", dataset)
        created.append(dataset)
        return dataset

    yield _make

    for dataset in created:
        for name in (dataset, f"{dataset}-new", f"{dataset}-old"):
            subprocess.run(["zfs", "destroy", "-r", name],
                           stderr=subprocess.DEVNULL)


@pytest.fixture
def migrate():
    """Invoke the CLI under test in-process against the given source/blocksize."""
    import importlib.util

    here = os.path.dirname(__file__)
    spec = importlib.util.spec_from_file_location(
        "zvol_reblock", os.path.join(here, "..", "zvol_reblock.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    def _run(source: str, volblocksize: str, *extra: str) -> int:
        argv = [source, volblocksize, *extra]
        return mod.main(argv)

    return _run
