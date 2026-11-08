"""Build the standalone DOOMHud Torque volume from its source tree."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parent
OUTPUT = ROOT / "DOOMHud.vl2"
VOLUME_DIRS = ("fonts", "gui", "scripts", "textures")

def main() -> None:
    files = sorted(
        path
        for directory in VOLUME_DIRS
        for path in (ROOT / directory).rglob("*")
        if path.is_file()
    )
    with ZipFile(OUTPUT, "w", compression=ZIP_DEFLATED, compresslevel=9) as volume:
        for path in files:
            name = path.relative_to(ROOT).as_posix()
            info = ZipInfo(name, date_time=(2026, 11, 7, 0, 0, 0))
            info.compress_type = ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            volume.writestr(info, path.read_bytes(), compress_type=ZIP_DEFLATED, compresslevel=9)


if __name__ == "__main__":
    main()
