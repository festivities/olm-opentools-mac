#!/usr/bin/env bash
# One-click macOS build of every OLM plug-in into a single output folder.
#
#   ./build-all.sh                       build all -> ./dist
#   OUT=/path/to/folder ./build-all.sh   choose the output folder
#   AE_SDK=/path/to/SDK/Examples ./build-all.sh
#   ./build-all.sh --clean               wipe each build dir first
#   ./build-all.sh OLMBlur OLMKiraKira   build only the named plug-ins
#
# Requires full Xcode (Rez + codesign) and cmake. Afterwards copy the .plugin
# bundles from OUT into AE's Plug-ins folder, clear AE's plug-in cache, relaunch.
#
# Written for stock macOS bash 3.2: no arrays, no `set -u`.
set -o pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT" || exit 1

CLEAN=0
PLUGIN_ARGS=""
for a in "$@"; do
    case "$a" in
        --clean) CLEAN=1 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) PLUGIN_ARGS="$PLUGIN_ARGS $a" ;;
    esac
done

# Locate the Mac AE SDK Examples dir: $AE_SDK, else the bundled read-only copy.
if [ -z "${AE_SDK:-}" ]; then
    for d in .opencode/AfterEffectsSDK_*_MacOS/*/Examples; do
        if [ -d "$d" ]; then AE_SDK="$d"; break; fi
    done
fi
if [ -z "${AE_SDK:-}" ] || [ ! -d "$AE_SDK" ]; then
    echo "error: AE SDK Examples dir not found. Set AE_SDK=/path/to/SDK/Examples" >&2
    exit 1
fi
AE_SDK="$(cd "$AE_SDK" && pwd)"

OUT="${OUT:-$ROOT/dist}"
ARCHS="${ARCHS:-arm64;x86_64}"
if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found" >&2; exit 1
fi

# Build the plug-in list: explicit args, else every top-level dir with a
# CMakeLists.txt (skipping dot/build/dist dirs).
PLUGINS=""
if [ -n "$PLUGIN_ARGS" ]; then
    PLUGINS="$PLUGIN_ARGS"
else
    for d in */; do
        n="${d%/}"
        case "$n" in .*|build-*|dist) continue ;; esac
        if [ -f "$n/CMakeLists.txt" ]; then PLUGINS="$PLUGINS $n"; fi
    done
fi

echo "SDK : $AE_SDK"
echo "OUT : $OUT"
echo "Arch: $ARCHS"
echo

mkdir -p "$OUT"
ok=""; fail=""; nok=0; nfail=0; ntot=0
for name in $PLUGINS; do
    ntot=$((ntot + 1))
    if [ ! -f "$name/CMakeLists.txt" ]; then
        echo "== $name : SKIP (no CMakeLists.txt)"
        fail="$fail $name"; nfail=$((nfail + 1)); continue
    fi
    echo "== $name =="
    bd="build-${name}"
    if [ "$CLEAN" = 1 ]; then rm -rf "$bd"; fi
    if ! cmake -S "$name" -B "$bd" \
            -DAE_SDK="$AE_SDK" \
            -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
            -DCMAKE_BUILD_TYPE=Release; then
        echo "   configure FAILED"; fail="$fail $name"; nfail=$((nfail + 1)); continue
    fi
    if ! cmake --build "$bd" --config Release -j; then
        echo "   build FAILED"; fail="$fail $name"; nfail=$((nfail + 1)); continue
    fi
    bundle="$bd/${name}.plugin"
    if [ ! -d "$bundle" ]; then
        bundle="$(find "$bd" -maxdepth 2 -name '*.plugin' -type d 2>/dev/null | head -1)"
    fi
    if [ -z "$bundle" ] || [ ! -d "$bundle" ]; then
        echo "   no .plugin bundle produced"; fail="$fail $name"; nfail=$((nfail + 1)); continue
    fi
    base="$(basename "$bundle")"
    rm -rf "$OUT/$base"
    cp -R "$bundle" "$OUT/"
    echo "   -> $OUT/$base"
    ok="$ok $name"; nok=$((nok + 1))
done

echo
echo "Built ${nok}/${ntot}:${ok:- none}"
if [ "$nfail" -gt 0 ]; then
    echo "FAILED:${fail}"
    exit 1
fi
echo "All plug-ins in: $OUT"
