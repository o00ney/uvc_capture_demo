#!/usr/bin/env bash
# Sync the project to the Jetson, then configure, build and run CTest there.
set -euo pipefail

usage() { echo "Usage: $0 [--no-cuda] [--clean]" >&2; exit 2; }
cuda=ON
clean=0
for arg in "$@"; do
    case "$arg" in
        --no-cuda) cuda=OFF ;;
        --clean) clean=1 ;;
        *) usage ;;
    esac
done

"$(dirname -- "${BASH_SOURCE[0]}")/sync_to_jetson.sh"

# The remote build directory is Git-ignored, so the sync preserves it.
ssh jetson bash -s -- "$cuda" "$clean" <<'REMOTE'
set -euo pipefail
cd Desktop/uvc_capture_demo
if [ "$2" = 1 ]; then rm -rf build; fi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_CUDA="$1"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
REMOTE
