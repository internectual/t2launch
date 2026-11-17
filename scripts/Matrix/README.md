# Matrix: a map with no terrain and a living sky

`Matrix` is a Deathmatch map on a single platform in the sky: no `TerrainBlock`, just a `MissionArea`, a `Sky`, a
`Sun`, a stock Phoenix platform (`pplat4.dif`, already in every client) and spawn points. Its dark green skybox is
the *fallback*; with the t2launch hook the six sky faces can host **views**: animations today, video, a terminal and
the game console in later phases.

## Install

Copy `zMatrix.vl2` into the game's `base/` directory (rebuild it with `python3 scripts/Matrix/build_vl2.py`), then
host **Matrix** (Deathmatch) from the LAN profile, or from the command line:
`T2LAUNCH_AUTOHOST_MAP=Matrix T2LAUNCH_AUTOHOST_TYPE=DM`. On a stock client the map works as a plain static sky.

With the hook, loading the map turns on `t2launch_skyFullBox(1)` (the engine normally clips the lower half of the
four sky walls at the camera's height because terrain hides it) and adds a few default views; leaving the map turns
both off again.

## The in-game panel

**Ctrl+End** opens and closes the Sky Views panel: pick a view (or ADD one with the face and source dropdowns and the
file/URL/`#color` field; face `all` with `video360` covers the whole sky), UPDATE or REMOVE it, drag the X / Y /
WIDTH / HEIGHT / OPACITY sliders to move it live, toggle VISIBLE, save or load a layout by name, CLEAR everything, or
switch the full sky box. Everything it does is also a console command (below).

**BROWSE** (next to the source field, for the `video`, `image` and `video360` sources) opens the game's own file
dialog on the media folder: put clips and pictures in the game's `base/media` folder and pick one from the list
(it filters by file type; the field then holds `media/<file>`, which the hook resolves under `base/`). For files
anywhere else, type the full path or URL into the field.

### Terminal and console views

| Spec | |
|---|---|
| `terminal` | a real shell (`$SHELL`) on a pseudo-terminal, drawn as text on the face |
| `terminal:<profile>` | the command named `<profile>` in `~/.config/t2launch/sky_terminals.cfg` (`name=command` lines, e.g. `monitor=htop`) |
| `console` | the Tribes 2 console, live (read from `~/.loki/tribes2/console.log`, which the engine writes while logging is on; the view switches logging on, natively, and off again when it goes) |

Terminals are interactive: **Ctrl+PageUp** (or the panel's TYPE button) gives the keyboard to the first terminal. Every
key then goes to the shell and not to the game, and a cyan frame shows the focus; Ctrl+PageUp gives the keyboard back.
The text is drawn by `sky_terminal.py` (PySide6 offscreen, so it needs the launcher's Python, which the launcher passes
as `T2LAUNCH_SKY_PYTHON`) at `sky.terminal_width` x `sky.terminal_height` pixels (1024x768), so give the view a rectangle
with the same aspect, for example `w=1 h=0.75`. Profiles come only from that config file, never from the game or a
map's scripts: a spec can name a profile but cannot make the hook run a command of its own.

## Sky view commands (hook consoles, client)

| Command | |
|---|---|
| `t2launch_skyViewAdd(face, spec[, x, y, w, h, opacity])` | add a view, returns its id |
| `t2launch_skyViewSet(id, field, value)` | `face`, `spec`, `x`, `y`, `w`, `h`, `opacity`, `blend` (alpha/add), `z`, `enabled` |
| `t2launch_skyViewRemove(id)` / `t2launch_skyViewClear()` | remove one / all views |
| `t2launch_skyViewList()` | one view per line |
| `t2launch_skyLayoutSave(name)` / `t2launch_skyLayoutLoad(name)` | `~/.local/share/t2launch/skyviews/<name>.cfg` |
| `t2launch_skyFullBox(on)` | draw the whole sky box |
| `t2launch_skyKeyCapture(id)` | give the keyboard to a terminal view (0 releases) |
| `t2launch_skyViews()` / `t2launch_skyLayouts()` / `t2launch_skyStatus()` | tab/line records the panel reads |

`face` is `fr`, `rt`, `bk`, `lf`, `up`, `dn` (the order of the DML), `0`-`5`, or `front`/`right`/`back`/`left`/`top`/
`bottom`. A view's rectangle is in displayed coordinates: `0,0` is the top left of the face as you see it from inside
(on the top and bottom faces north is up). Sources today: `builtin:plasma`, `builtin:stars`, `builtin:aurora`,
`builtin:gradient[:#rrggbb]`, `builtin:solid:#rrggbb` and `builtin:calibrate` (a labelled test pattern), plus media
through ffmpeg (see below). A face with no enabled view shows its original graphics.

### Video, images and 360 video

| Spec | |
|---|---|
| `video:<file or URL>` | any video ffmpeg reads (file, http(s), rtsp), scaled to fit the view's rectangle |
| `image:<file>` | a picture or animated GIF/APNG |
| `video360:<file or URL>[@layout]` | a 360 video on the whole sky: use face `all`, which makes six views sharing one decoder. Layouts: equirectangular (default), `c3x2`, `c6x1`, `c1x6`, `c2x3`, `eac`, `e-sbs`, `e-tb` (stereo) |

Example: `t2launch_skyViewAdd("all", "video360:/home/me/tour.mp4");` or
`t2launch_skyViewAdd("fr", "video:/home/me/clip.mp4", 0.1, 0.1, 0.8, 0.8, 1);`. The centre of a 360 video is north
(the BK wall); east is to the right. Each distinct source is one 64-bit `ffmpeg` process (the game is 32-bit), killed
when its last view is removed, when it fails or ends (the faces then show the regular sky again) and when the game
exits. Sound comes from a second `ffmpeg` to PulseAudio/PipeWire (`Tribes 2 sky`, honours the audio output device;
approximately in sync). Paths are used as given, so no `file:`/`concat:`/pipe tricks: only plain paths and
http/https/rtsp URLs. Options: `sky.media`, `sky.ffmpeg`, `sky.stream_audio`, `sky.stream_volume`, `sky.stream_loop`,
`sky.stream_fps`, `sky.video_width`, `sky.sphere_width`. Spaces in a path are saved in layouts as `%20`.

Geometry (from the engine): FR = south wall, RT = east, BK = north, LF = west, UP = top, DN = bottom.

If your profile uses a datablock rule that replaces every sky (such as the grid skybox example), that rule also
replaces Matrix's own faces; the views then draw over the replaced faces.
