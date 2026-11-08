# Tribes 2 HUD demo pack

`HudDemo.vl2` contains `tests/hudBarDemo.cs`, both Tani/HUDbot horizontal and
vertical health/energy artwork sets, and the original beta compass strip/frame
assets. Cardinal markers use `gui/hud_north`, `gui/hud_south`, `gui/hud_east`,
and `gui/hud_west` from the game's existing `textures.vl2`; they are not copied
into the demo archive. The script's compass mode can be changed in-game:

```ts
hudBarDemoSetCompassMode("circular");   // stock round compass only
hudBarDemoSetCompassMode("horizontal"); // beta horizon markers replace the round one
hudBarDemoSetCompassMode("both");       // supplement the round one (default)
```

The horizontal compass uses the beta `hud_north`, `hud_south`, `hud_east`, and
`hud_west` marker images. It projects the cardinal markers along the screen
horizon using the controlled object's native eye heading, including observer
cameras. The demo's hook RadarHUD renders the active
TerrainBlock height/material map, rotates it with the camera view, and shows
teammates, the assigned waypoint, and opponents only from T2's visible
ShapeBase set. It runs for Player and observer Camera control objects during
play and demo playback, and is gated to an awake/visible PlayGui/NavHud. Sky
rotation remains opt-in through `$pref::skyRotationSpeed`. `DOOMHud.vl2`
remains a separate optional package.

The bar-example cluster anchors to the bottom-right of the safe HUD area at
each resolution, leaving a 176-pixel right-side reserve for the stock weapon
and inventory HUDs. The HUDbot horizontal and vertical samples move with that
cluster; the radar/compass status remains at the upper-left.

Install `HudDemo.vl2` into the game's `base/` directory. To rebuild after
editing the demo or its assets, run `python3 scripts/HudDemo/build_vl2.py`.

## Credits

Thanks to **PenguinOfDoom**, **Andrew** (HUDbot 0.2b and Tani sample layouts),
and **NoFix** for the supplied HUD, compass, RadarHUD, and sky-rotation reference
work. DOOMHud credits are maintained in `scripts/DOOMHud/README.md`.
