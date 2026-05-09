#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMCMAKE="${EMCMAKE:-/home/micu/emsdk/upstream/emscripten/emcmake}"

"$EMCMAKE" cmake -S "$ROOT_DIR/nui-frontend" -B "$ROOT_DIR/build/nui-frontend" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF

cmake --build "$ROOT_DIR/build/nui-frontend" --config Release -j "$(nproc)"
cmake --build "$ROOT_DIR/build/nui-frontend" --target nuigraph-nui-frontend-parcel --config Release -j "$(nproc)"
install -m 0644 "$ROOT_DIR/build/nui-frontend/bin/index.js" "$ROOT_DIR/public/nui/index.js"
