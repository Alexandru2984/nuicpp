#!/usr/bin/env node

import { readFileSync } from "node:fs";

if (process.argv.length !== 4) {
  throw new Error("usage: validate_sbom.mjs <native.cdx.json> <npm.cdx.json>");
}

const nativeBom = JSON.parse(readFileSync(process.argv[2], "utf8"));
const npmBom = JSON.parse(readFileSync(process.argv[3], "utf8"));
const nuiCommit = "547f34d694acfae7abf802733273113fcacddc98";

function requireValue(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function validateCycloneDx(bom, label) {
  requireValue(bom.bomFormat === "CycloneDX", `${label} is not CycloneDX`);
  requireValue(/^1\.[5-9]$/.test(bom.specVersion), `${label} uses an unsupported CycloneDX version`);
  requireValue(/^urn:uuid:[0-9a-f-]{36}$/i.test(bom.serialNumber), `${label} has no UUID serial number`);
  requireValue(!Number.isNaN(Date.parse(bom.metadata?.timestamp)), `${label} has no valid timestamp`);
  requireValue(Array.isArray(bom.components), `${label} has no component list`);

  const knownRefs = new Set([bom.metadata?.component?.["bom-ref"]]);
  for (const component of bom.components) {
    requireValue(typeof component["bom-ref"] === "string", `${label} component has no bom-ref`);
    requireValue(!knownRefs.has(component["bom-ref"]), `${label} contains duplicate bom-ref ${component["bom-ref"]}`);
    requireValue(typeof component.name === "string" && component.name !== "", `${label} component has no name`);
    requireValue(typeof component.version === "string" && component.version !== "", `${label} component has no version`);
    knownRefs.add(component["bom-ref"]);

    for (const reference of component.externalReferences ?? []) {
      const url = new URL(reference.url);
      requireValue(url.username === "" && url.password === "", `${label} contains a credentialed URL`);
      requireValue(url.protocol === "https:", `${label} contains a non-HTTPS component URL`);
    }
  }

  for (const dependency of bom.dependencies ?? []) {
    requireValue(knownRefs.has(dependency.ref), `${label} has unknown dependency root ${dependency.ref}`);
    for (const ref of dependency.dependsOn ?? []) {
      requireValue(knownRefs.has(ref), `${label} depends on unknown component ${ref}`);
    }
  }
}

validateCycloneDx(nativeBom, "native SBOM");
validateCycloneDx(npmBom, "npm SBOM");

requireValue(nativeBom.specVersion === "1.6", "native SBOM must use CycloneDX 1.6");
requireValue(nativeBom.metadata.component.name === "NuiGraph Studio", "native SBOM has the wrong root component");
requireValue(nativeBom.components.length >= 6, "native SBOM is missing direct dependencies");

const nativeNames = new Set(nativeBom.components.map((component) => component.name));
for (const name of ["libpqxx", "libpq", "cpp-httplib", "nlohmann-json", "OpenSSL", "Nui"]) {
  requireValue(nativeNames.has(name), `native SBOM is missing ${name}`);
}
const nui = nativeBom.components.find((component) => component.name === "Nui");
requireValue(nui.version === nuiCommit, "native SBOM does not identify the immutable Nui commit");
requireValue(nui.purl === `pkg:github/NuiCpp/Nui@${nuiCommit}`, "native SBOM has the wrong Nui purl");

requireValue(npmBom.components.length >= 100, "npm SBOM has implausibly few locked components");
requireValue(
  npmBom.metadata.component.name === "nuigraph-nui-frontend",
  "npm SBOM has the wrong root component",
);
const npmGitProperty = npmBom.metadata.properties?.find(
  (property) => property.name === "nuigraph:git.commit",
);
requireValue(/^[0-9a-f]{40}$/.test(npmGitProperty?.value ?? ""), "npm SBOM has no source commit");

for (const [label, bom] of [["native", nativeBom], ["npm", npmBom]]) {
  const serialized = JSON.stringify(bom);
  for (const secretName of ["DATABASE_URL", "SESSION_SECRET", "AUTH_PASSWORD_HASH"]) {
    requireValue(!serialized.includes(secretName), `${label} SBOM contains configuration secret name ${secretName}`);
  }
}

console.log(
  `validated native=${nativeBom.components.length} npm=${npmBom.components.length} CycloneDX components`,
);
