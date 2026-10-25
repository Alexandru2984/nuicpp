#!/usr/bin/env bash
set -euo pipefail

node --check public/editor.js >/dev/null
node --check public/app.js >/dev/null
node --check public/diagram-review.js >/dev/null
node --check public/diagram-arrange.js >/dev/null
node --check public/diagram-selection.js >/dev/null
node --check public/canvas-rulers.js >/dev/null
node --check public/offline.js >/dev/null
node --check public/sw.js >/dev/null
node --check public/nui/bootstrap.js >/dev/null
node --check nui-frontend/static/source/index.js >/dev/null
node --check nui-frontend/tests/browser.spec.cjs >/dev/null
node -e 'require("./nui-frontend/node_modules/@axe-core/playwright")' >/dev/null
grep -q "nuigraph:draft:" public/editor.js
grep -q "draft-status" public/editor.js
grep -q 'ASSET_VERSION = "__ASSET_VERSION__"' public/sw.js
grep -q 'asset("/offline.html")' public/sw.js
grep -q 'asset("/offline.js")' public/sw.js
grep -q 'asset("/diagram-review.js")' public/sw.js
grep -q 'asset("/diagram-arrange.js")' public/sw.js
grep -q 'asset("/diagram-selection.js")' public/sw.js
grep -q 'asset("/canvas-rulers.js")' public/sw.js
grep -q '<script src="/offline.js?v=__ASSET_VERSION__"></script>' public/offline.html
grep -Fq 'const assetQuery = document.currentScript' public/app.js
grep -Fq 'navigator.serviceWorker.register(`/sw.js' public/app.js
grep -Fq 'script.src = `/editor.js' public/app.js
grep -Fq 'review.src = `/diagram-review.js' public/app.js
grep -Fq 'arrange.src = `/diagram-arrange.js' public/app.js
grep -Fq 'selection.src = `/diagram-selection.js' public/app.js
grep -Fq 'rulers.src = `/canvas-rulers.js' public/app.js
grep -Fq 'selection.onload = attachRulers' public/app.js
for loader in public/nui/bootstrap.js nui-frontend/static/source/index.js; do
  grep -Fq 'review.src = `/diagram-review.js' "$loader"
  grep -Fq 'arrange.src = `/diagram-arrange.js' "$loader"
  grep -Fq 'selection.src = `/diagram-selection.js' "$loader"
  grep -Fq 'rulers.src = `/canvas-rulers.js' "$loader"
  grep -Fq 'review.onload = attachArrange' "$loader"
  grep -Fq 'arrange.onload = attachSelection' "$loader"
  grep -Fq 'selection.onload = attachRulers' "$loader"
  grep -Fq 'rulers.onload = attachEditor' "$loader"
done
grep -Fq 'css.href = `/styles.css' public/editor.js
grep -Fq 'Ctrl / Cmd + K' public/editor.js
grep -Fq 'Find and focus node' public/editor.js
grep -Fq 'Edit selected label' public/editor.js
grep -Fq 'className = "inline-label-editor"' public/editor.js
grep -Fq 'applyBatchProperties' public/editor.js
grep -Fq 'Frame selection' public/editor.js
grep -Fq 'Actual size (100%)' public/editor.js
grep -Fq 'function resetZoom()' public/editor.js
grep -Fq 'zoomLabel.setAttribute("aria-label", `${zoomPercent} zoom; reset to 100%`)' public/editor.js
grep -Fq 'Enter presentation mode' public/editor.js
grep -Fq 'function togglePresentationMode(force)' public/editor.js
grep -Fq 'evt.shiftKey && evt.key.toLowerCase() === "p"' public/editor.js
grep -Fq 'Print diagram' public/editor.js
grep -Fq 'function beginPrintLayout()' public/editor.js
grep -Fq 'window.addEventListener("afterprint", restorePrintLayout)' public/editor.js
grep -Fq 'Export embeddable SVG' public/editor.js
grep -Fq 'Export selection as SVG' public/editor.js
grep -Fq 'function makeSvgSafeToEmbed(clone)' public/editor.js
grep -Fq 'Export transparent PNG' public/editor.js
grep -Fq 'Export architecture review as SARIF' public/editor.js
grep -Fq 'application/sarif+json' public/editor.js
grep -Fq 'Export selection as transparent PNG' public/editor.js
grep -Fq 'PNG_EXPORT_MAX_PIXELS' public/editor.js
grep -Fq 'Toggle canvas rulers' public/editor.js
grep -Fq 'function drawCanvasRulers()' public/editor.js
grep -Fq 'evt.code === "Digit1"' public/editor.js
grep -Fq 'evt.code === "Digit2"' public/editor.js
grep -Fq 'function snapValue(value, grid = state.gridSize)' public/editor.js
grep -Fq 'nuigraph:grid-size:v1' public/editor.js
grep -Fq 'nudgeSelectedNodes' public/editor.js
grep -Fq 'evt.key === "ArrowLeft"' public/editor.js
grep -Fq 'function selectAllNodes()' public/editor.js
grep -Fq 'function selectConnectedNodes()' public/editor.js
grep -Fq 'function selectNeighbourNodes()' public/editor.js
grep -Fq 'function selectTerminalNodes(mode)' public/editor.js
grep -Fq 'evt.key.toLowerCase() === "a"' public/editor.js
grep -Fq 'function nextAvailableKey(base, used, nonce)' public/editor.js
grep -Fq 'function duplicateSelected()' public/editor.js
grep -Fq 'evt.key.toLowerCase() === "d"' public/editor.js
grep -Fq 'function cutSelected()' public/editor.js
grep -Fq 'evt.key.toLowerCase() === "x"' public/editor.js
grep -Fq 'function toggleSelectedGeometryLock()' public/editor.js
grep -Fq 'metadata.geometry_locked = true' public/editor.js
grep -Fq 'if (fetchOptions.body !== undefined' public/editor.js
grep -Fq 'id="grid-size"' src/ui/NuiApp.cpp
grep -Fq 'id = "grid-size"' nui-frontend/frontend/source/frontend/main_page.cpp
for type in queue cache container cloud; do
  grep -Fq "value=\"${type}\"" src/ui/NuiApp.cpp
  grep -Fq "value = \"${type}\"" nui-frontend/frontend/source/frontend/main_page.cpp
  grep -Fq "node-shape-${type}" public/editor.js
