#!/usr/bin/env python3
"""Generate the default (fallback) Matrix skybox faces with ffmpeg: green digital rain over a faint grid.

Run from anywhere: python3 scripts/Matrix/tools/make_faces.py
Needs ffmpeg with drawtext/drawgrid/geq and a monospace font (fc-match monospace). The PNGs are committed, so
building the pack does not need ffmpeg."""

import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "textures" / "matrix" / "skies"
SIZE = 512
GLYPHS = "0123456789ABCDEF:.=*+<>"


def font_file() -> str:
    result = subprocess.run(["fc-match", "monospace", "file"], capture_output=True, text=True)
    path = result.stdout.strip().split("file=")[-1].rstrip(":") if result.returncode == 0 else ""
    if not path or not Path(path).exists():
        sys.exit("no monospace font found (fc-match monospace)")
    return path


def rain_filters(rng: random.Random, font: str, columns: int, scratch: Path) -> list[str]:
    filters = []
    step = SIZE // columns
    for column in range(columns):
        x = column * step + 3
        length = rng.randint(8, 22)
        top = rng.randint(-40, SIZE - 60)
        glyphs = [rng.choice(GLYPHS) for _ in range(length)]
        # a long dim streak, a brighter middle, and a pale head
        for layer, (alpha, count) in enumerate(((0.28, length), (0.55, max(3, length // 2)), (0.95, 2))):
            text_path = scratch / f"c{column}_{layer}.txt"
            text_path.write_text("\n".join(glyphs[-count:]))
            y = top + (length - count) * 17
            color = "0xb8ffb8" if layer == 2 else "0x00ff55"
            filters.append(
                f"drawtext=fontfile={font}:textfile={text_path}:x={x}:y={y}:fontsize=16:line_spacing=1:"
                f"fontcolor={color}@{alpha}")
    return filters


def render(name: str, kind: str, seed: int, font: str) -> None:
    rng = random.Random(seed)
    base = f"color=c=black:s={SIZE}x{SIZE}:d=1"
    chain = []
    if kind == "wall":
        # a darker band at mid height where the walls meet the ground, brighter toward the top
        chain.append("geq=r=0:g='6+34*pow(abs(Y-256)/256,1.2)':b='3+10*pow(abs(Y-256)/256,1.2)'")
    elif kind == "up":
        chain.append("geq=r=0:g='8+60*exp(-pow(hypot(X-256,Y-256)/190,2))':b='4+16*exp(-pow(hypot(X-256,Y-256)/190,2))'")
    else:
        chain.append("geq=r=0:g='5+24*exp(-pow(hypot(X-256,Y-256)/230,2))':b='2+8*exp(-pow(hypot(X-256,Y-256)/230,2))'")
    chain.append(f"drawgrid=w=64:h=64:t=1:c=0x00ff55@{0.16 if kind != 'down' else 0.30}")
    if kind != "down":
        with tempfile.TemporaryDirectory() as scratch:
            chain += rain_filters(rng, font, 24 if kind == "wall" else 14, Path(scratch))
            # The engine maps these textures mirrored left-right (walls and the top), so mirror them here and the
            # glyphs read correctly from inside the room.
            chain.append("hflip")
            chain.append("format=rgb24")
            run_ffmpeg(base, chain, name)
        return
    chain.append("format=rgb24")
    run_ffmpeg(base, chain, name)


def run_ffmpeg(base: str, chain: list[str], name: str) -> None:
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", base,
               "-vf", ",".join(chain), "-frames:v", "1", "-update", "1", str(OUT / f"Matrix_{name}.png")]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        sys.exit(f"ffmpeg failed for {name}: {result.stderr[-400:]}")


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    font = font_file()
    for seed, (name, kind) in enumerate((("FR", "wall"), ("RT", "wall"), ("BK", "wall"), ("LF", "wall"),
                                         ("UP", "up"), ("DN", "down"))):
        render(name, kind, 1000 + seed, font)
        print("wrote", OUT / f"Matrix_{name}.png")


if __name__ == "__main__":
    main()
