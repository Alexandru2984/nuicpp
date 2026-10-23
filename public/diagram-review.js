((root, factory) => {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.NuiGraphReview = api;
})(typeof globalThis === "object" ? globalThis : null, () => {
  "use strict";

  function shortName(value, fallback) {
    const text = typeof value === "string" ? value.trim() : "";
    if (!text) return fallback;
    return text.length > 60 ? `${text.slice(0, 57)}...` : text;
  }

  function analyzeDiagram(diagram) {
    const nodes = Array.isArray(diagram?.nodes) ? diagram.nodes : [];
    const edges = Array.isArray(diagram?.edges) ? diagram.edges : [];
    const findings = [];
    const add = (severity, code, message, target = null) => {
      findings.push({ severity, code, message, target });
    };

    if (!nodes.length) {
      add("error", "empty_diagram", "The diagram has no nodes.");
    }

    const nodeByKey = new Map();
    const degree = new Map();
    for (const [index, node] of nodes.entries()) {
      const hasKey = typeof node?.key === "string" && node.key !== "";
      const key = hasKey ? node.key : `node_at_${index}`;
      const target = hasKey ? { type: "node", key } : null;
      if (nodeByKey.has(key)) {
        add("error", "duplicate_node_key", `Node key "${shortName(key, "unknown")}" is duplicated.`, target);
        continue;
      }
      nodeByKey.set(key, node);
      degree.set(key, 0);
      if (typeof node?.title !== "string" || node.title.trim() === "") {
        add("warning", "missing_node_title", `Node "${shortName(key, `#${index + 1}`)}" has no title.`, target);
      }
    }

    const adjacency = new Map([...nodeByKey.keys()].map((key) => [key, []]));
    const edgeSignatures = new Map();
    for (const [index, edge] of edges.entries()) {
      const edgeKey = typeof edge?.key === "string" && edge.key ? edge.key : `edge_at_${index}`;
      const target = { type: "edge", key: edgeKey };
      const sourceExists = nodeByKey.has(edge?.source);
      const targetExists = nodeByKey.has(edge?.target);
      if (!sourceExists || !targetExists) {
        const missing = [!sourceExists ? edge?.source : null, !targetExists ? edge?.target : null]
          .filter((value) => value !== null)
          .map((value) => shortName(value, "unknown"))
          .join(", ");
        add("error", "missing_edge_endpoint", `Edge "${shortName(edgeKey, `#${index + 1}`)}" references missing node ${missing}.`, target);
        continue;
      }

      degree.set(edge.source, degree.get(edge.source) + 1);
      degree.set(edge.target, degree.get(edge.target) + 1);
      if (edge.directed) adjacency.get(edge.source).push(edge.target);

      if (typeof edge.label !== "string" || edge.label.trim() === "") {
        add("info", "missing_edge_label", `Edge "${shortName(edgeKey, `#${index + 1}`)}" has no label.`, target);
      }
      if (edge.source === edge.target) {
        add("warning", "self_loop", `Edge "${shortName(edgeKey, `#${index + 1}`)}" loops back to the same node.`, target);
      }

      const endpoints = edge.directed
        ? [edge.source, edge.target]
        : [edge.source, edge.target].sort();
      const signature = JSON.stringify([Boolean(edge.directed), endpoints[0], endpoints[1]]);
      const previous = edgeSignatures.get(signature);
      if (previous) {
        add("warning", "duplicate_edge", `Edge "${shortName(edgeKey, `#${index + 1}`)}" duplicates "${shortName(previous, "another edge")}".`, target);
      } else {
        edgeSignatures.set(signature, edgeKey);
      }
    }

    for (const [key, count] of degree) {
      if (count !== 0) continue;
      const node = nodeByKey.get(key);
      add("warning", "orphan_node", `Node "${shortName(node?.title, key)}" is not connected to any edge.`, { type: "node", key });
    }

    // Iterative Kosaraju traversal reports each directed cycle once without
    // risking a JavaScript call-stack overflow on the configured graph limit.
    const reverseAdjacency = new Map([...nodeByKey.keys()].map((key) => [key, []]));
    for (const [source, destinations] of adjacency) {
      for (const destination of destinations) reverseAdjacency.get(destination).push(source);
    }
    const visited = new Set();
    const finishOrder = [];
    for (const start of adjacency.keys()) {
      if (visited.has(start)) continue;
      visited.add(start);
      const work = [{ key: start, next: 0 }];
      while (work.length) {
        const current = work[work.length - 1];
        const neighbours = adjacency.get(current.key) || [];
        if (current.next >= neighbours.length) {
          finishOrder.push(current.key);
          work.pop();
          continue;
        }
        const neighbour = neighbours[current.next];
        current.next += 1;
        if (visited.has(neighbour)) continue;
        visited.add(neighbour);
        work.push({ key: neighbour, next: 0 });
      }
    }

    const assigned = new Set();
    for (let index = finishOrder.length - 1; index >= 0; index -= 1) {
      const start = finishOrder[index];
      if (assigned.has(start)) continue;
      assigned.add(start);
      const component = [];
      const work = [start];
      while (work.length) {
        const key = work.pop();
        component.push(key);
        for (const neighbour of reverseAdjacency.get(key) || []) {
          if (assigned.has(neighbour)) continue;
          assigned.add(neighbour);
          work.push(neighbour);
        }
      }
      if (component.length <= 1) continue;
      const names = component
        .map((nodeKey) => shortName(nodeByKey.get(nodeKey)?.title, nodeKey))
        .sort();
      add("warning", "directed_cycle", `Directed cycle: ${names.join(" -> ")}.`, {
        type: "node",
        key: component[0]
      });
    }

    const summary = { error: 0, warning: 0, info: 0, total: findings.length };
    for (const finding of findings) summary[finding.severity] += 1;
    return { summary, findings };
  }

  return Object.freeze({ analyzeDiagram });
});
