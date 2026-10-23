#!/usr/bin/env bash
set -euo pipefail

node --check public/editor.js >/dev/null
node --check public/app.js >/dev/null
node --check public/offline.js >/dev/null
node --check public/sw.js >/dev/null
node --check public/nui/bootstrap.js >/dev/null
grep -q "nuigraph:draft:" public/editor.js
grep -q "draft-status" public/editor.js
grep -q 'ASSET_VERSION = "__ASSET_VERSION__"' public/sw.js
grep -q 'asset("/offline.html")' public/sw.js
grep -q 'asset("/offline.js")' public/sw.js
grep -q '<script src="/offline.js?v=__ASSET_VERSION__"></script>' public/offline.html
grep -Fq 'const assetQuery = document.currentScript' public/app.js
grep -Fq 'navigator.serviceWorker.register(`/sw.js' public/app.js
grep -Fq 'script.src = `/editor.js' public/app.js
grep -Fq 'css.href = `/styles.css' public/editor.js
if grep -Eqi '<[^>]+[[:space:]]on[a-z]+[[:space:]]*=' public/offline.html; then
  echo "offline page must not contain inline event handlers" >&2
  exit 1
fi

# Two shells render the canvas: the server-rendered fallback and the Nui/WASM
# one. editor.js drives both, so a layer added to one and forgotten in the other
# silently changes what the canvas looks like depending on which shell served.
for layer in edges nodes overlay; do
  grep -q "id=\"${layer}\"" src/ui/NuiApp.cpp
  grep -q "id = \"${layer}\"" nui-frontend/frontend/source/frontend/main_page.cpp
done
