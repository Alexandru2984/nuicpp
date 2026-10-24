((root, factory) => {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.NuiGraphSelection = api;
})(typeof globalThis === "object" ? globalThis : null, () => {
  "use strict";

  // Connectivity is intentionally weak: a directed relationship still joins
  // two objects in the same visual subgraph. The iterative walk stays safe for
  // deep diagrams and returns keys in the diagram's stable display order.
  function connectedNodeKeys(diagram, seedKeys) {
    if (!Array.isArray(diagram?.nodes) || !Array.isArray(seedKeys)) return [];

    const orderedKeys = [];
    const nodeKeys = new Set();
    for (const node of diagram.nodes) {
      if (typeof node?.key !== "string" || !node.key || nodeKeys.has(node.key)) continue;
      nodeKeys.add(node.key);
      orderedKeys.push(node.key);
    }

    const visited = new Set(seedKeys.filter((key) => nodeKeys.has(key)));
    if (!visited.size) return [];

    const adjacency = new Map(orderedKeys.map((key) => [key, []]));
    const edges = Array.isArray(diagram.edges) ? diagram.edges : [];
    for (const edge of edges) {
      if (!nodeKeys.has(edge?.source) || !nodeKeys.has(edge?.target)) continue;
      adjacency.get(edge.source).push(edge.target);
      adjacency.get(edge.target).push(edge.source);
    }

    const queue = [...visited];
    for (let index = 0; index < queue.length; index += 1) {
      for (const neighbour of adjacency.get(queue[index]) || []) {
        if (visited.has(neighbour)) continue;
        visited.add(neighbour);
        queue.push(neighbour);
      }
    }
    return orderedKeys.filter((key) => visited.has(key));
  }

  return Object.freeze({ connectedNodeKeys });
});
