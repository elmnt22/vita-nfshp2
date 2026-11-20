#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:/usr/bin:$PATH"
args=()
if [[ -n "${HP2_ICON_PYTHON:-}" ]]; then args+=("-DHP2_ICON_PYTHON=$HP2_ICON_PYTHON"); fi
cmake -S . -B build-nx/switch1 -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DHP2_SWITCH_LTO=ON -DHP2_SWITCH_PGO=off \
    -DHP2_SWITCH_DIAGNOSTICS=OFF "${args[@]}"
cmake --build build-nx/switch1 -j "${JOBS:-8}"
