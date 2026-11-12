# HUD bar demo artwork

These are the sample images from the supplied `hudbot.0.2b` pack: the default
horizontal set at the top level, plus Tani's original horizontal and vertical
sample configurations in `hudbot_horizontal/` and `hudbot_vertical/`. Both
sample sets contain health/energy bar and frame art. The T2 `HudDamage` and
`HudEnergy` controls continue to provide the live values and fills.

The standalone demo archive `scripts/HudDemo/HudDemo.vl2` packages these samples
with the horizontal compass and `hudBarDemo.cs`. Install that `.vl2` into the
game's `base/` folder. To run from loose files instead, copy this directory to
`base/hudBarDemo_assets/` and the script to `base/scripts/autoexec/`. Set the
profile's `render.sky_rotation_speed` option to `0` to stop sky rotation; use
`hudBarDemoSetCompassMode("circular"|"horizontal"|"both")` to choose compass
presentation.

Credits: PenguinOfDoom, Andrew (HUDbot 0.2b), and NoFix.
