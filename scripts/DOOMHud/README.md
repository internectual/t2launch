# DOOMHud for Tribes 2

This standalone `.vl2` repackages the supplied `DOOMHud` assets and updates the
2003 TorqueScript HUD for the current T2 hook. The archive contains the GUI,
autoexec script, DOOM status-bar textures, and all `doom_*.gft` font sizes.
The status bar uses GUI `center`/`top` sizing anchors on the 640×480 base
canvas. In T2, `vertSizing = "top"` pins the bottom edge, so the bar stays
bottom-centered across viewport size changes without HudMover.

## Install and test

Copy `DOOMHud.vl2` into the game's `base/` directory. Its `#autoload` script
adds the centered bottom status bar automatically. For the combined t2launch
test, also copy `tests/hudBarDemo.cs` to `base/scripts/autoexec/` and the
`tests/hudBarDemo_assets/` directory to `base/hudBarDemo_assets/`.

Health and energy digits read the T2 hook's `.value` fields on live
`HudDamage`/`HudEnergy` controls; no `t2health.exe`, `support.vl2`, `loadout.cs`,
or legacy xHealth/xEnergy globals are required. Weapon/ammo icons follow the
normal client HUD messages. Optional keycard indicators are exposed through
`$DOOMHud::BlueKey`, `$DOOMHud::YellowKey`, and `$DOOMHud::RedKey`.

## Rebuild the archive

Edit the files under this directory, then run:

```sh
python3 scripts/DOOMHud/build_vl2.py
```

The build script writes a sorted, reproducible `DOOMHud.vl2` containing only
`fonts/`, `gui/`, `scripts/`, and `textures/` entries.

## Provenance

The art and fonts came from the user-supplied `Dynamix/scripts/DOOMHud` package.
The original script credits Canadian-, PenguinOfDoom, Silverspirit, and
UberGuy; those credits remain in the packaged script metadata. The original
package's `READ1ST.TXT` installation instructions are superseded because they
required a separate T1/T2 memory-patching executable and support archive.
