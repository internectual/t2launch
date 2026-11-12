# Grid sky override handoff

## User-visible result

The user still sees the server's default skybox, not the grid, after enabling the
`render.datablock_rules` example in the Online client profile. The intended
behavior is a client-local replacement of the mission Sky's DML with
`grid.dml`, leaving the server and gameplay state untouched.

## Current configuration and assets

- Client profiles `LAN`, `Online`, and `Shazbot CRC Probe` have
  `render.sky_rotation_speed=0.0` and
  `render.datablock_rules=/home/methodown/t2launch/runtime/render-modifiers.datablocks.example.cfg`.
- The datablock rules file has active `client connect` and `client frame` rules
  for `class:Sky`, `Sky`, and `MissionSky`, assigning `materialList = "grid.dml"`.
  Katabatic's mission source uses `new Sky(Sky)` with its server-selected DML in
  `Sky.materialList`.
- `zGridSkyBox.vl2` is at the repo root and installed at
  `/home/methodown/t2-linux/base/zGridSkyBox.vl2`. Both copies have SHA-256
  `2af79ae152856b6b744826f2a6a648ec02f13558d16badc1f4dc6959c402dc0a`.
- The VL2 contains only six `textures/koko/skies/Grid_*.png` faces and
  `textures/grid.dml`. The DML references those six faces plus the stock
  `lush/skies/lush_day_emap` from `lush.vl2`. Mission `Sky.materialList` values
  use the DML basename (`grid.dml`), not `textures/grid.dml`.

## Evidence

- The latest captured hook log before the renderer-specific patch reported:
  ```text
  loaded 6 render modifier rules from .../render-modifiers.datablocks.example.cfg
  render modifier connect matches=0 direct=0 objects=46 selectors=0 fields=0
  ```
- The user reports the default sky is still visible. There is no captured client
  log yet from a launch using the latest renderer-specific patch, so its setter
  result and match count have not been verified live.
- Earlier `render modifier connect matches=0` entries are from the generic
  SimGroup scan before the renderer-specific Sky hook was added. The current
  renderer-specific hook directly targets the Sky object received by
  `Sky::renderSkyBox`.
- The unresolved question is whether assigning the `materialList` field after
  Sky creation reloads/replaces the native cached `MaterialList` used by the
  renderer, or whether that cached material list needs an explicit refresh/reload
  path. Also verify that `grid.dml` and its cube-face images resolve through the
  resource manager from a live client render.
- The client profiles' saved hook configs contain the render-rule path and
  rotation speed `0.0`; the app was restarted to refresh its option schema.
  `Online.conf` and `LAN.conf` were regenerated with those values.
- No client is running at handoff; the user's dedicated server remains active.

## Relevant implementation

- Profile option schema and hook value: `t2options.py` and `t2_hook.c`
  (`render.sky_rotation_speed`, `render.datablock_rules`).
- Rule parser and general object traversal: `t2_hook.c`, the
  `render_modifier_*` functions around the “Configurable render modifiers”
  section.
- Renderer-specific material assignment: `render_apply_sky_material_modifiers()`
  called from `sky_render_slices_wrapper()`. It gets the live Sky object's ID,
  class, and name, matches `materialList` rules, then calls the TorqueScript
  helper `t2launch_applySkyMaterialList(id, value)`.
- On a successful TorqueScript setter result, the new hook logs
  `Sky materialList override applied: name=... class=... id=... connect=... frame=...`.
- Existing tests: `tests/render_modifier_test.c`,
  `tests/test_game_binaries.py`, `tests/test_grid_skybox_pack.py`, and
  `tests/test_options.py`.

## Next diagnostic steps

1. In a client actually in a mission, log the Sky object's current
   `materialList` immediately before/after the direct assignment and confirm the
   resource manager can open `grid.dml` and each `Grid_*` face.
2. Inspect the Sky class's native `onStaticModified`/material-list reload path
   and use the supported refresh method if the field assignment does not replace
   the cached render materials.
3. Capture a screenshot/log from the same mission after reconnecting with the
   current hook; the last visible client result was not visually successful.
4. Keep any live test client local and temporary. A command-line `-connect`
   attempt did not reach the local server. An earlier Online/autolaunch smoke
   initiated a T2CSRI challenge to a non-local server; that client was stopped.
   Do not use `-online` as the connection mechanism for the isolated test.

## Repository state

At handoff, `main` is clean and synced with `origin/main`. Recent commits:

- `8c8e2fa Apply Sky material rules at render time`
- `0e5aca8 Assign literal Sky fields directly`
- `e4fd172 Apply literal render modifiers through TorqueScript`
- `e0f59b5 Scan mission group for render modifiers`
- `284f386 Add environment map to grid sky DML`
- `fe36187 Target named Sky mission objects directly`
- `a150ca7 Apply grid sky rule every frame`
- `e098f2e Retry Sky modifiers as missions load`

The latest hook build and 30 relevant tests passed. The user settings file is
outside the repository at `~/.config/t2launch/settings.json` (mode `0600`).
