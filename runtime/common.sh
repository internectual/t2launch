# Shared host checks for install.sh and runtime/run-game. Source, don't run.
# shellcheck shell=bash

# True when $1 is a 32-bit ELF file (EI_CLASS 1). Distributions keep 64-bit
# copies under some of the same names, e.g. Arch's /usr/lib/pulseaudio.
is_elf32() {
    [[ -f "$1" && "$(od -An -tx1 -j4 -N1 "$1" 2>/dev/null | tr -d ' ')" == "01" ]]
}

# True when the dynamic linker cache has a 32-bit copy of soname $1.
# ldconfig tags i386 entries "(libc6)" and x86-64 ones "(libc6,x86-64)".
has_32bit_lib() {
    local ldconfig
    ldconfig=$(command -v ldconfig || echo /sbin/ldconfig)
    # grep reads all input (no -q) so ldconfig never dies of SIGPIPE, which
    # callers running with pipefail would see as a missing library.
    "$ldconfig" -p 2>/dev/null | grep -E "^[[:space:]]+${1//./\\.} \(libc6\)" >/dev/null
}

# Print the 32-bit libpulsedsp.so (padsp) under the Arch, Debian/Ubuntu, or
# Fedora 32-bit libdir; fail when there is none.
find_pulse_dsp() {
    local candidate
    for candidate in /usr/lib32/pulseaudio/libpulsedsp.so \
                     /usr/lib/i386-linux-gnu/pulseaudio/libpulsedsp.so \
                     /usr/lib/pulseaudio/libpulsedsp.so; do
        if is_elf32 "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

# Print the distribution family: arch, debian, fedora, or unknown.
distro_family() {
    local family=""
    if [[ -r /etc/os-release ]]; then
        # shellcheck source=/dev/null
        family=$(. /etc/os-release && echo "${ID:-} ${ID_LIKE:-}")
    fi
    case " $family " in
        *" arch "*) echo arch ;;
        *" debian "*|*" ubuntu "*) echo debian ;;
        *" fedora "*|*" rhel "*) echo fedora ;;
        *) echo unknown ;;
    esac
}

# Print an install hint for this distribution family: install_hint ARCH DEBIAN FEDORA
install_hint() {
    case "$(distro_family)" in
        arch) echo "  sudo pacman -S --needed $1" >&2 ;;
        debian)
            echo "  sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install $2" >&2 ;;
        fedora) echo "  sudo dnf install $3" >&2 ;;
        *)
            echo "  Arch:          sudo pacman -S --needed $1" >&2
            echo "  Debian/Ubuntu: sudo apt install $2  (after dpkg --add-architecture i386)" >&2
            echo "  Fedora:        sudo dnf install $3" >&2 ;;
    esac
}
