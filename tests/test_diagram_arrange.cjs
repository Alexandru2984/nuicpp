const assert = require("node:assert/strict");
const { arrangeNodes } = require("../public/diagram-arrange.js");

function apply(nodes, updates) {
  const byKey = new Map(updates.map((update) => [update.key, update]));
  return nodes.map((node) => ({ ...node, ...(byKey.get(node.key) || {}) }));
}

function close(actual, expected, message) {
  assert(Math.abs(actual - expected) < 1e-9, `${message}: expected ${expected}, received ${actual}`);
}

const base = [
  { key: "a", x: 10, y: 20, width: 40, height: 20 },
  { key: "b", x: 100, y: 80, width: 60, height: 40 },
  { key: "c", x: 250, y: 180, width: 20, height: 60 }
];
const before = JSON.stringify(base);

let arranged = apply(base, arrangeNodes(base, ["a", "b", "c"], "align-left"));
assert.deepEqual(arranged.map((node) => node.x), [10, 10, 10]);

arranged = apply(base, arrangeNodes(base, ["a", "b", "c"], "align-center"));
for (const node of arranged) close(node.x + node.width / 2, 140, "horizontal centres must align");

arranged = apply(base, arrangeNodes(base, ["a", "b", "c"], "align-right"));
assert.deepEqual(arranged.map((node) => node.x + node.width), [270, 270, 270]);

arranged = apply(base, arrangeNodes(base, ["a", "b", "c"], "align-top"));
assert.deepEqual(arranged.map((node) => node.y), [20, 20, 20]);

arranged = apply(base, arrangeNodes(base, ["a", "b", "c"], "align-middle"));
for (const node of arranged) close(node.y + node.height / 2, 130, "vertical middles must align");

arranged = apply(base, arrangeNodes(base, ["a", "b", "c"], "align-bottom"));
assert.deepEqual(arranged.map((node) => node.y + node.height), [240, 240, 240]);

const horizontal = [
  { key: "left", x: 0, y: 0, width: 20, height: 20 },
  { key: "right", x: 190, y: 0, width: 20, height: 20 },
  { key: "middle", x: 30, y: 0, width: 40, height: 20 }
];
arranged = apply(horizontal, arrangeNodes(horizontal, ["middle", "right", "left"], "distribute-horizontal"));
assert.equal(arranged[0].x, 0, "first horizontal endpoint must stay fixed");
assert.equal(arranged[1].x, 190, "last horizontal endpoint must stay fixed");
close(arranged[2].x + arranged[2].width / 2, 105, "middle horizontal centre must be evenly distributed");

const vertical = [
  { key: "top", x: 0, y: -20, width: 20, height: 20 },
  { key: "bottom", x: 0, y: 180, width: 20, height: 40 },
  { key: "middle", x: 0, y: 10, width: 20, height: 60 }
];
arranged = apply(vertical, arrangeNodes(vertical, ["top", "bottom", "middle"], "distribute-vertical"));
close(arranged[2].y + arranged[2].height / 2, 95, "middle vertical centre must be evenly distributed");

assert.deepEqual(arrangeNodes(base, ["a"], "align-left"), [], "alignment needs two nodes");
assert.deepEqual(arrangeNodes(base, ["a", "b"], "distribute-horizontal"), [], "distribution needs three nodes");
assert.deepEqual(arrangeNodes(base, ["a", "b"], "unknown"), [], "unknown modes must fail closed");
assert.deepEqual(arrangeNodes([{ key: "bad", x: Infinity, y: 0, width: 10, height: 10 }], ["bad"], "align-left"), []);

const prototypeKeyNodes = [
  { key: "__proto__", x: 10, y: 10, width: 10, height: 10 },
  { key: "constructor", x: 30, y: 30, width: 10, height: 10 }
];
arranged = apply(prototypeKeyNodes, arrangeNodes(prototypeKeyNodes, ["__proto__", "constructor"], "align-top"));
assert.deepEqual(arranged.map((node) => node.y), [10, 10], "keys must not enter an object prototype lookup");
assert.equal(JSON.stringify(base), before, "arrangement must not mutate its input");

// Deterministic generated cases exercise varying dimensions, negative
// coordinates and input orders while checking the geometric invariants.
let seed = 0x6d2b79f5;
const random = () => {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return seed / 0x100000000;
};
for (let run = 0; run < 200; run += 1) {
  const nodes = Array.from({ length: 3 + Math.floor(random() * 18) }, (_, index) => ({
    key: `generated_${index}`,
    x: Math.round((random() - 0.5) * 2000),
    y: Math.round((random() - 0.5) * 2000),
    width: 10 + Math.round(random() * 190),
    height: 10 + Math.round(random() * 190)
  }));
  const keys = nodes.map((node) => node.key).reverse();
  const aligned = apply(nodes, arrangeNodes(nodes, keys, "align-center"));
  const expectedCenter = aligned[0].x + aligned[0].width / 2;
  for (const node of aligned) close(node.x + node.width / 2, expectedCenter, "generated centres must align");

  const distributed = apply(nodes, arrangeNodes(nodes, keys, "distribute-vertical"))
    .sort((a, b) => (a.y + a.height / 2) - (b.y + b.height / 2));
  const gap = (distributed.at(-1).y + distributed.at(-1).height / 2 -
    (distributed[0].y + distributed[0].height / 2)) / (distributed.length - 1);
  for (let index = 1; index < distributed.length; index += 1) {
    const previous = distributed[index - 1].y + distributed[index - 1].height / 2;
    const current = distributed[index].y + distributed[index].height / 2;
    close(current - previous, gap, "generated vertical gaps must match");
  }
}

console.log("diagram arrangement tests passed");
