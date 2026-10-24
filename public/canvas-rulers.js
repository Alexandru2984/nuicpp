(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  root.NuiGraphRulers = api;
})(typeof globalThis !== "undefined" ? globalThis : this, () => {
  "use strict";

  const NICE_FACTORS = [1, 2, 5, 10];
  const MAX_MARKS = 2048;

  function majorStepForZoom(zoom, minimumPixels = 80) {
    if (!Number.isFinite(zoom) || zoom <= 0 ||
        !Number.isFinite(minimumPixels) || minimumPixels <= 0) return 100;
    const requiredWorldUnits = minimumPixels / zoom;
    const magnitude = 10 ** Math.floor(Math.log10(requiredWorldUnits));
    for (const factor of NICE_FACTORS) {
      const candidate = factor * magnitude;
      if (candidate >= requiredWorldUnits) return candidate;
    }
    return 10 * magnitude;
  }

  function formatCoordinate(value, majorStep) {
    const normalized = Math.abs(value) < 1e-9 ? 0 : value;
    const decimals = majorStep >= 1 ? 0 : Math.min(4, Math.ceil(-Math.log10(majorStep)));
    return normalized.toFixed(decimals).replace(/\.0+$/, "");
  }

  function rulerMarks(length, pan, zoom, minimumPixels = 80) {
    const empty = { majorStep: 100, minorStep: 20, marks: [] };
    if (!Number.isFinite(length) || length <= 0 || !Number.isFinite(pan) ||
        !Number.isFinite(zoom) || zoom <= 0) return empty;

    const majorStep = majorStepForZoom(zoom, minimumPixels);
    const minorStep = majorStep / 5;
    const firstIndex = Math.ceil(((-pan / zoom) / minorStep) - 1e-9);
    const lastIndex = Math.floor((((length - pan) / zoom) / minorStep) + 1e-9);
    const count = lastIndex - firstIndex + 1;
    if (!Number.isSafeInteger(firstIndex) || !Number.isSafeInteger(lastIndex) ||
        count <= 0 || count > MAX_MARKS) {
      return { majorStep, minorStep, marks: [] };
    }

    const marks = [];
    for (let index = firstIndex; index <= lastIndex; index += 1) {
      const world = index * minorStep;
      const major = ((index % 5) + 5) % 5 === 0;
      marks.push({
        world,
        screen: pan + world * zoom,
        major,
        label: major ? formatCoordinate(world, majorStep) : ""
      });
    }
    return { majorStep, minorStep, marks };
  }

  return { majorStepForZoom, rulerMarks };
});
