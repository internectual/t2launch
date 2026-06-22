#!/usr/bin/env bash
# bundle-libs.sh — Copy required compatibility libraries from a Loki game
# installation into the Docker build context.
#
# Usage: ./bundle-libs.sh [path/to/game/lib]
#   If no path is given, you'll be prompted to enter one interactively.
#   You can point it at either:
#     - The game's lib/ subfolder (e.g. /path/to/game/lib)
#     - The game root folder (e.g. /path/to/game) — it'll search recursively
#
# This populates the lib/ directory with the old-era shared libraries
# (libSDL, libsmpeg, libstdc++, etc.) that the Docker image needs at
# build time to run GCC 2.95 / egcs-compiled game binaries.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$SCRIPT_DIR/lib"
SRC_DIR="${1:-}"

# --- Interactive library-folder selection ------------------------------------

# True if the directory contains the old-era compat libraries we need.
dir_has_libs() {
    ls "$1"/libstdc++* "$1"/libSDL* "$1"/libsmpeg* &>/dev/null
}

# Print any auto-detected folders that hold the compat libraries, one per line.
# Points at the game's lib/ subfolder when the libs live there.
detect_lib_candidates() {
    local guess path
    local -A seen=()
    for guess in \
        "$HOME/tribes2/asgard/lib" \
        "$HOME/tribes2/asgard" \
        "$HOME/t2-linux" \
        "$HOME/Downloads/t2-linux" \
        "$HOME/Tribes2/Linux" \
        "$HOME/tribes2" \
        "$HOME/Games/Tribes2" \
        "/media/cdrom/Tribes2/Linux" \
        "/mnt/cdrom/Tribes2/Linux" \
        "/opt/tribes2" \
        "/usr/local/games/tribes2"; do
        [[ -d "$guess" ]] || continue
        path=""
        if dir_has_libs "$guess"; then
            path="$guess"
        elif dir_has_libs "$guess/lib"; then
            path="$guess/lib"
        fi
        [[ -n "$path" ]] || continue
        path="$(cd "$path" && pwd)"          # normalize to absolute
        [[ -n "${seen[$path]:-}" ]] && continue
        seen[$path]=1
        printf '%s\n' "$path"
    done
}

# Arrow-key folder browser (whiptail). Prints the chosen directory to stdout.
#   [up/down]            highlight a sub-folder
#   Enter / "Open"       go inside the highlighted folder
#   Tab -> "Choose ..."  select the folder you are currently in
#   Esc                  cancel
browse_for_folder() {
    local cur
    cur="$(cd "${1:-$HOME}" 2>/dev/null && pwd)" || cur="$HOME"
    while true; do
        local menu=() d note=""
        [[ "$cur" != "/" ]] && menu+=(".." ".. (go up a level)")
        while IFS= read -r d; do
            [[ -n "$d" ]] && menu+=("$d" "[folder] $d")
        done < <(find "$cur" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | LC_ALL=C sort -f)
        [[ ${#menu[@]} -eq 0 ]] && menu+=(" " "(no sub-folders here)")
        if dir_has_libs "$cur"; then
            note="\n\n  *** Compatibility libraries detected in THIS folder! ***"
        fi

        local choice rc
        if choice=$(whiptail --title "Browse for your Tribes 2 library folder" \
                --menu "Now in:  $cur$note\n\n[up/down] highlight a folder, then [Open] to go inside it.\n[Tab] over to [Choose THIS folder] to pick where you are." \
                22 78 12 "${menu[@]}" \
                --ok-button "Open" --cancel-button "Choose THIS folder" \
                3>&1 1>&2 2>&3); then
            rc=0
        else
            rc=$?
        fi

        if [[ $rc -eq 0 ]]; then            # "Open" the highlighted folder
            case "$choice" in
                " ")  : ;;
                "..") cur="$(cd "$cur/.." && pwd)" ;;
                *)    cur="$(cd "$cur/$choice" 2>/dev/null && pwd)" || true ;;
            esac
        elif [[ $rc -eq 1 ]]; then          # "Choose THIS folder"
            printf '%s\n' "$cur"
            return 0
        else                                # Esc -> cancel
            return 1
        fi
    done
}

