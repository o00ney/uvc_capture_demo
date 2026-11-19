#!/usr/bin/env bash
set -euo pipefail

# Relative remote paths are resolved from the SSH user's home directory.
destination='jetson:Desktop/uvc_capture_demo/'
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

options=(-rltv)
case "${1:-}" in
    '') ;;
    --dry-run) options+=(--dry-run) ;;
    *) echo "Usage: $0 [--dry-run]" >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    echo "Usage: $0 [--dry-run]" >&2
    exit 2
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
file_list="$work_dir/files"
filtered_list="$work_dir/selected"
mkdir "$work_dir/project"
cd -- "$project_dir"

# Git handles nested ignore files and negation rules. Also exclude ignored
# tracked files, which git ls-files would otherwise include.
git ls-files --cached --others --exclude-standard -z > "$file_list"
: > "$filtered_list"
while IFS= read -r -d '' path; do
    if git check-ignore --no-index -q -- "$path"; then
        continue
    else
        status=$?
        if (( status != 1 )); then
            exit "$status"
        fi
    fi
    # Locally deleted tracked files should not be sent.
    if [[ -e "$path" || -L "$path" ]]; then
        printf '%s\0' "$path" >> "$filtered_list"
    fi
done < "$file_list"

# Build a complete source tree so --delete also handles removed files/directories.
rsync -rlt --from0 --files-from="$filtered_list" ./ "$work_dir/project/"

# Ask Git which device-local paths are ignored, including nested/negated rules.
# Keep ignored paths on the device even when they do not exist locally.
ssh jetson 'if [ -d Desktop/uvc_capture_demo ]; then
    cd Desktop/uvc_capture_demo &&
    find . -mindepth 1 -path "./.git" -prune -o -type d -printf "%P/\0" -o -printf "%P\0"
fi' > "$work_dir/remote-files"
if git check-ignore --no-index --stdin -z < "$work_dir/remote-files" > "$work_dir/ignored"; then
    :
else
    status=$?
    if (( status != 1 )); then
        exit "$status"
    fi
fi

# Escape literal names before passing them to rsync's exclusion patterns.
python3 - "$work_dir/ignored" "$work_dir/excludes" <<'PYTHON'
import sys
from pathlib import Path

patterns = [b'/.git/']
for path in Path(sys.argv[1]).read_bytes().split(b'\0'):
    if not path:
        continue
    for char in (b'\\', b'*', b'?', b'['):
        path = path.replace(char, b'\\' + char)
    patterns.append(b'/' + path)
Path(sys.argv[2]).write_bytes(b'\0'.join(patterns) + b'\0')
PYTHON

# Excluded files are protected from deletion; never use --delete-excluded.
rsync "${options[@]}" --delete -e ssh --from0 \
    --exclude-from="$work_dir/excludes" "$work_dir/project/" "$destination"
