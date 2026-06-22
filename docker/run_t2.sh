#!/bin/bash
# Runs the self-contained "tribes2" build (SDL/smpeg/FreeType statically linked
# in), rather than tribes2.dynamic. No flags are forced — pass whatever you want
# via asgard-run, e.g.
#   ./asgard-run tribes2 -nologin -mod Classic
# (Use -nologin for offline play; without it the game tries the defunct
#  WON/Sierra login servers.)
./tribes2 "$@"
cp console.log /home/loki/.loki/tribes2/console.log 2>/dev/null || true
