#!/usr/bin/env bash
set -euo pipefail

TARGET=${1:-1}
shift || true

ORIG=$(cat /proc/sys/kernel/perf_event_paranoid)

echo "original = $ORIG"
echo "setting  = $TARGET"

sudo sysctl -w kernel.perf_event_paranoid="$TARGET" >/dev/null

cleanup() {
    echo "restoring = $ORIG"
    sudo sysctl -w kernel.perf_event_paranoid="$ORIG" >/dev/null
}

trap cleanup EXIT INT TERM ERR

DR_ROOT="$(pwd)/ext/dynamorio"
BUILD_DIR="$(pwd)/build"
CLIENT_SO="$BUILD_DIR/libetcvalidation.so"
DRRUN="$DR_ROOT/build/bin64/drrun"

if [[ ! -x "$DRRUN" ]]; then
    echo "missing drrun"
    exit 1
fi

if [[ ! -f "$CLIENT_SO" ]]; then
    echo "missing client"
    exit 1
fi

export LD_LIBRARY_PATH="$DR_ROOT/build/bin64:${LD_LIBRARY_PATH:-}"

WORKLOAD=${1:-/bin/ls}
shift || true

echo "running workload: $WORKLOAD"

"$DRRUN" -c "$CLIENT_SO" -- "$WORKLOAD" "$@"

EXIT_CODE=$?

echo "dr exited with code $EXIT_CODE"

exit $EXIT_CODE
