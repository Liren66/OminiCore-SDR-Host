#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
# Keep old FPGA SDK tools out of the native host build.
export PATH=/usr/bin:/bin:$PATH
build=${OCH_BUILD_DIR:-build/linux}
prefix=${OCH_INSTALL_DIR:-dist/OmniCoreHost-linux}
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel "${OCH_JOBS:-4}"
cmake --install "$build" --prefix "$(realpath -m "$prefix")"
printf '\nReady: %s/bin/och\n' "$prefix"
