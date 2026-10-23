((root, factory) => {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.NuiGraphArrange = api;
})(typeof globalThis === "object" ? globalThis : null, () => {
  "use strict";

  const ALIGN_MODES = new Set([
    "align-left",
    "align-center",
    "align-right",
    "align-top",
    "align-middle",
    "align-bottom",
    "distribute-horizontal",
    "distribute-vertical"
  ]);

  function validNode(node) {
    return node && typeof node.key === "string" &&
      Number.isFinite(node.x) && Number.isFinite(node.y) &&
      Number.isFinite(node.width) && Number.isFinite(node.height) &&
      node.width > 0 && node.height > 0;
  }

  function arrangeNodes(nodes, selectedKeys, mode) {
    if (!Array.isArray(nodes) || !Array.isArray(selectedKeys) || !ALIGN_MODES.has(mode)) return [];
    const wanted = new Set(selectedKeys.filter((key) => typeof key === "string"));
    const seen = new Set();
    const selected = [];
    for (const [index, node] of nodes.entries()) {
      if (!validNode(node) || !wanted.has(node.key) || seen.has(node.key)) continue;
      seen.add(node.key);
      selected.push({ node, index, x: node.x, y: node.y });
    }

    const distributing = mode.startsWith("distribute-");
    if (selected.length < (distributing ? 3 : 2)) return [];

    const left = Math.min(...selected.map(({ node }) => node.x));
    const right = Math.max(...selected.map(({ node }) => node.x + node.width));
    const top = Math.min(...selected.map(({ node }) => node.y));
    const bottom = Math.max(...selected.map(({ node }) => node.y + node.height));

    if (mode === "align-left") {
      for (const item of selected) item.x = left;
    } else if (mode === "align-center") {
      const center = (left + right) / 2;
      for (const item of selected) item.x = center - item.node.width / 2;
    } else if (mode === "align-right") {
      for (const item of selected) item.x = right - item.node.width;
    } else if (mode === "align-top") {
      for (const item of selected) item.y = top;
    } else if (mode === "align-middle") {
      const middle = (top + bottom) / 2;
      for (const item of selected) item.y = middle - item.node.height / 2;
    } else if (mode === "align-bottom") {
      for (const item of selected) item.y = bottom - item.node.height;
    } else if (mode === "distribute-horizontal") {
      const ordered = [...selected].sort((a, b) =>
        (a.node.x + a.node.width / 2) - (b.node.x + b.node.width / 2) || a.index - b.index);
      const first = ordered[0].node.x + ordered[0].node.width / 2;
      const last = ordered[ordered.length - 1].node.x + ordered[ordered.length - 1].node.width / 2;
      const step = (last - first) / (ordered.length - 1);
      for (let index = 1; index < ordered.length - 1; index += 1) {
        ordered[index].x = first + step * index - ordered[index].node.width / 2;
      }
    } else if (mode === "distribute-vertical") {
      const ordered = [...selected].sort((a, b) =>
        (a.node.y + a.node.height / 2) - (b.node.y + b.node.height / 2) || a.index - b.index);
      const first = ordered[0].node.y + ordered[0].node.height / 2;
      const last = ordered[ordered.length - 1].node.y + ordered[ordered.length - 1].node.height / 2;
      const step = (last - first) / (ordered.length - 1);
      for (let index = 1; index < ordered.length - 1; index += 1) {
        ordered[index].y = first + step * index - ordered[index].node.height / 2;
      }
    }

    return selected
      .filter((item) => item.x !== item.node.x || item.y !== item.node.y)
      .map((item) => ({ key: item.node.key, x: item.x, y: item.y }));
  }

  return Object.freeze({ arrangeNodes });
});
