#!/usr/bin/env bash
# Run image_benchmark on the Jetson and archive the result locally under
# build/bench/<timestamp>/ with metadata and a tegrastats log.
# Lock clocks first (script/perf_mode.sh lock) for comparable numbers.
set -euo pipefail

usage() { echo "Usage: $0 [--jpeg input.jpg] [image_benchmark options...]" >&2; exit 2; }
jpeg=
if [[ "${1:-}" == --jpeg ]]; then
    (( $# >= 2 )) || usage
    jpeg=$2
    shift 2
    [[ -f "$jpeg" ]] || { echo "not a file: $jpeg" >&2; exit 1; }
fi

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date +%Y%m%d-%H%M%S)"
commit="$(git -C "$project_dir" describe --always --dirty)"
remote_out="Desktop/uvc_capture_demo/build/bench/$stamp"

"$project_dir/script/sync_to_jetson.sh" > /dev/null
ssh jetson mkdir -p "$remote_out"
if [[ -n "$jpeg" ]]; then
    scp -q "$jpeg" "jetson:$remote_out/input.jpg"
fi

ssh jetson bash -s -- "$stamp" "$commit" "$@" <<'REMOTE'
set -euo pipefail
cd Desktop/uvc_capture_demo
stamp=$1 commit=$2
shift 2
out=build/bench/$stamp

[ -f build/CMakeCache.txt ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)" --target image_benchmark > /dev/null

if [ ! -e "$out/input.jpg" ]; then
    # Synthetic 720p frame; a real camera image is more representative.
    gst-launch-1.0 -q videotestsrc num-buffers=1 ! video/x-raw,width=1280,height=720 \
        ! jpegenc ! filesink location="$out/input.jpg"
fi

clocks=dynamic
[ -e /tmp/image_pipeline_clocks.conf ] && clocks=locked
[ "$clocks" = locked ] || echo "warning: clocks are dynamic; results may vary" >&2
{
    echo "date: $(date -Is)"
    echo "commit: $commit"
    echo "host: $(uname -srm)"
    echo "power_mode: $(sudo nvpmodel -q | head -n1)"
    echo "clocks: $clocks"
    echo "args: $*"
} > "$out/meta.txt"

tegrastats --interval 500 > "$out/tegrastats.log" &
stats=$!
trap 'kill "$stats" 2>/dev/null || true' EXIT
./build/image_benchmark --jpeg-file "$out/input.jpg" --save-prefix "$out/result" "$@" \
    | tee "$out/benchmark.txt"
REMOTE

mkdir -p "$project_dir/build/bench"
rsync -a "jetson:$remote_out" "$project_dir/build/bench/"
echo "saved: build/bench/$stamp"
