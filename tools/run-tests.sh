#!/usr/bin/env bash
#
# Build the engine with CMake and run the self-tests (ctest). Exits non-zero
# on failure. Binaries and rendered PNGs land in build/cmake/.
#
#   tools/run-tests.sh                 # configure + build + all tests
#   tools/run-tests.sh eel beat        # a subset (ctest -R names: eel fft beat effects gallery avs)
#   tools/run-tests.sh local           # also render the local classic set, if present
#   tools/run-tests.sh corpus          # also sweep the whole local archive (long)
#
# The tracked fixtures under presets/tests/ are ours (tools/make-test-presets.py).
# presets/classic/ and presets/extracted/ are local-only content and never
# required: their stages run only when the folders exist and are asked for.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
BUILD="$REPO/build/cmake"

want_local=0; want_corpus=0; names=()
for a in "$@"; do
    case "$a" in
        local)  want_local=1 ;;
        corpus) want_corpus=1 ;;
        *)      names+=("$a") ;;
    esac
done

echo "== configure + build (cmake) =="
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > "$REPO/build/cmake-configure.log" 2>&1 \
    || { cat "$REPO/build/cmake-configure.log"; exit 1; }
cmake --build "$BUILD" --parallel

echo "== self-tests (ctest) =="
if [[ ${#names[@]} -gt 0 ]]; then
    pattern="^($(IFS='|'; echo "${names[*]}"))$"
    ctest --test-dir "$BUILD" --output-on-failure -R "$pattern"
else
    ctest --test-dir "$BUILD" --output-on-failure
fi

if [[ $want_local -eq 1 && -d "$REPO/presets/classic" ]]; then
    echo "== local classic set (not tracked) =="
    mkdir -p "$BUILD/test-out/classic"
    "$BUILD/avs_selftest" "$REPO/presets/classic" "$BUILD/test-out/classic" | tail -3
fi
if [[ $want_corpus -eq 1 && -d "$REPO/presets/extracted" ]]; then
    echo "== local archive sweep (not tracked; long) =="
    AVS_FRAMES=30 AVS_NO_PNG=1 "$BUILD/avs_selftest" "$REPO/presets/extracted" "$BUILD/test-out" | tail -3
fi

echo
echo "run-tests: ALL PASS"
