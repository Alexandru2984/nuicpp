const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const spec = JSON.parse(readFileSync(path.join(root, "public/openapi.json"), "utf8"));
const routesSource = readFileSync(path.join(root, "src/api/Routes.cpp"), "utf8");
const validatorSource = readFileSync(path.join(root, "src/validation/DiagramValidator.cpp"), "utf8");
const methods = new Set(["get", "post", "put", "delete", "patch"]);

assert.equal(spec.openapi, "3.1.0");
assert.equal(spec.info?.title, "NuiGraph Studio API");
assert.ok(spec.info?.version);
assert.ok(spec.paths && typeof spec.paths === "object");

function resolvePointer(pointer) {
  assert.match(pointer, /^#\//, `external or malformed reference: ${pointer}`);
  return pointer.slice(2).split("/").reduce((value, token) => {
    const key = token.replace(/~1/g, "/").replace(/~0/g, "~");
    assert.ok(value && Object.hasOwn(value, key), `unresolved reference: ${pointer}`);
    return value[key];
  }, spec);
}

function inspectReferences(value) {
  if (!value || typeof value !== "object") return;
  if (typeof value.$ref === "string") resolvePointer(value.$ref);
  for (const child of Object.values(value)) inspectReferences(child);
}
inspectReferences(spec);

function normaliseRoute(raw) {
  let route = raw.replaceAll("\\.json", ".json");
  if (route.includes("/templates/")) route = route.replace("([A-Za-z0-9-]+)", "{key}");
  else route = route.replace("([A-Za-z0-9-]+)", "{slug}");
  let numeric = 0;
  route = route.replace(/\(\\d\+\)/g, () => `{${numeric++ === 0 ? "id" : "versionId"}}`);
  return route;
}

const implemented = new Set();
for (const line of routesSource.split("\n")) {
  const match = line.match(/server\.(Get|Post|Put|Delete)\((?:R"\((.*)\)"|"([^"]+)")/);
  if (!match) continue;
  const raw = match[2] || match[3];
  if (raw.startsWith("/api/")) implemented.add(`${match[1].toUpperCase()} ${normaliseRoute(raw)}`);
}

const documented = new Set();
const operationIds = new Set();
for (const [route, pathItem] of Object.entries(spec.paths)) {
  const placeholders = [...route.matchAll(/\{([^}]+)\}/g)].map((match) => match[1]);
  for (const [method, operation] of Object.entries(pathItem)) {
    if (!methods.has(method)) continue;
    documented.add(`${method.toUpperCase()} ${route}`);
    assert.ok(operation.operationId, `${method.toUpperCase()} ${route} lacks operationId`);
    assert.ok(!operationIds.has(operation.operationId), `duplicate operationId: ${operation.operationId}`);
    operationIds.add(operation.operationId);
    assert.ok(operation.summary && operation.tags?.length, `${operation.operationId} lacks summary or tags`);
    assert.ok(operation.responses?.default, `${operation.operationId} lacks a default error response`);
    const parameters = [...(pathItem.parameters || []), ...(operation.parameters || [])]
      .map((parameter) => parameter.$ref ? resolvePointer(parameter.$ref) : parameter);
    for (const placeholder of placeholders) {
      assert.ok(parameters.some((parameter) => parameter.in === "path" && parameter.name === placeholder && parameter.required),
        `${operation.operationId} does not declare required path parameter ${placeholder}`);
    }
    if (["post", "put", "delete", "patch"].includes(method)) {
      assert.ok(operation.security?.length, `${operation.operationId} lacks mutation security`);
      assert.ok(operation.security.every((requirement) => requirement.csrfHeader &&
        (requirement.sessionCookie || requirement.localSessionCookie)),
      `${operation.operationId} must require CSRF and a session cookie`);
    }
  }
}

assert.deepEqual([...documented].sort(), [...implemented].sort(), "OpenAPI routes drifted from Routes.cpp");
assert.equal(operationIds.size, 21);
assert.equal(spec.paths["/api/trash"].get.responses["200"].$ref,
  "#/components/responses/TrashListResponse");
assert.ok(spec.components.schemas.TrashList.allOf.some((entry) =>
  entry.$ref === "#/components/schemas/DiagramList"));

const revisionSafe = new Set([
  "PUT /api/diagrams/{id}",
  "DELETE /api/diagrams/{id}",
  "POST /api/trash/{id}/restore",
  "DELETE /api/trash/{id}",
  "POST /api/diagrams/{id}/sharing",
  "POST /api/diagrams/{id}/restore/{versionId}"
]);
for (const entry of revisionSafe) {
  const [method, route] = entry.split(" ");
  const operation = spec.paths[route][method.toLowerCase()];
  const parameters = (operation.parameters || []).map((parameter) =>
    parameter.$ref ? resolvePointer(parameter.$ref) : parameter);
  assert.ok(parameters.some((parameter) => parameter.in === "header" && parameter.name === "If-Match" && parameter.required),
    `${entry} must require If-Match`);
}

const idempotentCreates = new Set([
  "POST /api/diagrams",
  "POST /api/templates/{key}/create",
  "POST /api/diagrams/slug/{slug}/fork",
  "POST /api/diagrams/{id}/duplicate",
  "POST /api/diagrams/import"
]);
for (const entry of idempotentCreates) {
  const [method, route] = entry.split(" ");
  const operation = spec.paths[route][method.toLowerCase()];
  const parameters = (operation.parameters || []).map((parameter) =>
    parameter.$ref ? resolvePointer(parameter.$ref) : parameter);
  const idempotency = parameters.find((parameter) =>
    parameter.in === "header" && parameter.name === "Idempotency-Key");
  assert.ok(idempotency && idempotency.required === false,
    `${entry} must declare optional Idempotency-Key`);
  assert.equal(operation.responses?.["409"]?.$ref,
    "#/components/responses/IdempotencyConflictResponse",
    `${entry} must document conflicting idempotency-key reuse`);
}
assert.equal(spec.components.headers.IdempotencyReplayed.schema.const, "true");
assert.equal(spec.components.responses.DiagramCreatedResponse.headers["Idempotency-Replayed"].$ref,
  "#/components/headers/IdempotencyReplayed");

const typeBlock = validatorSource.match(/const std::unordered_set<std::string> kTypes = \{([\s\S]*?)\};/);
assert.ok(typeBlock, "could not locate validator node types");
const expectedTypes = [...typeBlock[1].matchAll(/"([a-z]+)"/g)].map((match) => match[1]).sort();
assert.deepEqual([...spec.components.schemas.Node.properties.type.enum].sort(), expectedTypes,
  "OpenAPI node types drifted from DiagramValidator");
const trustZoneBlock = validatorSource.match(/const std::unordered_set<std::string> kTrustZones = \{([\s\S]*?)\};/);
assert.ok(trustZoneBlock, "could not locate validator trust zones");
const expectedTrustZones = [...trustZoneBlock[1].matchAll(/"([a-z]+)"/g)].map((match) => match[1]).sort();
assert.deepEqual([...spec.components.schemas.Node.properties.metadata.properties.trust_zone.enum].sort(), expectedTrustZones,
  "OpenAPI trust zones drifted from DiagramValidator");
assert.equal(spec.components.schemas.DiagramInput.properties.title.maxLength, 120);
assert.equal(spec.components.schemas.DiagramInput.properties.description.maxLength, 1000);
assert.equal(spec.components.schemas.Node.properties.title.maxLength, 160);
assert.equal(spec.components.schemas.Edge.properties.label.maxLength, 160);
assert.equal(spec.components.schemas.VersionNote.properties.note.maxLength, 240);
assert.equal(spec.components.parameters.Limit.schema.default,
  Number(routesSource.match(/kDefaultListLimit = (\d+);/)?.[1]));
assert.equal(spec.components.parameters.Limit.schema.maximum,
  Number(routesSource.match(/kMaxListLimit = (\d+);/)?.[1]));
assert.equal(spec.components.parameters.Offset.schema.maximum,
  Number(routesSource.match(/kMaxListOffset = (\d+);/)?.[1]));
assert.equal(spec.components.securitySchemes.sessionCookie.name, "__Host-ngs_session");
assert.equal(spec.components.securitySchemes.csrfHeader.name, "X-CSRF-Token");

console.log(`OpenAPI contract validated: ${operationIds.size} operations, ${documented.size} routes`);
