const assert = require("node:assert/strict");
const { majorStepForZoom, rulerMarks } = require("../public/canvas-rulers.js");

assert.equal(majorStepForZoom(1), 100);
assert.equal(majorStepForZoom(2), 50);
assert.equal(majorStepForZoom(0.25), 500);
assert.equal(majorStepForZoom(2.5, 40), 20);
assert.equal(majorStepForZoom(0), 100);
assert.equal(majorStepForZoom(Number.NaN), 100);

const base = rulerMarks(500, 80, 1);
assert.equal(base.majorStep, 100);
assert.equal(base.minorStep, 20);
assert(base.marks.length > 0);
assert.deepEqual(base.marks.find((mark) => mark.world === 0), {
  world: 0,
  screen: 80,
  major: true,
  label: "0"
});
assert.equal(base.marks[0].screen, 0);
assert(base.marks.every((mark, index, marks) =>
  index === 0 || Math.abs(mark.screen - marks[index - 1].screen - 20) < 1e-9));

const zoomed = rulerMarks(400, 125, 2);
assert.equal(zoomed.majorStep, 50);
assert(zoomed.marks.some((mark) => mark.world < 0));
assert(zoomed.marks.some((mark) => mark.world > 0));
assert(zoomed.marks.every((mark) => mark.screen >= -1e-9 && mark.screen <= 400 + 1e-9));
assert(zoomed.marks.filter((mark) => mark.major).every((mark) => mark.label !== "-0"));

for (const zoom of [0.25, 0.5, 1, 1.75, 2.5]) {
  for (const pan of [-10000, -137.5, 0, 91, 10000]) {
    const result = rulerMarks(1920, pan, zoom);
    assert(result.marks.length <= 2048);
    assert(result.marks.every((mark) => Number.isFinite(mark.world) && Number.isFinite(mark.screen)));
    assert(result.marks.every((mark) => mark.screen >= -1e-7 && mark.screen <= 1920 + 1e-7));
  }
}

for (const args of [
  [0, 0, 1],
  [-1, 0, 1],
  [100, Number.POSITIVE_INFINITY, 1],
  [100, 0, 0],
  [100, 0, Number.NaN]
]) {
  assert.deepEqual(rulerMarks(...args).marks, []);
}

assert.deepEqual(rulerMarks(1e12, 0, 1, 0.0001).marks, [], "pathological output must be bounded");

console.log("canvas ruler tests passed");
