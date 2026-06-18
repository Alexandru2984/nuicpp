#!/usr/bin/env bash
set -euo pipefail

node --check public/editor.js >/dev/null
node --check public/app.js >/dev/null
node --check public/nui/bootstrap.js >/dev/null
grep -q "nuigraph:draft:" public/editor.js
grep -q "draft-status" public/editor.js
