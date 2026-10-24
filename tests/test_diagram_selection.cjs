const assert = require("node:assert/strict");
const { connectedNodeKeys } = require("../public/diagram-selection.js");

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
assert.equal(JSON.stringify(diagram), before, "selection analysis must not mutate the diagram");

const specialKeys = {
  nodes: [{ key: "__proto__" }, { key: "constructor" }],
  edges: [{ source: "__proto__", target: "constructor" }]
};
assert.deepEqual(connectedNodeKeys(specialKeys, ["__proto__"]), ["__proto__", "constructor"],
  "node keys must never enter an object-prototype lookup");

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

console.log("diagram selection tests passed");
