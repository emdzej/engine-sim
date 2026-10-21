#!/usr/bin/env bash
# Renders steady on/off-load takes for one engine kind.
# usage: tools/engine-sounds/render.sh <kind> <out-root>
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"  # the script compiler searches es/ relative to the working directory

kind="$1"
out="$2/$kind"

case "$kind" in
    r4) rpms=750,1050,1400,1900,2600,3500,4800,6500 ;;
    r6) rpms=1000,1300,1700,2250,2950,3800,5000,6500 ;;
    v6) rpms=750,1000,1300,1700,2200,2900,3900,5100 ;;
    *) echo "unknown kind: $kind" >&2; exit 2 ;;
esac

mkdir -p "$out"
build-headless/engine-sim-cli \
    --script "tools/engine-sounds/$kind.mr" \
    --kind "$kind" \
    --out "$out" \
    --rpms "$rpms"
