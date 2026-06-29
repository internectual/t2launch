#!/bin/bash
./tribes2.dynamic "$@"
cp console.log /home/loki/.loki/tribes2/console.log 2>/dev/null || true
