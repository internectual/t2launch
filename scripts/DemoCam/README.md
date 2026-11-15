# DemoCam for Tribes 2

Switch which player you watch while a demo plays. This is Lou Cypher's DemoCam
(version 0.2, January 2014) without its `memPatch()` calls: the changes those
made to the Windows 25034 executable are made natively by the t2launch hook on
Linux.

## Install

1. Turn on **Switch players in demos** (`review.demo_camera`) in the launcher's
   profile options (or put `review.demo_camera=1` in the hook config). The hook
   logs `Demo camera` when the patch is in.
2. Copy `DemoCam.vl2` into the game's `base/` directory. Its `#autoload` script
   binds the keys when a demo starts.

## Keys (while a demo plays)

| Key | Action |
|---|---|
| Down | Lock on / off to the chosen player (shows `lock: OFF` when released) |
| Right | Next player in the player list (and lock) |
| Left | Previous player (and lock) |

The player's name appears as a bottom print. The bindings are removed again
when the demo ends.

## What the option changes

Build 25034 hides every script object whose flag bit `0x40` is set from the
script engine. DemoCam needs the demo's player objects, so `review.demo_camera`
removes that check in three places (the same three spots the original memPatch
lines changed on Windows): the object-dump loop, `SimSet.getObject()`, and the
method-call lookup. The option is applied when the client starts, so those
objects are also visible to scripts in live games while it is on; leave it off
when you do not use DemoCam.

## Rebuild the archive

```sh
python3 scripts/DemoCam/build_vl2.py
```

## Provenance

`scripts/autoexec/democam.cs` is the original script by Lou Cypher (LouCypher)
with the three `memPatch()` lines replaced by a comment and the version/date
header updated. The script logic is unchanged.
