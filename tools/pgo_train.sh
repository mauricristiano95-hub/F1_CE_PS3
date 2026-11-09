#!/bin/sh
# PGO training: runs the instrumented build (cmake -DF1CE_PGO=GEN, default build-pgo-gen) through scripted quick
# races (menus, grid, driving; Vulkan and OpenGL, dry and rain, 30 fps mode) and merges the raw profiles into
# pgo/f1ce.profdata for a -DF1CE_PGO=USE build. Runs that do not reach their quit time write no profile and are
# reported. usage: tools/pgo_train.sh [gen build dir]
cd "$(dirname "$0")/.." || exit 1
R=$(pwd)
B="${1:-build-pgo-gen}"
LLVM="${LLVM_MINGW:-C:/Tools/llvm-mingw-20261006-ucrt-x86_64}"
[ -f "$B/f1ce.exe" ] || { echo "no $B/f1ce.exe"; exit 1; }
rm -rf pgo/raw; mkdir -p pgo/raw
# programs and pipelines come from the normal build's cache (the training exe has no cache of its own)
export F1CE_CACHE_DIR="$R/build-win/shader_cache"
run() {   # <name> <seconds after the race scene> [VAR=value ...]
    name="$1"; secs="$2"; shift 2
    LLVM_PROFILE_FILE="$R/pgo/raw/$name.profraw" DRIVE=1 QUIT_AFTER="$secs" TIMEOUT=400 EXE="$R/$B/f1ce.exe" \
        sh tools/race_test.sh "pgo_$name" "$@" > /dev/null 2>&1
    L="dbg/pgo_$name/log.txt"
    echo "$name: race scene $(grep -c '3D scene detected' "$L"), quit $(grep -c 'QUIT_AFTER_3D reached' "$L")," \
         "profile $(ls "pgo/raw/$name.profraw" 2>/dev/null || echo MISSING), fps $(awk '/3D scene detected/{f=1}
         f && /perf: [0-9]/{for(k=1;k<=NF;k++) if($k=="fps") {s+=$(k-1); n++}} END{if(n) printf "%.1f", s/n}' "$L")"
}
run silverstone 60 F1CE_RENDERER=vulkan F1CE_SCALE=1.5
run monaco      45 F1CE_RENDERER=vulkan F1CE_SCALE=1.5 F1CE_QR_TRACK=monaco
run suzuka      45 F1CE_RENDERER=vulkan F1CE_SCALE=1.5 F1CE_QR_TRACK=suzuka
run melbourne   45 F1CE_RENDERER=vulkan F1CE_SCALE=1.5 F1CE_QR_TRACK=melbourne
run rain        45 F1CE_RENDERER=vulkan F1CE_SCALE=1.5 F1CE_QR_TRACK=spain F1CE_QR_WEATHER=heavyrain
run opengl      30 F1CE_RENDERER=opengl F1CE_QR_TRACK=monza
"$LLVM/bin/llvm-profdata" merge -o pgo/f1ce.profdata pgo/raw/*.profraw && ls -l pgo/f1ce.profdata
