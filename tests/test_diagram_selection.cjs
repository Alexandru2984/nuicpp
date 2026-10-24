const assert = require("node:assert/strict");
const { connectedNodeKeys, neighbourNodeKeys } = require("../public/diagram-selection.js");

const diagram = {
  nodes: ["a", "b", "c", "isolated", "x", "y"].map((key) => ({ key })),
  edges: [
    { source: "a", target: "b", directed: true },
    { source: "c", target: "b", directed: true },
    { source: "x", target: "y", directed: false },
    { source: "a", target: "missing", directed: true }
  ]
};
const before = JSON.stringify(diagram);

assert.deepEqual(connectedNodeKeys(diagram, ["b"]), ["a", "b", "c"],
  "directed edges must form one weakly connected visual component");
assert.deepEqual(connectedNodeKeys(diagram, ["isolated"]), ["isolated"],
  "an isolated seed is its own component");
assert.deepEqual(connectedNodeKeys(diagram, ["y", "a", "a"]), ["a", "b", "c", "x", "y"],
  "multiple seeds must union their components in diagram order");
assert.deepEqual(connectedNodeKeys(diagram, ["missing"]), [],
  "unknown seeds must fail closed");
assert.deepEqual(connectedNodeKeys({ nodes: diagram.nodes }, ["a"]), ["a"],
  "a missing edge list must behave like an empty graph");
assert.deepEqual(connectedNodeKeys(null, ["a"]), []);
assert.deepEqual(neighbourNodeKeys(diagram, ["a"]), ["a", "b"],
  "one-hop selection must include the seed and direct weak neighbours only");
assert.deepEqual(neighbourNodeKeys(diagram, ["b"]), ["a", "b", "c"],
  "incoming and outgoing directed relationships must both count as neighbours");
assert.deepEqual(neighbourNodeKeys(diagram, ["a", "x"]), ["a", "b", "x", "y"],
  "multiple seeds must union their one-hop neighbourhoods");
assert.deepEqual(neighbourNodeKeys(diagram, ["isolated"]), ["isolated"]);
assert.deepEqual(neighbourNodeKeys(diagram, ["missing"]), []);
assert.equal(JSON.stringify(diagram), before, "selection analysis must not mutate the diagram");

const specialKeys = {
  nodes: [{ key: "__proto__" }, { key: "constructor" }],
  edges: [{ source: "__proto__", target: "constructor" }]
};
assert.deepEqual(connectedNodeKeys(specialKeys, ["__proto__"]), ["__proto__", "constructor"],
  "node keys must never enter an object-prototype lookup");
assert.deepEqual(neighbourNodeKeys(specialKeys, ["__proto__"]), ["__proto__", "constructor"]);

const deepNodeCount = 20000;
const deep = {
  nodes: Array.from({ length: deepNodeCount }, (_, index) => ({ key: `deep_${index}` })),
  edges: Array.from({ length: deepNodeCount - 1 }, (_, index) => ({
    source: `deep_${index}`,
    target: `deep_${index + 1}`,
    directed: true
  }))
};
const deepSelection = connectedNodeKeys(deep, ["deep_0"]);
assert.equal(deepSelection.length, deepNodeCount, "deep graphs must not overflow the call stack");
assert.equal(deepSelection.at(-1), `deep_${deepNodeCount - 1}`);
assert.deepEqual(neighbourNodeKeys(deep, ["deep_0"]), ["deep_0", "deep_1"],
  "one-hop selection must not accidentally traverse an entire deep graph");

console.log("diagram selection tests passed");
