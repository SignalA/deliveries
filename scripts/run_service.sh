#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 PHYS_GUN1 PHYS_GUN2 [RUNTIME_DIR] [PLAYER1] [PLAYER2]" >&2
    exit 2
fi

[ -n "$1" ] && [ -n "$2" ] || { echo "Physical port values must be non-empty" >&2; exit 2; }
[ "$1" != "$2" ] || { echo "The two physical ports must differ" >&2; exit 2; }
runtime=${3:-/tmp/gaime-input}
player1=${4:-1}
player2=${5:-2}
case "$player1" in 1|3) :;; *) echo "PLAYER1 must be 1 or 3" >&2; exit 2;; esac
case "$player2" in 2|4) :;; *) echo "PLAYER2 must be 2 or 4" >&2; exit 2;; esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)
binary="$root/bin/arm-linux-gnueabi/gaime_input_service"
[ -x "$binary" ] || { echo "Not executable: $binary" >&2; exit 1; }
if [ ! -d "$runtime" ]; then
    old_umask=$(umask)
    umask 077
    mkdir -p "$runtime"
    umask "$old_umask"
fi
chmod 700 "$runtime"
[ ! -e "$runtime/input.sock" ] || { echo "$runtime/input.sock already exists; check for a running service" >&2; exit 1; }

exec "$binary" --mode evdev --phys1 "$1" --phys2 "$2" \
    --player1 "$player1" --player2 "$player2" --screen 32768x32768 \
    --socket "$runtime/input.sock" --result-dir "$runtime"

