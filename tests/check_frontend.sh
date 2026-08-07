#!/usr/bin/env bash
set -euo pipefail

node --check public/editor.js >/dev/null
node --check public/app.js >/dev/null
node --check public/nui/bootstrap.js >/dev/null
grep -q "nuigraph:draft:" public/editor.js
grep -q "draft-status" public/editor.js

# Two shells render the canvas: the server-rendered fallback and the Nui/WASM
# one. editor.js drives both, so a layer added to one and forgotten in the other
# silently changes what the canvas looks like depending on which shell served.
for layer in edges nodes overlay; do
  grep -q "id=\"${layer}\"" src/ui/NuiApp.cpp
  grep -q "id = \"${layer}\"" nui-frontend/frontend/source/frontend/main_page.cpp
done
