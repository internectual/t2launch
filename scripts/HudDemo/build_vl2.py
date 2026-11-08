"""Build the standalone HUD demo VL2 from repository source/assets."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
ASSETS = ROOT / "assets"
OUTPUT = ROOT / "HudDemo.vl2"


def main() -> None:
    entries: dict[str, Path] = {
        "scripts/autoexec/hudBarDemo.cs": REPO_ROOT / "tests/hudBarDemo.cs",
    }
    for path in sorted((ASSETS / "hudBarDemo_assets").rglob("*.png")):
        relative = path.relative_to(ASSETS).as_posix()
        entries[relative] = path
        entries[f"textures/{relative}"] = path
    for path in sorted((ASSETS / "textures/gui").iterdir()):
        if not path.is_file():
            continue
        entries[f"gui/{path.name}"] = path
        entries[f"textures/gui/{path.name}"] = path
    # The compass's circled N/E/S/W markers are the stock
    # textures/gui/hud_north..hud_west in textures.vl2; nothing to bundle.

    with ZipFile(OUTPUT, "w", compression=ZIP_DEFLATED, compresslevel=9) as volume:
        for name, path in sorted(entries.items()):
            info = ZipInfo(name, date_time=(2026, 11, 7, 0, 0, 0))
            info.compress_type = ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            volume.writestr(info, path.read_bytes(), compress_type=ZIP_DEFLATED, compresslevel=9)


if __name__ == "__main__":
    main()
