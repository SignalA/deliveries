#!/bin/sh
set -u

CALIB_DIR="${CALIB_DIR:-/tmp/calibration-demo-armel}"
SERVER_SOCK="${SERVER_SOCK:-/tmp/gaime-input/input.sock}"
CLIENT_SOCK="${CLIENT_SOCK:-/tmp/gaime-calibration.sock}"
RESULT_FILE="${RESULT_FILE:-/tmp/gaime-calibration-result.json}"
GAME_START="${GAME_START:-/g3/start.sh}"
GUN="${1:-0}"
PLAYER="${2:-0}"

killall game 2>/dev/null || true
rm -f "$CLIENT_SOCK"

cd "$CALIB_DIR" || exit 1
./launch.sh --input uds --gun "$GUN" --player "$PLAYER" \
    --server-sock "$SERVER_SOCK" --client-sock "$CLIENT_SOCK" \
    --output "$RESULT_FILE" --verbose
calib_status=$?

# CALIB_RESULT is a datagram. Give the input service time to finish the HID
# 0x06 upload and index=0x08 verification before the game opens gun devices.
sleep 3

ulimit -n 4096 2>/dev/null || true
exec "$GAME_START"

exit "$calib_status"
