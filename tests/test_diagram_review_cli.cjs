const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const path = require("node:path");

const cli = path.resolve(__dirname, "../scripts/review_diagram.cjs");
const invoke = (diagram, args = []) => spawnSync(process.execPath, [cli, ...args, "-"], {
  input: typeof diagram === "string" ? diagram : JSON.stringify(diagram),
  encoding: "utf8"
});

const clean = {
  nodes: [
    { key: "client", title: "Client" },
    { key: "api", title: "API" }
  ],
  edges: [{ key: "request", source: "client", target: "api", label: "HTTPS", directed: true }]
};
const cleanRun = invoke(clean);
assert.equal(cleanRun.status, 0, cleanRun.stderr);
assert.equal(JSON.parse(cleanRun.stdout).runs[0].results.length, 0);
assert.match(cleanRun.stderr, /0 error\(s\), 0 warning\(s\), 0 info finding\(s\)/);

const warning = {
  nodes: [{ key: "orphan", title: "Orphan" }],
  edges: []
};
const defaultThreshold = invoke(warning);
assert.equal(defaultThreshold.status, 0, defaultThreshold.stderr);
assert.equal(JSON.parse(defaultThreshold.stdout).runs[0].results[0].ruleId, "orphan_node");

const warningThreshold = invoke(warning, ["--fail-on", "warning"]);
assert.equal(warningThreshold.status, 1);
assert.equal(JSON.parse(warningThreshold.stdout).version, "2.1.0", "a failing gate must still emit SARIF");

const ignoredThreshold = invoke({ nodes: [], edges: [] }, ["--fail-on", "none"]);
assert.equal(ignoredThreshold.status, 0);
assert.equal(JSON.parse(ignoredThreshold.stdout).runs[0].results[0].level, "error");

const invalidJson = invoke("not-json-with-private-content");
assert.equal(invalidJson.status, 2);
assert.equal(invalidJson.stdout, "");
assert.equal(invalidJson.stderr, "review failed: input is not valid JSON\n");
assert(!invalidJson.stderr.includes("private-content"), "parse failures must not echo input");

const invalidShape = invoke({ title: "missing arrays" });
assert.equal(invalidShape.status, 2);
assert.match(invalidShape.stderr, /must contain nodes and edges arrays/);

const invalidOption = spawnSync(process.execPath, [cli, "--fail-on", "fatal", "-"], {
  input: JSON.stringify(clean),
  encoding: "utf8"
});
assert.equal(invalidOption.status, 2);
assert.match(invalidOption.stderr, /invalid --fail-on level/);

console.log("diagram review CLI tests passed");
