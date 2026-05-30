#!/usr/bin/env bash

set -e

DR_ROOT="$(pwd)/ext/dynamorio"
BUILD_DIR="$(pwd)/build"
CLIENT_SO="$BUILD_DIR/libetcvalidation.so"
DRRUN="$DR_ROOT/build/bin64/drrun"

if [ ! -f "$DRRUN" ]; then
    echo "drrun not found at $DRRUN"
    exit 1
fi

if [ ! -f "$CLIENT_SO" ]; then
    echo "client not found at $CLIENT_SO"
    exit 1
fi

export LD_LIBRARY_PATH="$DR_ROOT/build/bin64:$LD_LIBRARY_PATH"

# workload is just ls, change to execution of one of the PolyBench/PARSEC/SPEC or whatever
exec "$DRRUN" -c "$CLIENT_SO" -- /bin/ls "$@"