done
grep -Fq 'id="zoom-label"' src/ui/NuiApp.cpp
grep -Fq 'id = "zoom-label"' nui-frontend/frontend/source/frontend/main_page.cpp
grep -Fq 'id="multi-node-props"' src/ui/NuiApp.cpp
grep -Fq 'id = "multi-node-props"' nui-frontend/frontend/source/frontend/main_page.cpp
grep -Fq '<h1>NuiGraph Studio</h1>' src/ui/NuiApp.cpp
grep -Fq 'h1{}("NuiGraph Studio")' nui-frontend/frontend/source/frontend/main_page.cpp
grep -Fq 'schema.org/SoftwareApplication' public/nui/index.html
grep -Fq 'schema.org/SoftwareApplication' nui-frontend/static/index.html
grep -Fq 'Share link lifetime' public/editor.js
grep -Fq 'NODE_NAVIGATOR_RESULT_LIMIT = 100' public/editor.js
grep -Fq 'evt.key === "/"' public/editor.js
grep -Fq 'DIAGRAM_OUTLINE_RESULT_LIMIT = 250' public/editor.js
grep -Fq 'setAttribute("role", "tree")' public/editor.js
grep -Fq 'Open diagram outline' public/editor.js
grep -Fq 'editor-live-status' public/editor.js
grep -Fq 'function updateSelectionAnnouncement()' public/editor.js
grep -Fq '.sr-only' public/styles.css
grep -Fq 'textarea:not(:disabled):not([tabindex="-1"])' public/editor.js
grep -Fq 'overlay.closeDialog = closeShortcuts' public/editor.js
grep -Fq 'overlay.closeDialog = closeDiff' public/editor.js
grep -Fq 'expectNoHighImpactAccessibilityViolations' nui-frontend/tests/browser.spec.cjs
grep -Fq 'nuigraph:mobile-actions:v1' public/editor.js
grep -Fq 'className = "command-palette-overlay"' public/editor.js
test "$(grep -Fc 'if (state.current.nodes?.length) zoomToFit();' public/editor.js)" -eq 2
grep -Fq '.mobile-action-bar' public/styles.css
grep -Fq '.inline-label-editor' public/styles.css
grep -Fq '.canvas-rulers' public/styles.css
grep -Fq 'body.presentation-mode #app' public/styles.css
grep -Fq '.presentation-controls' public/styles.css
grep -Fq '@page {' public/styles.css
grep -Fq 'body.print-mode .print-header' public/styles.css
grep -Fq '@media (forced-colors: active)' public/styles.css
grep -Fq '.node-lock-body' public/styles.css
grep -Fq '@media (prefers-reduced-motion: reduce)' public/styles.css
grep -Fq '#canvas:focus-visible' public/styles.css
node tests/test_diagram_review.cjs
node tests/test_diagram_review_cli.cjs
node tests/test_diagram_arrange.cjs
node tests/test_diagram_selection.cjs
node tests/test_canvas_rulers.cjs
if rg -n 'innerHTML|outerHTML|insertAdjacentHTML|document\.write|\beval[[:space:]]*\(|new Function' public --glob '*.js'; then
  echo "public JavaScript must not use HTML/code execution sinks" >&2
  exit 1
fi
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
