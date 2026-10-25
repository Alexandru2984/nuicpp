const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const path = require("node:path");

const text = readFileSync(path.resolve(__dirname, "../public/security.txt"), "utf8");
assert.ok(text.endsWith("\n"), "security.txt must end with a newline");
assert.ok(!text.includes("\r"), "security.txt must use canonical LF line endings");

const fields = new Map();
for (const [index, line] of text.trimEnd().split("\n").entries()) {
  assert.ok(line && !/^\s/.test(line), `invalid whitespace on security.txt line ${index + 1}`);
  const separator = line.indexOf(":");
  assert.ok(separator > 0, `missing field separator on security.txt line ${index + 1}`);
  const name = line.slice(0, separator);
  const value = line.slice(separator + 1).trim();
  assert.match(name, /^[A-Za-z-]+$/, `invalid field name on line ${index + 1}`);
  assert.ok(value, `empty ${name} field`);
  if (!fields.has(name)) fields.set(name, []);
  fields.get(name).push(value);
}

assert.ok(fields.get("Contact")?.some((value) => /^mailto:[^@\s]+@[^@\s]+$/.test(value)),
  "security.txt needs a valid mailto Contact");
assert.deepEqual(fields.get("Canonical"), ["https://nuicpp.micutu.com/.well-known/security.txt"]);
assert.deepEqual(fields.get("Preferred-Languages"), ["ro, en"]);
assert.ok(fields.get("Policy")?.every((value) => value.startsWith("https://")),
  "security policy links must use HTTPS");

assert.equal(fields.get("Expires")?.length, 1, "security.txt needs exactly one Expires field");
const expiresAt = Date.parse(fields.get("Expires")[0]);
assert.ok(Number.isFinite(expiresAt), "security.txt Expires must be an ISO timestamp");
const daysRemaining = (expiresAt - Date.now()) / 86_400_000;
assert.ok(daysRemaining >= 60, `security.txt expires too soon (${Math.floor(daysRemaining)} days remain)`);
assert.ok(daysRemaining <= 370, "security.txt expiry must be reviewed at least annually");

console.log(`security.txt validated: ${Math.floor(daysRemaining)} days until renewal`);
