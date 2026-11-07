#!/usr/bin/env python3
"""Fail-closed source-only provenance: exact bytes from bool/i64 PR #1483,
independently repeated in conformance #533. NOT compiled .atto guest evidence.
"""
import hashlib
from pathlib import Path
import subprocess
import sys

EXPECTED = {
    "pair-positive-bool-write.atto": ("eaa213958b79c786c17b3b0a53ccbdd7bb0a342e", "bcfd5611a10dab48a7b44bc67e65eb77c3283a65b8e4a460e7427c5ecc342d8b"),
    "pair-positive-swap-tail.atto": ("a2d69807bdd32e5124f7118505a233c0ba41a351", "b249035038e6ba64ed197a94460e81ccf0b7e4ab2936d894817b0cc99204f110"),
    "pair-negative-bounds-trap.atto": ("33f8c5adc99e3b31b06c934018b75e60d3d4f6d7", "e2ea752d26fc3d6136564bae11e7de000f1b5c1ffe6f9881d48f0a19a010bed1"),
    "pair-negative-bool-as-int.atto": ("baafab8be5662bb892c15d7a0a4f0928c10ccfc9", "2019b42293863650a7c1d633f68c4516d966f928358e99630bc9d325ffdb3ec5"),
    "pair-negative-bool-to-int-return.atto": ("713b154dea2f29e689d1eaee035e101cd49a48da", "eb0a6559b7e374d03d542a0bbbc079dfb56f0d432aa1c58d07bedcb5f70d3653"),
    "pair-negative-immutable-bool-set.atto": ("9b69da6b6b072033714806a219f3b1922419317e", "fc9a597227d5df61adfceaae32f79730d15bf962dcfa1450026c31a7d9ebe0cf"),
    "pair-negative-bool-write-int.atto": ("1e700dc6f00539ffaaea6a4dce569d361ae17069", "5dd0f2b686e5a52cfcaa8438816ba1b56f2edb34b46f49e7cd77a33975ef0c78"),
    "pair-negative-f64-unimplemented.atto": ("06feb1d952ce28af4a1e2aa67a5444a578d45ffb", "bee2a89f4beee83031eca61bfd142dd8c50a047c7e582a25add747e2d33a8408"),
}

def require_pair_sources(directory: Path, *, verify_git: bool = True) -> None:
    actual = {p.name for p in directory.glob("pair-*.atto")}
    if actual != set(EXPECTED):
        raise ValueError(f"pair source inventory differs: {sorted(actual ^ set(EXPECTED))}")
    for name, (git_sha, digest) in EXPECTED.items():
        file = directory / name
        if file.is_symlink() or not file.is_file():
            raise ValueError(f"unsafe or missing fixture: {name}")
        contents = file.read_bytes()
        if not contents or len(contents) > 64 * 1024:
            raise ValueError(f"empty or oversized fixture: {name}")
        blob = b"blob " + str(len(contents)).encode("ascii") + b"\\x00" + contents
        # Git hashes prefix the byte payload with blob <decimal-length><NUL>.
        observed_git = hashlib.sha1(blob).hexdigest()
        observed_sha256 = hashlib.sha256(contents).hexdigest()
        if observed_git != git_sha or observed_sha256 != digest:
            raise ValueError(f"source changed from independently pinned corpus: {name}")
        if verify_git:
            path = file.as_posix()
            committed = subprocess.check_output(
                ["git", "rev-parse", "HEAD:" + path], text=True).strip()
            if committed != git_sha:
                raise ValueError(f"Git HEAD content differs from approved source: {name}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_dop_pair_source_blobs.py <repo-relative-fixtures>")
    require_pair_sources(Path(sys.argv[1]))
    print("DOP_PAIR_SOURCE_BLOBS_PINNED source_only=8 guest_executed=NO")
