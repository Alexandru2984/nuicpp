((root, factory) => {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.NuiGraphSelection = api;
})(typeof globalThis === "object" ? globalThis : null, () => {
  "use strict";

  function graphIndex(diagram) {
    if (!Array.isArray(diagram?.nodes)) return null;
    const orderedKeys = [];
    const nodeKeys = new Set();
    for (const node of diagram.nodes) {
      if (typeof node?.key !== "string" || !node.key || nodeKeys.has(node.key)) continue;
      nodeKeys.add(node.key);
      orderedKeys.push(node.key);
    }

    const adjacency = new Map(orderedKeys.map((key) => [key, []]));
    const edges = Array.isArray(diagram.edges) ? diagram.edges : [];
    for (const edge of edges) {
      if (!nodeKeys.has(edge?.source) || !nodeKeys.has(edge?.target)) continue;
      adjacency.get(edge.source).push(edge.target);
      adjacency.get(edge.target).push(edge.source);
    }
    return { orderedKeys, nodeKeys, adjacency };
  }

  function validSeeds(index, seedKeys) {
    if (!index || !Array.isArray(seedKeys)) return new Set();
    return new Set(seedKeys.filter((key) => index.nodeKeys.has(key)));
  }

  // Connectivity is intentionally weak: a directed relationship still joins
  // two objects in the same visual subgraph. The iterative walk stays safe for
  // deep diagrams and returns keys in the diagram's stable display order.
  function connectedNodeKeys(diagram, seedKeys) {
    const graph = graphIndex(diagram);
    const visited = validSeeds(graph, seedKeys);
    if (!visited.size) return [];

    const queue = [...visited];
    for (let cursor = 0; cursor < queue.length; cursor += 1) {
      for (const neighbour of graph.adjacency.get(queue[cursor]) || []) {
        if (visited.has(neighbour)) continue;
        visited.add(neighbour);
        queue.push(neighbour);
      }
    }
    return graph.orderedKeys.filter((key) => visited.has(key));
  }

  function neighbourNodeKeys(diagram, seedKeys) {
    const index = graphIndex(diagram);
    const selected = validSeeds(index, seedKeys);
    if (!selected.size) return [];
    for (const seed of [...selected]) {
      for (const neighbour of index.adjacency.get(seed) || []) selected.add(neighbour);
    }
    return index.orderedKeys.filter((key) => selected.has(key));
  }

  function terminalNodeKeys(diagram, mode) {
    if (mode !== "source" && mode !== "sink") return [];
    const index = graphIndex(diagram);
    if (!index) return [];
    const incoming = new Map(index.orderedKeys.map((key) => [key, 0]));
    const outgoing = new Map(index.orderedKeys.map((key) => [key, 0]));
    const edges = Array.isArray(diagram.edges) ? diagram.edges : [];
    for (const edge of edges) {
      if (!index.nodeKeys.has(edge?.source) || !index.nodeKeys.has(edge?.target)) continue;
      outgoing.set(edge.source, outgoing.get(edge.source) + 1);
      incoming.set(edge.target, incoming.get(edge.target) + 1);
      if (!edge.directed) {
        incoming.set(edge.source, incoming.get(edge.source) + 1);
        outgoing.set(edge.target, outgoing.get(edge.target) + 1);
      }
    }
    const degree = mode === "source" ? incoming : outgoing;
    return index.orderedKeys.filter((key) => degree.get(key) === 0);
  }

  return Object.freeze({ connectedNodeKeys, neighbourNodeKeys, terminalNodeKeys });
});
