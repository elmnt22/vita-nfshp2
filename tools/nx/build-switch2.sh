#!/usr/bin/env bash
# Run in the devkitPro shell. Mesa stays outside the source tree.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:/usr/bin:$PATH"
: "${HP2_SWITCH2_MESA_SDK:?Set HP2_SWITCH2_MESA_SDK to the external SDK opt/devkitpro/portlibs/switch2 directory}"
args=()
if [[ -n "${HP2_ICON_PYTHON:-}" ]]; then args+=("-DHP2_ICON_PYTHON=$HP2_ICON_PYTHON"); fi
cmake -S . -B build-nx/switch2 -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/libounce/cmake/Switch2.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DHP2_SWITCH_LTO=ON -DHP2_SWITCH_PGO=off \
    -DHP2_SWITCH_DIAGNOSTICS=OFF -DHP2_SWITCH2_MESA_SDK="$HP2_SWITCH2_MESA_SDK" "${args[@]}"
cmake --build build-nx/switch2 -j "${JOBS:-8}"
