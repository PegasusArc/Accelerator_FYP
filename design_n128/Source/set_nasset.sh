#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# set_nasset.sh N — set the interleaving factor everywhere it is written down.
#
# Two places, and they must agree. The kernels take it as -DNASSET on the
# compile line; the host has its own #ifndef NASSET / #define block because it
# is built with plain g++ and never sees that flag. If they disagree the host
# feeds a different number of instruments than the hardware was scheduled for,
# the dependence promise is violated on every tick, and the design reads stale
# state while reporting nothing wrong.
#
#   ./set_nasset.sh 128     before the 128 build
#   ./set_nasset.sh 32      to go back
#
# Prints both values afterwards so the agreement is visible rather than assumed.
# ─────────────────────────────────────────────────────────────────────────────
set -e
N=${1:-}
case "$N" in
    ''|*[!0-9]*) echo "usage: $0 <N>   e.g. $0 128"; exit 1;;
esac
if [ "$N" -gt 256 ]; then
    echo "ASSET_ID_W is 8 bits, so the instrument identifier cannot exceed 256."
    exit 1
fi

BUILD="$(cd "$(dirname "$0")" && pwd)/build_all.sh"
HOST="$(cd "$(dirname "$0")/../host" && pwd)/host_rr.cpp"

[ -f "$BUILD" ] || { echo "not found: $BUILD"; exit 1; }
[ -f "$HOST" ]  || { echo "not found: $HOST"; exit 1; }

sed -i -E "s/^NASSET=[0-9]+/NASSET=$N/" "$BUILD"
sed -i -E "s/^#define NASSET [0-9]+/#define NASSET $N/" "$HOST"

echo "kernels : $(grep -E '^NASSET=' "$BUILD")"
echo "host    : $(grep -E '^#define NASSET' "$HOST")"
echo
echo "Both must read $N. Rebuild the kernels AND the host - the host's value is"
echo "compiled in, so an old binary silently disagrees with a new bitstream."
