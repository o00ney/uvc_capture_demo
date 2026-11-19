#!/usr/bin/env bash
# Lock Jetson clocks for reproducible measurements, and restore them afterwards.
# The power mode (nvpmodel) is only reported: switching it may require a reboot.
set -euo pipefail

usage() { echo "Usage: $0 lock|restore|status" >&2; exit 2; }
(( $# == 1 )) || usage
case "$1" in lock|restore|status) ;; *) usage ;; esac

# The saved file doubles as the "locked" marker. /tmp is cleared on reboot,
# which also resets the clocks, so the marker cannot outlive the locked state.
ssh jetson bash -s -- "$1" <<'REMOTE'
set -euo pipefail
saved=/tmp/image_pipeline_clocks.conf
case "$1" in
lock)
    if [ -e "$saved" ]; then
        echo "clocks already locked; original settings kept in $saved"
    else
        sudo jetson_clocks --store "$saved"
        sudo jetson_clocks
        echo "clocks locked; run script/perf_mode.sh restore when done"
    fi ;;
restore)
    if [ -e "$saved" ]; then
        sudo jetson_clocks --restore "$saved"
        sudo rm -f "$saved"
        echo "clocks restored"
    else
        echo "clocks not locked by this script; nothing to restore"
    fi ;;
status)
    sudo nvpmodel -q
    if [ -e "$saved" ]; then echo "clocks: locked"; else echo "clocks: dynamic"; fi
    sudo jetson_clocks --show ;;
esac
REMOTE
