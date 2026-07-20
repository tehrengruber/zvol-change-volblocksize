"""Test helpers for driving zvols and comparing their contents."""

from __future__ import annotations

import hashlib
import os
import random
import subprocess

CHUNK = 4 * 1024 * 1024


def zfs(*args: str) -> str:
    return subprocess.run(["zfs", *args], check=True,
                          stdout=subprocess.PIPE, text=True).stdout


def zpool(*args: str) -> str:
    return subprocess.run(["zpool", *args], check=True,
                          stdout=subprocess.PIPE, text=True).stdout


def get_prop(dataset: str, prop: str) -> str:
    return zfs("get", "-Hp", "-o", "value", prop, dataset).strip()


def dev_path(dataset: str) -> str:
    """Path to a zvol/snapshot block device, waited for until present."""
    path = f"/dev/zvol/{dataset}"
    subprocess.run(["udevadm", "settle"], stderr=subprocess.DEVNULL)
    for _ in range(150):
        if os.path.exists(path):
            return path
        subprocess.run(["sleep", "0.1"])
    raise AssertionError(f"device never appeared: {path}")


def snapshot(dataset: str, name: str) -> str:
    zfs("snapshot", f"{dataset}@{name}")
    return f"{dataset}@{name}"


def set_volsize(dataset: str, size: int) -> None:
    zfs("set", f"volsize={size}", dataset)


def _rng_bytes(seed: int, size: int) -> bytes:
    return random.Random(seed).randbytes(size)


def write_pattern(dataset: str, offset: int, size: int, seed: int) -> None:
    """Write `size` deterministic bytes derived from `seed` at `offset`."""
    data = _rng_bytes(seed, size)
    fd = os.open(dev_path(dataset), os.O_RDWR)
    try:
        os.pwrite(fd, data, offset)
        os.fsync(fd)
    finally:
        os.close(fd)


def punch_hole(dataset: str, offset: int, length: int) -> None:
    subprocess.run(["blkdiscard", "--offset", str(offset), "--length",
                    str(length), dev_path(dataset)], check=True)


def read_range(dataset: str, offset: int, size: int) -> bytes:
    fd = os.open(dev_path(dataset), os.O_RDONLY)
    try:
        out = bytearray()
        pos = offset
        remaining = size
        while remaining:
            buf = os.pread(fd, min(CHUNK, remaining), pos)
            if not buf:
                break
            out += buf
            pos += len(buf)
            remaining -= len(buf)
        return bytes(out)
    finally:
        os.close(fd)


def sha256_device(dataset: str, size: int | None = None) -> str:
    """SHA-256 over the first `size` bytes (default: whole volume)."""
    if size is None:
        size = int(get_prop(dataset, "volsize"))
    fd = os.open(dev_path(dataset), os.O_RDONLY)
    try:
        h = hashlib.sha256()
        pos = 0
        while pos < size:
            buf = os.pread(fd, min(CHUNK, size - pos), pos)
            if not buf:
                h.update(b"\0" * (size - pos))
                break
            h.update(buf)
            pos += len(buf)
        return h.hexdigest()
    finally:
        os.close(fd)


def snapshot_names(dataset: str) -> list[str]:
    out = zfs("list", "-H", "-d", "1", "-t", "snapshot", "-o", "name",
              "-s", "creation", dataset)
    return [line.split("@", 1)[1] for line in out.splitlines() if line]
