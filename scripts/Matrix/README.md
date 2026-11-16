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

## Sky view commands (hook consoles, client)

| Command | |
|---|---|
| `t2launch_skyViewAdd(face, spec[, x, y, w, h, opacity])` | add a view, returns its id |
| `t2launch_skyViewSet(id, field, value)` | `face`, `spec`, `x`, `y`, `w`, `h`, `opacity`, `blend` (alpha/add), `z`, `enabled` |
| `t2launch_skyViewRemove(id)` / `t2launch_skyViewClear()` | remove one / all views |
| `t2launch_skyViewList()` | one view per line |
| `t2launch_skyLayoutSave(name)` / `t2launch_skyLayoutLoad(name)` | `~/.local/share/t2launch/skyviews/<name>.cfg` |
| `t2launch_skyFullBox(on)` | draw the whole sky box |

`face` is `fr`, `rt`, `bk`, `lf`, `up`, `dn` (the order of the DML), `0`-`5`, or `front`/`right`/`back`/`left`/`top`/
`bottom`. A view's rectangle is in displayed coordinates: `0,0` is the top left of the face as you see it from inside
(on the top and bottom faces north is up). Sources today: `builtin:plasma`, `builtin:stars`, `builtin:aurora`,
`builtin:gradient[:#rrggbb]`, `builtin:solid:#rrggbb` and `builtin:calibrate` (a labelled test pattern). A face with
no enabled view shows its original graphics.

Geometry (from the engine): FR = south wall, RT = east, BK = north, LF = west, UP = top, DN = bottom.

If your profile uses a datablock rule that replaces every sky (such as the grid skybox example), that rule also
replaces Matrix's own faces; the views then draw over the replaced faces.
