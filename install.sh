#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/t2launch/venv"
BIN_DIR="$HOME/.local/bin"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/python" -m pip install --upgrade pip
"$VENV_DIR/bin/python" -m pip install -r "$ROOT_DIR/requirements.txt"
mkdir -p "$BIN_DIR" "$APP_DIR"
printf '#!/usr/bin/env bash\nexec "%s/bin/python" "%s/t2launch_app.py" "$@"\n' \
    "$VENV_DIR" "$ROOT_DIR" > "$BIN_DIR/t2launch"
chmod +x "$BIN_DIR/t2launch"
python3 - "$ROOT_DIR" "$BIN_DIR/t2launch" "$APP_DIR/t2launch.desktop" <<'PY'
import sys
from pathlib import Path

root, executable, output = map(Path, sys.argv[1:])
source = (root / "t2launch.desktop").read_text()
source = source.replace("Exec=t2launch", f"Exec={executable}")
output.write_text(source)
PY
printf 'Installed T2Launch. Start it with: %s\n' "$BIN_DIR/t2launch"
