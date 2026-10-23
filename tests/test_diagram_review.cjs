const assert = require("node:assert/strict");
const { analyzeDiagram } = require("../public/diagram-review.js");

const clean = {
  title: "Clean",
  nodes: [
    { key: "client", title: "Client" },
    { key: "api", title: "API" }
  ],
  edges: [
    { key: "request", source: "client", target: "api", label: "HTTPS", directed: true }
  ]
};
assert.deepEqual(analyzeDiagram(clean), {
  summary: { error: 0, warning: 0, info: 0, total: 0 },
  findings: []
});

const problematic = {
  nodes: [
    { key: "a", title: "" },
    { key: "b", title: "Worker" },
    { key: "orphan", title: "Unused" }
  ],
  edges: [
    { key: "ab", source: "a", target: "b", label: "", directed: true },
    { key: "ab-copy", source: "a", target: "b", label: "again", directed: true },
    { key: "ba", source: "b", target: "a", label: "reply", directed: true },
    { key: "loop", source: "b", target: "b", label: "retry", directed: true },
    { key: "broken", source: "b", target: "missing", label: "lost", directed: true }
  ]
};
const before = JSON.stringify(problematic);
const result = analyzeDiagram(problematic);
const codes = new Set(result.findings.map((finding) => finding.code));
for (const code of [
  "missing_node_title",
  "missing_edge_label",
  "duplicate_edge",
  "self_loop",
  "missing_edge_endpoint",
  "orphan_node",
  "directed_cycle"
]) {
  assert(codes.has(code), `missing expected finding ${code}`);
}
assert.equal(result.summary.error, 1);
assert(result.summary.warning >= 4);
assert.equal(result.summary.info, 1);
assert.equal(JSON.stringify(problematic), before, "review must not mutate the diagram");

const empty = analyzeDiagram({ nodes: [], edges: [] });
assert.equal(empty.summary.error, 1);
assert.equal(empty.findings[0].code, "empty_diagram");

const convergingDag = analyzeDiagram({
  nodes: ["a", "b", "c"].map((key) => ({ key, title: key.toUpperCase() })),
  edges: [
    { key: "ab", source: "a", target: "b", label: "next", directed: true },
    { key: "ac", source: "a", target: "c", label: "next", directed: true },
    { key: "bc", source: "b", target: "c", label: "next", directed: true }
  ]
});
assert.equal(convergingDag.summary.total, 0, "a converging DAG must not be reported as cyclic");

const deepNodeCount = 12000;
const deep = {
  nodes: Array.from({ length: deepNodeCount }, (_, index) => ({
    key: `deep_${index}`,
    title: `Deep ${index}`
  })),
  edges: Array.from({ length: deepNodeCount - 1 }, (_, index) => ({
    key: `deep_edge_${index}`,
    source: `deep_${index}`,
    target: `deep_${index + 1}`,
    label: "next",
    directed: true
  }))
};
const deepResult = analyzeDiagram(deep);
assert.equal(deepResult.summary.total, 0, "deep acyclic graphs must not overflow or report a cycle");

console.log("diagram review tests passed");
