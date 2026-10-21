#!/usr/bin/env bash
# Renders steady on/off-load takes for one engine kind.
# usage: tools/engine-sounds/render.sh <kind> <out-root>
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"  # the script compiler searches es/ relative to the working directory

kind="$1"
out="$2/$kind"

extra=()
case "$kind" in
    r4) rpms=750,1050,1400,1900,2600,3500,4800,6500 ;;
    r6|r6b) rpms=700,1000,1300,1700,2250,2950,3800,5000,6500 ;;
    v6) rpms=750,1000,1300,1700,2200,2900,3900,5100 ;;
    v8_diesel) rpms=650,800,1050,1300,1600,2050,2550,3200; extra=(--fuel-cut) ;;
    flat2) rpms=1000,1300,1700,2250,3000,3900,5200,7000 ;;
    single_2t) rpms=2000,2450,2950,3600,4400,5350,6550,8000 ;;
    twin_2t) rpms=1200,1550,2000,2550,3250,4150,5300,6800 ;;
    *) echo "unknown kind: $kind" >&2; exit 2 ;;
esac

mkdir -p "$out"
build-headless/engine-sim-cli \
    --script "tools/engine-sounds/$kind.mr" \
    --kind "$kind" \
    --out "$out" \
    --rpms "$rpms" \
    ${extra[@]+"${extra[@]}"}
