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

# index.js is only the Emscripten loader; it calls createWasm() to fetch the
# compiled module. Installing it without the .wasm leaves a shell that can
# never start, which is what the server's nuiShellReady() check now detects.
WASM_SRC="$ROOT_DIR/build/nui-frontend/bin/index.wasm"
if [[ ! -f "$WASM_SRC" ]]; then
  echo "build_nui_frontend: expected $WASM_SRC after the parcel build" >&2
  exit 1
fi
install -m 0644 "$WASM_SRC" "$ROOT_DIR/public/nui/index.wasm"
