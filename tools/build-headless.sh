#!/usr/bin/env bash
# Builds the headless engine-sim-cli on macOS (and Linux).
# Requires: cmake, boost, bison >= 3, flex (brew install cmake boost bison flex)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

git submodule update --init --recursive

# The submodules point at upstream repos; carry the portability fixes as patches
for patch in tools/patches/*.patch; do
    name="$(basename "$patch" .patch)"
    dir="dependencies/submodules/$name"
    if git -C "$dir" apply --reverse --check "$root/$patch" 2>/dev/null; then
        continue  # already applied
    fi
    git -C "$dir" apply "$root/$patch"
done

prefix="$(brew --prefix 2>/dev/null || true)"
extra=()
if [[ -n "$prefix" ]]; then
    extra+=(
        -DBISON_EXECUTABLE="$prefix/opt/bison/bin/bison"
        -DFLEX_EXECUTABLE="$prefix/opt/flex/bin/flex"
        -DFLEX_INCLUDE_DIR="$prefix/opt/flex/include"
    )
fi

cmake -S . -B build-headless \
    -DENGINE_SIM_HEADLESS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -Wno-dev \
    "${extra[@]}"
cmake --build build-headless --target engine-sim-cli -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

echo "built: build-headless/engine-sim-cli"
