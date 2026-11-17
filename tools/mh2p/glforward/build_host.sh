#!/usr/bin/env bash
# Host graphics bridge: Mesa EGL/GLES on Linux and macOS.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-${MH2P_BUILD_DIR:-/tmp}/mh2p-glhost}
source=${2:-tools/mh2p/glforward/host/glhost.c}
# pkg-config emits shell words, including Homebrew's non-system include/lib paths.
mesa_config=$(pkg-config --cflags --libs egl glesv2)
read -r -a mesa_flags <<< "$mesa_config"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -O2 "$source" -o "$out" "${mesa_flags[@]}" -pthread -lm
