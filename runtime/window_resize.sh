#!/usr/bin/env bash
# Follow the selected Tribes 2 video mode; SDL 1.2 can change its render surface
# without resizing its X11 window, and its fullscreen window can be smaller than
# the monitor when the selected mode is lower than the display's native mode.
set -u

PREFS=${1:?ClientPrefs.cs path required}
GAME_PID=${2:?game pid required}
MONITOR_NAME=${3:-}
FORCE_FULLSCREEN=${4:-0}
FORCE_WINDOWED=${5:-0}

fullscreen_monitor() {
    local monitors line flags geometry name
    monitors=$(xrandr --listmonitors 2>/dev/null) || return 1
    while IFS= read -r line; do
        [[ "$line" == Monitors:* ]] && continue
        read -r _ flags geometry name <<<"$line"
        if [[ -n "$MONITOR_NAME" ]]; then
            [[ "$name" == "$MONITOR_NAME" ]] || continue
        else
            [[ "$flags" == *"*"* ]] || continue
        fi
        if [[ "$geometry" =~ ^([0-9]+)/[0-9]+x([0-9]+)/[0-9]+([+-][0-9]+)([+-][0-9]+)$ ]]; then
            printf '%s %s %s %s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" \
                "${BASH_REMATCH[3]}" "${BASH_REMATCH[4]}"
            return 0
        fi
    done <<<"$monitors"
    return 1
}

while kill -0 "$GAME_PID" 2>/dev/null; do
    width=""
    height=""
    fullscreen="1"
    if [[ -r "$PREFS" ]]; then
        while IFS= read -r line; do
            # shellcheck disable=SC2016 # These match literal TorqueScript pref names.
            case "$line" in
                '$pref::Video::resolution = '* )
                    value=${line#*\"}
                    value=${value%%\"*}
                    read -r width height _depth <<<"$value"
                    ;;
                '$pref::Video::fullScreen = '* )
                    value=${line#*=}
                    value=${value//[\"\;[:space:]]/}
                    fullscreen=$value
                    ;;
            esac
        done < "$PREFS"
    fi

    if [[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ &&
          "$width" -ge 320 && "$width" -le 8192 && "$height" -ge 200 && "$height" -le 8192 ]]; then
        for window in $(xdotool search --onlyvisible --name '^Tribes 2$' 2>/dev/null); do
            geometry=$(xdotool getwindowgeometry --shell "$window" 2>/dev/null) || continue
            current_x=""
            current_y=""
            current_width=""
            current_height=""
            while IFS= read -r line; do
                case "$line" in
                    X=*) current_x=${line#*=} ;;
                    Y=*) current_y=${line#*=} ;;
                    WIDTH=*) current_width=${line#*=} ;;
                    HEIGHT=*) current_height=${line#*=} ;;
                esac
            done <<<"$geometry"

            if [[ "$FORCE_FULLSCREEN" == "1" ||
                  ( "$FORCE_WINDOWED" != "1" && "$fullscreen" == "1" ) ]]; then
                read -r monitor_width monitor_height monitor_x monitor_y < <(fullscreen_monitor) || continue
                # Scale the game mode into the selected output without changing
                # its aspect ratio, centered inside SDL's fullscreen parent.
                if (( width * monitor_height > height * monitor_width )); then
                    target_width=$monitor_width
                    target_height=$((monitor_width * height / width))
                else
                    target_height=$monitor_height
                    target_width=$((monitor_height * width / height))
                fi
                target_offset_x=$(((monitor_width - target_width) / 2))
                target_offset_y=$(((monitor_height - target_height) / 2))
                target_x=$((monitor_x + target_offset_x))
                target_y=$((monitor_y + target_offset_y))
                if [[ "$current_width" != "$target_width" || "$current_height" != "$target_height" ||
                      "$current_x" != "$target_x" || "$current_y" != "$target_y" ]]; then
                    # SDL's fullscreen GL child is parented in its Xinerama
                    # window, so use centered offsets relative to that parent.
                    xdotool windowmove --sync "$window" "$target_offset_x" "$target_offset_y" 2>/dev/null || true
                    xdotool windowsize --sync "$window" "$target_width" "$target_height" 2>/dev/null || true
                fi
            elif [[ ( "$FORCE_WINDOWED" == "1" || "$fullscreen" == "0" ) &&
                    "$FORCE_FULLSCREEN" != "1" &&
                    ( "$current_width" != "$width" || "$current_height" != "$height" ) ]]; then
                xdotool windowsize --sync "$window" "$width" "$height" 2>/dev/null || true
            fi
        done
    fi
    sleep 0.25
done
