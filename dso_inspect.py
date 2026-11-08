"""Read-only structural inspector for TorqueScript DSO files."""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass
from pathlib import Path


class DsoFormatError(ValueError):
    """The byte stream is truncated or internally inconsistent."""


@dataclass(frozen=True)
class IdentifierFixup:
    string_offset: int
    code_positions: tuple[int, ...]


@dataclass(frozen=True)
class DsoImage:
    version: int
    global_strings: bytes
    global_floats: tuple[float, ...]
    function_strings: bytes
    function_floats: tuple[float, ...]
    code_tokens: tuple[int, ...]
    line_break_pairs: tuple[tuple[int, int], ...]
    identifier_fixups: tuple[IdentifierFixup, ...]


class _Reader:
    def __init__(self, data: bytes):
        self.data = memoryview(data)
        self.offset = 0

    def _require(self, count: int) -> None:
        if count < 0 or count > len(self.data) - self.offset:
            raise DsoFormatError(f"truncated DSO at byte offset {self.offset}")

    def u32(self) -> int:
        self._require(4)
        value = struct.unpack_from("<I", self.data, self.offset)[0]
        self.offset += 4
        return value

    def f64(self) -> float:
        self._require(8)
        value = struct.unpack_from("<d", self.data, self.offset)[0]
        self.offset += 8
        return value

    def bytes(self, count: int) -> bytes:
        self._require(count)
        value = self.data[self.offset:self.offset + count].tobytes()
        self.offset += count
        return value

    def length_prefixed_bytes(self) -> bytes:
        return self.bytes(self.u32())

    def float_table(self) -> tuple[float, ...]:
        count = self.u32()
        if count > (len(self.data) - self.offset) // 8:
            raise DsoFormatError(f"float table exceeds DSO at byte offset {self.offset}")
        return tuple(self.f64() for _ in range(count))

    def code_tokens(self) -> tuple[int, ...]:
        count = self.u32()
        line_break_count = self.u32()
        # Each token occupies at least one byte; line pairs occupy two U32s.
        if count > len(self.data) - self.offset:
            raise DsoFormatError(f"code token count exceeds DSO at byte offset {self.offset}")
        tokens = []
        for _ in range(count):
            self._require(1)
            token = self.data[self.offset]
            self.offset += 1
            if token == 0xFF:
                token = self.u32()
            tokens.append(token)

        if line_break_count > (len(self.data) - self.offset) // 8:
            raise DsoFormatError(f"line-break table exceeds DSO at byte offset {self.offset}")
        pairs = tuple((self.u32(), self.u32()) for _ in range(line_break_count))
        return tuple(tokens), pairs


def parse_dso(data: bytes) -> DsoImage:
    """Parse DSO framing, pools, code tokens, line maps, and identifier fixups."""
    reader = _Reader(data)
    version = reader.u32()
    global_strings = reader.length_prefixed_bytes()
    global_floats = reader.float_table()
    function_strings = reader.length_prefixed_bytes()
    function_floats = reader.float_table()
    code_tokens, line_break_pairs = reader.code_tokens()

    fixup_group_count = reader.u32()
    if fixup_group_count > (len(reader.data) - reader.offset) // 8:
        raise DsoFormatError(f"identifier table exceeds DSO at byte offset {reader.offset}")
    fixups = []
    for _ in range(fixup_group_count):
        string_offset = reader.u32()
        site_count = reader.u32()
        if site_count > (len(reader.data) - reader.offset) // 4:
            raise DsoFormatError(f"identifier sites exceed DSO at byte offset {reader.offset}")
        sites = tuple(reader.u32() for _ in range(site_count))
        if any(site >= len(code_tokens) for site in sites):
            raise DsoFormatError("identifier fixup points outside the code token stream")
        fixups.append(IdentifierFixup(string_offset, sites))

    if reader.offset != len(reader.data):
        raise DsoFormatError(f"{len(reader.data) - reader.offset} trailing DSO bytes")

    return DsoImage(
        version=version,
        global_strings=global_strings,
        global_floats=global_floats,
        function_strings=function_strings,
        function_floats=function_floats,
        code_tokens=code_tokens,
        line_break_pairs=line_break_pairs,
        identifier_fixups=tuple(fixups),
    )


def _summary(image: DsoImage, size: int) -> dict[str, object]:
    return {
        "size": size,
        "version": image.version,
        "global_string_bytes": len(image.global_strings),
        "global_float_count": len(image.global_floats),
        "function_string_bytes": len(image.function_strings),
        "function_float_count": len(image.function_floats),
        "code_token_count": len(image.code_tokens),
        "line_break_count": len(image.line_break_pairs),
        "identifier_fixup_groups": len(image.identifier_fixups),
        "identifier_fixup_sites": sum(len(f.code_positions) for f in image.identifier_fixups),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dso", type=Path, help="DSO file to inspect; never executed")
    args = parser.parse_args()
    try:
        data = args.dso.read_bytes()
        image = parse_dso(data)
    except (OSError, DsoFormatError) as exc:
        parser.error(str(exc))
    print(json.dumps(_summary(image, len(data)), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
