#!/usr/bin/env bash
# Builds build-gasm/engine-sim.wasm with the gasm C SDK.
# usage: tools/gasm/build.sh [path to a gasm checkout or SDK bundle] [extra cmake args...]
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
gasm="${1:-${GASM_DIR:-$root/../gasm}}"
shift || true
cd "$root"

# Same submodule patches as the native headless build
for patch in tools/patches/*.patch; do
    dir="dependencies/submodules/$(basename "$patch" .patch)"
    git -C "$dir" apply --reverse --check "$root/$patch" 2>/dev/null || git -C "$dir" apply "$root/$patch"
done

toolchain="$gasm/sdk/c/cmake/gasm-toolchain.cmake"
[[ -f "$toolchain" ]] || toolchain="$gasm/cmake/gasm-toolchain.cmake"   # release bundle layout

prefix="$(brew --prefix 2>/dev/null || true)"
extra=()
if [[ -n "$prefix" ]]; then
    extra+=(
        -DBISON_EXECUTABLE="$prefix/opt/bison/bin/bison"
        -DFLEX_EXECUTABLE="$prefix/opt/flex/bin/flex"
        -DFLEX_INCLUDE_DIR="$prefix/opt/flex/include"
    )
fi

cmake -S tools/gasm -B build-gasm \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release \
    -Wno-dev \
    ${extra[@]+"${extra[@]}"} "$@"
cmake --build build-gasm -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ls -la build-gasm/engine-sim.wasm
