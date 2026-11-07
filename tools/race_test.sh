#!/bin/sh
# Quick Race test: boots with the save data in dbg/menu/ps3_data (a profile already exists) straight to
# Press Start (F1CE_SKIP_INTRO=1, English; the input times assume it), presses
# cross every 4 s through Single Player -> Quick Race -> Silverstone, and exits 10 s after the race
# scene appears (F1CE_QUIT_AFTER_3D). The last frame and the 3D target are saved as frame_s*_quit.
# usage: tools/race_test.sh <run dir name> [VAR=value ...]
cd "$(dirname "$0")/.." || exit 1
GAME="C:/Users/user/Desktop/Repos/F1_CE_PS3/F1_CE_PS3_og/PS3_GAME"
name="$1"; shift
d="dbg/$name"; rm -rf "$d"; mkdir -p "$d"; cp -r dbg/menu/ps3_data "$d/"
IN="9:start"
# DRIVE=1: menus by 41 s, then cross (accelerate) held for the whole race instead of a press every 4 s
if [ -n "$DRIVE" ]; then
    for t in $(seq 13 4 41); do IN="$IN,$t:cross"; done
    IN="$IN,47:cross:900"   # throttle held for the race
else
    for t in $(seq 13 4 137); do IN="$IN,$t:cross"; done
fi
cd "$d" && env F1CE_SKIP_INTRO=1 F1CE_LANGUAGE=english "$@" F1CE_QUIT_AFTER_3D="${QUIT_AFTER:-10}" F1CE_INPUT="$IN" timeout ${TIMEOUT:-400} ${EXE:-../../build-win/f1ce.exe} "$GAME" > log.txt 2>&1
rc=$?; cp f1ce_recomp.log log.txt 2>/dev/null   # (the console only gets warnings/errors)
echo "exit $rc"
python -I ../../tools/frames_to_png.py . > /dev/null
grep -E "3D scene detected|QUIT_AFTER_3D reached" log.txt | cut -c16-
ls frame_*quit*.png 2>/dev/null
