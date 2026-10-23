#!/usr/bin/env bash
set -euo pipefail

node --check public/editor.js >/dev/null
node --check public/app.js >/dev/null
node --check public/diagram-review.js >/dev/null
node --check public/diagram-arrange.js >/dev/null
node --check public/offline.js >/dev/null
node --check public/sw.js >/dev/null
node --check public/nui/bootstrap.js >/dev/null
grep -q "nuigraph:draft:" public/editor.js
grep -q "draft-status" public/editor.js
grep -q 'ASSET_VERSION = "__ASSET_VERSION__"' public/sw.js
grep -q 'asset("/offline.html")' public/sw.js
grep -q 'asset("/offline.js")' public/sw.js
grep -q 'asset("/diagram-review.js")' public/sw.js
grep -q 'asset("/diagram-arrange.js")' public/sw.js
grep -q '<script src="/offline.js?v=__ASSET_VERSION__"></script>' public/offline.html
grep -Fq 'const assetQuery = document.currentScript' public/app.js
grep -Fq 'navigator.serviceWorker.register(`/sw.js' public/app.js
grep -Fq 'script.src = `/editor.js' public/app.js
grep -Fq 'review.src = `/diagram-review.js' public/app.js
grep -Fq 'arrange.src = `/diagram-arrange.js' public/app.js
grep -Fq 'css.href = `/styles.css' public/editor.js
grep -Fq 'Ctrl / Cmd + K' public/editor.js
grep -Fq 'Find and focus node' public/editor.js
grep -Fq 'Edit selected label' public/editor.js
grep -Fq 'className = "inline-label-editor"' public/editor.js
grep -Fq 'applyBatchProperties' public/editor.js
grep -Fq 'id="multi-node-props"' src/ui/NuiApp.cpp
grep -Fq 'id = "multi-node-props"' nui-frontend/frontend/source/frontend/main_page.cpp
grep -Fq 'Share link lifetime' public/editor.js
grep -Fq 'NODE_NAVIGATOR_RESULT_LIMIT = 100' public/editor.js
grep -Fq 'evt.key === "/"' public/editor.js
grep -Fq 'nuigraph:mobile-actions:v1' public/editor.js
grep -Fq 'className = "command-palette-overlay"' public/editor.js
test "$(grep -Fc 'if (state.current.nodes?.length) zoomToFit();' public/editor.js)" -eq 2
grep -Fq '.mobile-action-bar' public/styles.css
grep -Fq '.inline-label-editor' public/styles.css
node tests/test_diagram_review.cjs
node tests/test_diagram_arrange.cjs
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