# Top-level picker: list detected lib folders + a "Browse..." option.
# Prints the chosen path. Returns non-zero if the user quits.
select_lib_folder() {
    local cands=() c menu=()
    while IFS= read -r c; do [[ -n "$c" ]] && cands+=("$c"); done < <(detect_lib_candidates)
    for c in "${cands[@]}"; do menu+=("$c" "use this: $c"); done
    menu+=("__browse__" "Browse for a folder...")

    local sel
    sel=$(whiptail --title "Tribes 2 - Bundle Compatibility Libraries" \
        --menu "Where are your Tribes 2 compat libraries?\n(the game's lib/ folder, or the game root)\n\nUse the arrow keys, then Enter." \
        20 78 10 "${menu[@]}" \
        --ok-button "Select" --cancel-button "Quit" \
        3>&1 1>&2 2>&3) || return 1

    case "$sel" in
        __browse__) browse_for_folder "${cands[0]:-$HOME}" ;;
        *)          printf '%s\n' "$sel" ;;
    esac
}

# If no argument provided, ask — graphical arrow-key menu when possible.
if [[ -z "$SRC_DIR" ]]; then
    echo "=== Tribes 2 Docker — Bundle Compatibility Libraries ==="
    echo ""

    if command -v whiptail >/dev/null 2>&1 && [[ -t 0 && -t 1 ]]; then
        if ! SRC_DIR="$(select_lib_folder)"; then
            echo "No folder selected. Aborting."
            exit 1
        fi
    else
        # Plain-text fallback (no whiptail, or non-interactive terminal)
        echo "Enter the path to your Tribes 2 game installation."
        echo "You can point at the game root or its lib/ subfolder."
        echo "The script will search for files like libSDL-1.2.so.0,"
        echo "libstdc++-libc6.2-2.so.3, libsmpeg-0.4.so.0, etc."
        echo ""
        mapfile -t CANDIDATES < <(detect_lib_candidates)
        if [[ ${#CANDIDATES[@]} -gt 0 ]]; then
            echo "Found possible game lib directories:"
            for i in "${!CANDIDATES[@]}"; do
                echo "  $((i+1)). ${CANDIDATES[$i]}"
            done
            echo ""
            read -rp "Enter a path directly, or pick a number [1-${#CANDIDATES[@]}] (default: 1): " INPUT
            INPUT="${INPUT:-1}"

            if [[ "$INPUT" =~ ^[0-9]+$ ]] && [[ "$INPUT" -ge 1 ]] && [[ "$INPUT" -le ${#CANDIDATES[@]} ]]; then
                SRC_DIR="${CANDIDATES[$((INPUT-1))]}"
            else
                SRC_DIR="$INPUT"
            fi
        else
            read -rp "Enter path: " SRC_DIR
        fi
    fi

    # Expand ~ and validate
    SRC_DIR="${SRC_DIR/#\~/$HOME}"
    echo ""
fi

if [[ ! -d "$SRC_DIR" ]]; then
    echo "ERROR: '$SRC_DIR' is not a directory."
    exit 2
fi

# If the user pointed at a game root (has tribes2.dynamic but no libstdc++),
# check if there's a lib/ subdir or search one level deeper
if ! ls "$SRC_DIR"/libstdc++* "$SRC_DIR"/libSDL* "$SRC_DIR"/libsmpeg* &>/dev/null; then
    if ls "$SRC_DIR"/lib/libstdc++* "$SRC_DIR"/lib/libSDL* "$SRC_DIR"/lib/libsmpeg* &>/dev/null; then
        SRC_DIR="$SRC_DIR/lib"
        echo "Using lib/ subdirectory: $SRC_DIR"
    else
        echo "WARNING: No compat library files found in '$SRC_DIR'."
        echo "Expected files like: libstdc++-libc6.2-2.so.3, libSDL-1.2.so.0, libsmpeg-0.4.so.0"
        echo "The Docker image includes fallback packages (libsmpeg0, libsdl1.2debian)"
        echo "so the game may still work without these."
        read -rp "Continue anyway? [y/N]: " CONFIRM
        if [[ ! "$CONFIRM" =~ ^[Yy] ]]; then
            echo "Aborted."
            exit 1
        fi
    fi
fi

# Clean out old binaries (keep .gitkeep and .gitignore)
find "$LIB_DIR" -maxdepth 1 -type f ! -name '.gitkeep' ! -name '.gitignore' -delete 2>/dev/null || true

COPIED=0
for pattern in libstdc++* libSDL* libsmpeg* libsmjpeg* libttf*; do
    for f in "$SRC_DIR"/$pattern; do
        if [[ -f "$f" ]]; then
            cp -L "$f" "$LIB_DIR/"
            echo "  copied: $(basename "$f")"
            COPIED=$((COPIED + 1))
        fi
    done
done

echo ""
if [[ $COPIED -eq 0 ]]; then
    echo "WARNING: No matching library files copied."
    echo "The Docker image's built-in packages may be sufficient."
else
    echo "Done. $COPIED files prepared in $LIB_DIR/"
fi
echo "Run './asgard-build <game>' to build the Docker image with these libraries."
