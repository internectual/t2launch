#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/t2launch/venv"
BIN_DIR="$HOME/.local/bin"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"

# shellcheck source=runtime/common.sh
. "$ROOT_DIR/runtime/common.sh"

# Check what launching the game needs. Each requirement names its package on
# Arch, Debian/Ubuntu, and Fedora; missing ones are collected into one hint.
MISSING=()
MISSING_ARCH=() MISSING_DEBIAN=() MISSING_FEDORA=()
need() {  # need DESCRIPTION ARCH DEBIAN FEDORA -- COMMAND...
    local description="$1" arch="$2" debian="$3" fedora="$4"
    shift 5
    if ! "$@" >/dev/null 2>&1; then
        MISSING+=("$description")
        MISSING_ARCH+=("$arch") MISSING_DEBIAN+=("$debian") MISSING_FEDORA+=("$fedora")
    fi
}
compiles_m32() {  # compiles_m32 [LIBRARY...]: a trivial gcc -m32 build links
    local dir status
    dir=$(mktemp -d)
    printf 'int main(void){return 0;}\n' > "$dir/probe.c"
    gcc -m32 -o "$dir/probe" "$dir/probe.c" "$@"
    status=$?
    rm -rf "$dir"
    return "$status"
}

echo "Checking the 32-bit runtime and build tools..."
need "32-bit C library (/lib/ld-linux.so.2)" lib32-glibc libc6:i386 glibc.i686 -- is_elf32 /lib/ld-linux.so.2
need "32-bit OpenGL (libGL.so.1)" lib32-mesa libgl1:i386 mesa-libGL.i686 -- has_32bit_lib libGL.so.1
need "32-bit GLU (libGLU.so.1)" lib32-glu libglu1-mesa:i386 mesa-libGLU.i686 -- has_32bit_lib libGLU.so.1
need "32-bit X11 (libX11.so.6)" lib32-libx11 libx11-6:i386 libX11.i686 -- has_32bit_lib libX11.so.6
need "32-bit Xext (libXext.so.6)" lib32-libxext libxext6:i386 libXext.i686 -- has_32bit_lib libXext.so.6
need "32-bit XRandR (libXrandr.so.2)" lib32-libxrandr libxrandr2:i386 libXrandr.i686 -- has_32bit_lib libXrandr.so.2
need "32-bit FreeType (libfreetype.so.6)" lib32-freetype2 libfreetype6:i386 freetype.i686 -- has_32bit_lib libfreetype.so.6
need "32-bit padsp for game audio (libpulsedsp.so)" lib32-libpulse pulseaudio-utils:i386 pulseaudio-utils.i686 -- find_pulse_dsp
need "C compiler (gcc)" gcc gcc gcc -- command -v gcc
if command -v gcc >/dev/null 2>&1; then
    need "32-bit build support (gcc -m32)" lib32-gcc-libs gcc-multilib "glibc-devel.i686 libgcc.i686" -- compiles_m32
    need "32-bit GMP for the RSA helper (-lgmp)" lib32-gmp libgmp-dev:i386 gmp-devel.i686 -- compiles_m32 -lgmp
fi
need "xdotool (window placement)" xdotool xdotool xdotool -- command -v xdotool
need "xrandr (monitor selection)" xorg-xrandr x11-xserver-utils xrandr -- command -v xrandr

if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "Missing:" >&2
    printf '  - %s\n' "${MISSING[@]}" >&2
    echo "Install them with:" >&2
    install_hint "${MISSING_ARCH[*]}" "${MISSING_DEBIAN[*]}" "${MISSING_FEDORA[*]}"
    if [[ "$(distro_family)" == "arch" ]]; then
        echo "(The lib32-* packages need the [multilib] repository enabled in /etc/pacman.conf.)" >&2
    fi
else
    echo "All game runtime requirements are installed."
fi

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
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "WARNING: install the missing packages listed above before launching the game." >&2
fi
