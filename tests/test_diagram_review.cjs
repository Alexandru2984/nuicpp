const assert = require("node:assert/strict");
const { analyzeDiagram, createSarifReport } = require("../public/diagram-review.js");

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

const sarif = createSarifReport(problematic, { artifactUri: "reports\\architecture\n.json" });
assert.equal(sarif.version, "2.1.0");
assert.equal(sarif.$schema, "https://json.schemastore.org/sarif-2.1.0.json");
assert.equal(sarif.runs.length, 1);
const run = sarif.runs[0];
assert.equal(run.tool.driver.name, "NuiGraph Architecture Review");
assert.equal(run.tool.driver.rules.length, 13);
assert.equal(run.artifacts[0].location.uri, "reports/architecture.json");
assert.deepEqual(run.invocations[0].properties.summary, result.summary);
assert.equal(run.results.length, result.findings.length);
const sarifRuleIds = new Set(run.tool.driver.rules.map((rule) => rule.id));
assert.equal(sarifRuleIds.size, run.tool.driver.rules.length, "SARIF rules must be unique");
for (const finding of run.results) {
  assert(sarifRuleIds.has(finding.ruleId), `missing SARIF rule metadata for ${finding.ruleId}`);
  assert.equal(run.tool.driver.rules[finding.ruleIndex].id, finding.ruleId);
  assert.match(finding.partialFingerprints["nuigraphFinding/v1"], /^v1-[0-9a-f]{8}$/);
  assert.equal(finding.locations[0].physicalLocation.artifactLocation.index, 0);
  assert.equal(finding.locations[0].physicalLocation.region.startLine, 1);
}
assert.equal(run.results.find((finding) => finding.ruleId === "missing_edge_endpoint").level, "error");
assert.equal(run.results.find((finding) => finding.ruleId === "missing_edge_label").level, "note");
assert.equal(JSON.stringify(problematic), before, "SARIF export must not mutate the diagram");

const cleanSarif = createSarifReport(clean);
assert.equal(cleanSarif.runs[0].results.length, 0);
assert.equal(cleanSarif.runs[0].artifacts[0].location.uri, "diagram.json");

const trustBoundaryReview = analyzeDiagram({
  nodes: [
    { key: "browser", type: "external", title: "Browser", metadata: { trust_zone: "internet" } },
    { key: "database", type: "database", title: "Database", metadata: { trust_zone: "data" } },
    { key: "worker", type: "service", title: "Worker", metadata: {} }
  ],
  edges: [
    { key: "unsafe", source: "browser", target: "database", label: "", directed: true },
    { key: "work", source: "database", target: "worker", label: "jobs", directed: true }
  ]
});
const trustBoundaryCodes = new Set(trustBoundaryReview.findings.map((finding) => finding.code));
for (const code of ["missing_trust_zone", "unlabelled_trust_boundary", "direct_internet_data_flow"]) {
  assert(trustBoundaryCodes.has(code), `missing trust-boundary finding ${code}`);
}
assert.equal(trustBoundaryReview.summary.error, 1);

const invalidTrustZone = analyzeDiagram({
  nodes: [{ key: "api", type: "api", title: "API", metadata: { trust_zone: "dmz" } }],
  edges: []
});
assert(invalidTrustZone.findings.some((finding) => finding.code === "invalid_trust_zone"));

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
