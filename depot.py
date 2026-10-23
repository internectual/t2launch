#!/usr/bin/env python3
"""Fetch a missing map asset from the TribesNext asset depot.

The game disconnects with "Unable to load interior: X", "Unable to load
shape: X" or "You are missing a file needed to play this mission: X" when a
server's mission uses a file the client doesn't have. The hook runs this
helper with that file name; it finds the file in the depot index, checks its
size and MD5, and saves it under the game's user base folder, where the game
finds loose files once its mod paths are rescanned.

Prints one line: "ok <saved path>" or "error <reason>".
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

INDEX_URL = "https://www.tribesnext.com/files/depot.txt"
FILE_BASE_URL = "https://depot.tribes2.net/"
INDEX_MAX_AGE_SECONDS = 24 * 60 * 60
INDEX_MAX_BYTES = 16_000_000
FILE_MAX_BYTES = 64_000_000
TIMEOUT_SECONDS = 30
# The folders a mission's interiors, shapes and terrains are loaded from.
ASSET_FOLDERS = {".dif": "interiors", ".dts": "shapes", ".ter": "terrains"}
USER_AGENT = "T2Launch depot helper"


@dataclass(frozen=True)
class DepotEntry:
    size: int
    md5: str
    path: str


def safe_relative_path(text: str) -> PurePosixPath | None:
    """The path as a relative POSIX path inside its folder, or None if unsafe."""
    if not text or "\\" in text or "\0" in text or text.startswith("/"):
        return None
    path = PurePosixPath(text)
    if any(part in ("", ".", "..") for part in path.parts):
        return None
    return path


def parse_index(text: str) -> list[DepotEntry]:
    """Parse "size md5 path" lines; paths may contain spaces."""
    entries = []
    for line in text.splitlines():
        fields = line.strip().split(" ", 2)
        if len(fields) != 3 or not fields[0].isdigit() or len(fields[1]) != 32:
            continue
        try:
            int(fields[1], 16)
        except ValueError:
            continue
        if safe_relative_path(fields[2]) is None:
            continue
        entries.append(DepotEntry(int(fields[0]), fields[1].lower(), fields[2]))
    return entries


def asset_path(name: str) -> PurePosixPath | None:
    """The depot path the game looks for, from the name in its error message."""
    # The terrain message ends its sentence with a full stop.
    path = safe_relative_path(name.strip().rstrip("."))
    if path is None:
        return None
    folder = ASSET_FOLDERS.get(path.suffix.lower())
    if folder is None:
        return None
    if path.parts[0].lower() != folder:
        path = PurePosixPath(folder) / path
    return path


def find_entry(entries: list[DepotEntry], wanted: PurePosixPath) -> DepotEntry | None:
    """The exact path ignoring case, else the shortest same-named file in its folder."""
    wanted_text = str(wanted).lower()
    folder = wanted.parts[0].lower()
    base = wanted.name.lower()
    fallback = None
    for entry in entries:
        lowered = entry.path.lower()
        if lowered == wanted_text:
            return entry
        if lowered.startswith(folder + "/") and lowered.rsplit("/", 1)[-1] == base:
            if fallback is None or len(entry.path) < len(fallback.path):
                fallback = entry
    return fallback


def fetch(url: str, limit: int) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("download is larger than expected")
    return data


def load_index(cache: Path, fetcher=fetch) -> list[DepotEntry]:
    try:
        fresh = time.time() - cache.stat().st_mtime < INDEX_MAX_AGE_SECONDS
    except OSError:
        fresh = False
    if fresh:
        return parse_index(cache.read_text(encoding="utf-8", errors="replace"))
    text = fetcher(INDEX_URL, INDEX_MAX_BYTES).decode("utf-8", errors="replace")
    entries = parse_index(text)
    if entries:
        cache.parent.mkdir(parents=True, exist_ok=True)
        temporary = cache.with_suffix(".tmp")
        temporary.write_text(text, encoding="utf-8")
        os.replace(temporary, cache)
    return entries


def file_md5(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(name: str, destination: Path, cache: Path, fetcher=fetch) -> Path:
    """Save the depot file for name under destination and return its path."""
    wanted = asset_path(name)
    if wanted is None:
        raise ValueError(f"not an interior, shape or terrain: {name}")
    entry = find_entry(load_index(cache, fetcher), wanted)
    if entry is None:
        raise LookupError(f"{wanted.name} is not in the depot")
    if entry.size > FILE_MAX_BYTES:
        raise ValueError(f"{entry.path} is too large ({entry.size} bytes)")
    # Save where the game looks, with the name it asked for: its file lookup
    # is case-sensitive, and some depot files sit in a map's own subfolder.
    target = destination / wanted
    if target.is_file() and target.stat().st_size == entry.size and file_md5(target) == entry.md5:
        raise FileExistsError(f"{target} is already installed")
    data = fetcher(FILE_BASE_URL + urllib.parse.quote(entry.path), entry.size)
    if len(data) != entry.size or hashlib.md5(data).hexdigest() != entry.md5:
        raise ValueError(f"{entry.path} failed its size or MD5 check")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target.parent, prefix=".depot-", delete=False) as handle:
        handle.write(data)
    os.replace(handle.name, target)
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("name", help="file name from the game's disconnect message")
    parser.add_argument("--dest", type=Path,
                        default=Path.home() / ".loki" / "tribes2" / "base")
    parser.add_argument("--cache", type=Path,
                        default=Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
                        / "t2launch" / "depot.txt")
    args = parser.parse_args(argv)
    try:
        saved = download(args.name, args.dest, args.cache)
    except (OSError, ValueError, LookupError) as error:
        print(f"error {error}")
        return 1
    print(f"ok {saved}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
