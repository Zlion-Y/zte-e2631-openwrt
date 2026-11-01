#!/bin/bash
awk "/static int mt7915_load_patch/,/^}/" /tmp/mt76x/src/mt7915/mcu.c | head -75

awk "/static int mt7915_load_patch/,/^}/" /tmp/mt76x/src/mt7915/mcu.c | head -75
