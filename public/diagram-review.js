((root, factory) => {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.NuiGraphReview = api;
})(typeof globalThis === "object" ? globalThis : null, () => {
  "use strict";

  const RULE_DEFINITIONS = Object.freeze([
    { id: "empty_diagram", severity: "error", title: "Diagram is empty", description: "The diagram must contain at least one node." },
    { id: "duplicate_node_key", severity: "error", title: "Duplicate node key", description: "Every node key must be unique within the diagram." },
    { id: "missing_node_title", severity: "warning", title: "Missing node title", description: "Every node should have a non-empty human-readable title." },
    { id: "missing_edge_endpoint", severity: "error", title: "Missing edge endpoint", description: "Every edge endpoint must reference a node in the diagram." },
    { id: "missing_edge_label", severity: "info", title: "Missing edge label", description: "Edges should describe the relationship or protocol they represent." },
    { id: "self_loop", severity: "warning", title: "Self-referencing edge", description: "An edge that returns to its source should be intentional and documented." },
    { id: "duplicate_edge", severity: "warning", title: "Duplicate edge", description: "Equivalent relationships should not be represented more than once." },
    { id: "orphan_node", severity: "warning", title: "Disconnected node", description: "Every architecture node should participate in at least one relationship." },
    { id: "directed_cycle", severity: "warning", title: "Directed dependency cycle", description: "Directed dependency cycles should be reviewed for coupling and failure propagation." },
    { id: "invalid_trust_zone", severity: "error", title: "Invalid trust zone", description: "Trust zones must use one of the supported, server-validated identifiers." },
    { id: "missing_trust_zone", severity: "warning", title: "Missing trust zone", description: "Every architecture node should be classified once trust-boundary modelling is enabled." },
    { id: "unlabelled_trust_boundary", severity: "warning", title: "Unlabelled trust-boundary crossing", description: "A relationship crossing trust zones should document its protocol or purpose." },
    { id: "direct_internet_data_flow", severity: "error", title: "Direct internet-to-data flow", description: "Internet-facing and data-tier nodes should communicate through an explicit mediated tier." },
    { id: "node_key_naming", severity: "warning", title: "Node key naming convention", description: "Node keys should follow the naming convention selected by the review policy." },
    { id: "edge_key_naming", severity: "warning", title: "Edge key naming convention", description: "Edge keys should follow the naming convention selected by the review policy." }
  ]);
  const SARIF_LEVEL = Object.freeze({ error: "error", warning: "warning", info: "note" });
  const TRUST_ZONES = new Set(["internet", "edge", "application", "data", "management"]);
  const SEVERITIES = new Set(["error", "warning", "info"]);
  const RULE_IDS = Object.freeze(RULE_DEFINITIONS.map((rule) => rule.id));
  const NAMING_CONVENTIONS = Object.freeze({
    snake_case: /^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$/,
    "kebab-case": /^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$/,
    camelCase: /^[a-z][A-Za-z0-9]*$/
  });

  function normalizePolicy(value) {
    const policy = value && typeof value === "object" && !Array.isArray(value) ? value : {};
    const namingConvention = Object.hasOwn(NAMING_CONVENTIONS, policy.namingConvention)
      ? policy.namingConvention
      : "none";
    const disabledRules = new Set(
      Array.isArray(policy.disabledRules)
        ? policy.disabledRules.filter((ruleId) => RULE_IDS.includes(ruleId))
        : []
    );
    const severityOverrides = {};
    if (policy.severityOverrides && typeof policy.severityOverrides === "object" &&
        !Array.isArray(policy.severityOverrides)) {
      for (const [ruleId, severity] of Object.entries(policy.severityOverrides)) {
        if (RULE_IDS.includes(ruleId) && SEVERITIES.has(severity)) severityOverrides[ruleId] = severity;
      }
    }
    return { namingConvention, disabledRules, severityOverrides };
  }

  function shortName(value, fallback) {
    const text = typeof value === "string" ? value.trim() : "";
    if (!text) return fallback;
    return text.length > 60 ? `${text.slice(0, 57)}...` : text;
  }

  function analyzeDiagram(diagram, options = {}) {
    const nodes = Array.isArray(diagram?.nodes) ? diagram.nodes : [];
    const edges = Array.isArray(diagram?.edges) ? diagram.edges : [];
    const findings = [];
    const policy = normalizePolicy(options);
    const add = (severity, code, message, target = null) => {
      if (policy.disabledRules.has(code)) return;
      findings.push({ severity: policy.severityOverrides[code] || severity, code, message, target });
    };

    if (!nodes.length) {
      add("error", "empty_diagram", "The diagram has no nodes.");
    }

    const nodeByKey = new Map();
    const degree = new Map();
    const trustZoneByKey = new Map();
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
      if (hasKey && policy.namingConvention !== "none" &&
          !NAMING_CONVENTIONS[policy.namingConvention].test(key)) {
        add("warning", "node_key_naming", `Node key "${shortName(key, "unknown")}" does not use ${policy.namingConvention}.`, target);
      }
      const trustZone = node?.metadata?.trust_zone;
      if (trustZone !== undefined) {
        if (typeof trustZone !== "string" || !TRUST_ZONES.has(trustZone)) {
          add("error", "invalid_trust_zone", `Node "${shortName(node?.title, key)}" has an invalid trust zone.`, target);
        } else {
          trustZoneByKey.set(key, trustZone);
        }
      }
    }

    if (trustZoneByKey.size > 0) {
      for (const [key, node] of nodeByKey) {
        if (node?.type === "note" || trustZoneByKey.has(key)) continue;
        add("warning", "missing_trust_zone", `Node "${shortName(node?.title, key)}" has no trust zone.`, {
          type: "node",
          key
        });
      }
    }

    const adjacency = new Map([...nodeByKey.keys()].map((key) => [key, []]));
    const edgeSignatures = new Map();
    for (const [index, edge] of edges.entries()) {
      const edgeKey = typeof edge?.key === "string" && edge.key ? edge.key : `edge_at_${index}`;
      const target = { type: "edge", key: edgeKey };
      const sourceExists = nodeByKey.has(edge?.source);
      const targetExists = nodeByKey.has(edge?.target);
      if (typeof edge?.key === "string" && edge.key && policy.namingConvention !== "none" &&
          !NAMING_CONVENTIONS[policy.namingConvention].test(edge.key)) {
        add("warning", "edge_key_naming", `Edge key "${shortName(edge.key, "unknown")}" does not use ${policy.namingConvention}.`, target);
      }
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
      const sourceZone = trustZoneByKey.get(edge.source);
      const targetZone = trustZoneByKey.get(edge.target);
      if (sourceZone && targetZone && sourceZone !== targetZone) {
        if (typeof edge.label !== "string" || edge.label.trim() === "") {
          add("warning", "unlabelled_trust_boundary", `Edge "${shortName(edgeKey, `#${index + 1}`)}" crosses ${sourceZone} to ${targetZone} without a label.`, target);
        }
        if ((sourceZone === "internet" && targetZone === "data") ||
            (sourceZone === "data" && targetZone === "internet")) {
          add("error", "direct_internet_data_flow", `Edge "${shortName(edgeKey, `#${index + 1}`)}" directly connects internet and data trust zones.`, target);
        }
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

  function sarifArtifactUri(value) {
    const input = typeof value === "string" ? value.trim() : "";
    const withoutControls = input.replace(/[\u0000-\u001f\u007f]/g, "").replaceAll("\\", "/");
    return withoutControls.slice(0, 2048) || "diagram.json";
  }

  function findingFingerprint(finding) {
    const target = finding.target ? `${finding.target.type}:${finding.target.key}` : "diagram";
    const text = `${finding.code}\u0000${target}\u0000${finding.message}`;
    let hash = 0x811c9dc5;
    for (let index = 0; index < text.length; index += 1) {
      hash ^= text.charCodeAt(index);
      hash = Math.imul(hash, 0x01000193);
    }
    return `v1-${(hash >>> 0).toString(16).padStart(8, "0")}`;
  }

  function createSarifReport(diagram, options = {}) {
    const policy = normalizePolicy(options.policy);
    const analysis = analyzeDiagram(diagram, options.policy);
    const artifactUri = sarifArtifactUri(options.artifactUri);
    const ruleIndexes = new Map(RULE_DEFINITIONS.map((rule, index) => [rule.id, index]));
    const rules = RULE_DEFINITIONS.map((rule) => ({
      id: rule.id,
      name: rule.id,
      shortDescription: { text: rule.title },
      fullDescription: { text: rule.description },
      defaultConfiguration: {
        level: policy.disabledRules.has(rule.id)
          ? "none"
          : SARIF_LEVEL[policy.severityOverrides[rule.id] || rule.severity]
      },
      properties: { tags: ["architecture", "quality"] }
    }));
    const results = analysis.findings.map((finding) => {
      const location = {
        physicalLocation: {
          artifactLocation: { uri: artifactUri, index: 0 },
          region: { startLine: 1 }
        }
      };
      if (finding.target) {
        location.logicalLocations = [{
          name: String(finding.target.key),
          fullyQualifiedName: `${finding.target.type}:${finding.target.key}`,
          kind: finding.target.type
        }];
      }
      return {
        ruleId: finding.code,
        ruleIndex: ruleIndexes.get(finding.code),
        level: SARIF_LEVEL[finding.severity],
        message: { text: finding.message },
        locations: [location],
        partialFingerprints: { "nuigraphFinding/v1": findingFingerprint(finding) },
        properties: finding.target ? { target: { ...finding.target } } : {}
      };
    });

    return {
      version: "2.1.0",
      $schema: "https://json.schemastore.org/sarif-2.1.0.json",
      runs: [{
        tool: {
          driver: {
            name: "NuiGraph Architecture Review",
            informationUri: "https://github.com/Alexandru2984/nuicpp",
            semanticVersion: "1.0.0",
            rules
          }
        },
        artifacts: [{ location: { uri: artifactUri }, roles: ["analysisTarget"] }],
        invocations: [{
          executionSuccessful: true,
          properties: { summary: { ...analysis.summary } }
        }],
        results
      }]
    };
  }

  return Object.freeze({ analyzeDiagram, createSarifReport, ruleIds: RULE_IDS });
});
