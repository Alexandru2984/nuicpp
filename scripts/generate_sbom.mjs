#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import {
  closeSync,
  existsSync,
  lstatSync,
  mkdirSync,
  openSync,
  readFileSync,
  realpathSync,
  writeFileSync,
} from "node:fs";
import { constants as fsConstants } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.resolve(scriptDir, "..");
const outputDir = path.resolve(
  process.cwd(),
  process.argv[2] ?? path.join("build", "sbom"),
);
const nativeOutput = path.join(outputDir, "nuigraph-native.cdx.json");
const npmOutput = path.join(outputDir, "nuigraph-npm.cdx.json");
const nuiCommit = "547f34d694acfae7abf802733273113fcacddc98";

if (outputDir === rootDir) {
  throw new Error("refusing to write SBOM files over the repository root");
}
mkdirSync(outputDir, { recursive: true, mode: 0o755 });
const outputDirectoryStat = lstatSync(outputDir);
if (!outputDirectoryStat.isDirectory() || outputDirectoryStat.isSymbolicLink()) {
  throw new Error("SBOM output path must be a real directory, not a symlink");
}

function command(program, args, cwd = rootDir, stderr = "pipe") {
  return execFileSync(program, args, {
    cwd,
    encoding: "utf8",
    stdio: ["ignore", "pipe", stderr],
  }).trim();
}

function optionalCommand(program, args, cwd = rootDir) {
  try {
    return command(program, args, cwd, "ignore");
  } catch {
    return null;
  }
}

function purlPart(value) {
  return encodeURIComponent(value).replaceAll("%2F", "/");
}

function writeJsonNoFollow(filePath, value) {
  const descriptor = openSync(
    filePath,
    fsConstants.O_WRONLY |
      fsConstants.O_CREAT |
      fsConstants.O_TRUNC |
      fsConstants.O_NOFOLLOW,
    0o644,
  );
  try {
    writeFileSync(descriptor, `${JSON.stringify(value, null, 2)}\n`);
  } finally {
    closeSync(descriptor);
  }
}

function pkgConfigComponent(module, name, license, externalUrl) {
  const version = command("pkg-config", ["--modversion", module]);
  const purl = `pkg:generic/${purlPart(name)}@${purlPart(version)}`;
  return {
    "bom-ref": purl,
    type: "library",
    name,
    version,
    scope: "required",
    purl,
    licenses: [{ license: { id: license } }],
    externalReferences: [{ type: "website", url: externalUrl }],
    properties: [{ name: "nuigraph:discovery", value: `pkg-config:${module}` }],
  };
}

function linkedDebianComponents(binaryPath) {
  if (!existsSync(binaryPath)) {
    return [];
  }

  const ldd = optionalCommand("ldd", [binaryPath]);
  if (ldd === null) {
    return [];
  }

  const packageComponents = new Map();
  for (const line of ldd.split("\n")) {
    const match = line.match(/(?:=>\s+)?(\/[^\s]+)\s+\(0x[0-9a-f]+\)/i);
    if (match === null) {
      continue;
    }

    let libraryPath = match[1];
    try {
      libraryPath = realpathSync(libraryPath);
    } catch {
      continue;
    }

    const ownerOutput = optionalCommand("dpkg-query", ["-S", libraryPath]);
    if (ownerOutput === null) {
      continue;
    }
    const ownerLine = ownerOutput.split("\n")[0];
    const separator = ownerLine.indexOf(": ");
    if (separator < 1) {
      continue;
    }
    const owner = ownerLine.slice(0, separator);
    const details = optionalCommand("dpkg-query", [
      "-W",
      "-f=${binary:Package}\t${Version}\t${Architecture}",
      owner,
    ]);
    if (details === null) {
      continue;
    }
    const [binaryPackage, version, architecture] = details.split("\t");
    if (!binaryPackage || !version || !architecture) {
      continue;
    }

    const packageName = binaryPackage.replace(/:[^:]+$/, "");
    const purl = `pkg:deb/ubuntu/${purlPart(packageName)}@${purlPart(version)}?arch=${purlPart(architecture)}`;
    const existing = packageComponents.get(purl);
    if (existing !== undefined) {
      existing.properties.push({
        name: "nuigraph:linked-library",
        value: libraryPath,
      });
      continue;
    }
    packageComponents.set(purl, {
      "bom-ref": purl,
      type: "library",
      group: "ubuntu",
      name: packageName,
      version,
      scope: "required",
      purl,
      properties: [
        { name: "nuigraph:architecture", value: architecture },
        { name: "nuigraph:discovery", value: "ldd+dpkg-query" },
        { name: "nuigraph:linked-library", value: libraryPath },
      ],
    });
  }

  return [...packageComponents.values()].map((component) => ({
    ...component,
    properties: component.properties.sort((left, right) =>
      `${left.name}:${left.value}`.localeCompare(`${right.name}:${right.value}`),
    ),
  }));
}

const cmake = readFileSync(path.join(rootDir, "CMakeLists.txt"), "utf8");
const versionMatch = cmake.match(/project\(NuiGraphStudio VERSION ([0-9]+\.[0-9]+\.[0-9]+)/);
if (versionMatch === null) {
  throw new Error("could not determine the NuiGraph version from CMakeLists.txt");
}

const gitCommit = command("git", ["rev-parse", "HEAD"]);
if (!/^[0-9a-f]{40}$/.test(gitCommit)) {
  throw new Error("git rev-parse did not return a full commit identifier");
}
const dirty = command("git", ["status", "--porcelain", "--untracked-files=normal"]) !== "";
const timestamp = new Date().toISOString();
const rootPurl = `pkg:github/Alexandru2984/nuicpp@${gitCommit}`;

const directComponents = [
  pkgConfigComponent("libpqxx", "libpqxx", "BSD-3-Clause", "https://github.com/jtv/libpqxx"),
  pkgConfigComponent("libpq", "libpq", "PostgreSQL", "https://www.postgresql.org/"),
  pkgConfigComponent("cpp-httplib", "cpp-httplib", "MIT", "https://github.com/yhirose/cpp-httplib"),
  pkgConfigComponent("nlohmann_json", "nlohmann-json", "MIT", "https://github.com/nlohmann/json"),
  pkgConfigComponent("openssl", "OpenSSL", "Apache-2.0", "https://www.openssl.org/"),
  {
    "bom-ref": `pkg:github/NuiCpp/Nui@${nuiCommit}`,
    type: "framework",
    group: "NuiCpp",
    name: "Nui",
    version: nuiCommit,
    scope: "optional",
    purl: `pkg:github/NuiCpp/Nui@${nuiCommit}`,
    externalReferences: [
      { type: "vcs", url: `https://github.com/NuiCpp/Nui/tree/${nuiCommit}` },
    ],
    properties: [
      { name: "nuigraph:upstream-tag", value: "v3.5.0" },
      { name: "nuigraph:discovery", value: "pinned-cmake-fetchcontent" },
    ],
  },
];

const osComponents = linkedDebianComponents(path.join(rootDir, "build", "nuigraph-studio"));
const nativeComponents = [...directComponents, ...osComponents].sort((left, right) =>
  left["bom-ref"].localeCompare(right["bom-ref"]),
);

const nativeBom = {
  $schema: "https://cyclonedx.org/schema/bom-1.6.schema.json",
  bomFormat: "CycloneDX",
  specVersion: "1.6",
  serialNumber: `urn:uuid:${randomUUID()}`,
  version: 1,
  metadata: {
    timestamp,
    lifecycles: [{ phase: "build" }],
    component: {
      "bom-ref": rootPurl,
      type: "application",
      group: "Alexandru2984",
      name: "NuiGraph Studio",
      version: versionMatch[1],
      purl: rootPurl,
      properties: [
        { name: "nuigraph:git.commit", value: gitCommit },
        { name: "nuigraph:git.dirty", value: String(dirty) },
        {
          name: "nuigraph:linked-os-packages",
          value: String(osComponents.length),
        },
      ],
    },
  },
  components: nativeComponents,
  dependencies: [
    {
      ref: rootPurl,
      dependsOn: nativeComponents.map((component) => component["bom-ref"]),
    },
  ],
};

const npmRaw = command("npm", [
  "sbom",
  "--prefix",
  path.join(rootDir, "nui-frontend"),
  "--package-lock-only",
  "--sbom-format",
  "cyclonedx",
  "--sbom-type",
  "application",
]);
const npmBom = JSON.parse(npmRaw);
const npmPackage = JSON.parse(readFileSync(path.join(rootDir, "nui-frontend", "package.json"), "utf8"));
npmBom.metadata ??= {};
npmBom.metadata.timestamp = timestamp;
npmBom.metadata.component.name = npmPackage.name;
npmBom.metadata.component.version = npmPackage.version;
npmBom.metadata.component.description = npmPackage.description;
npmBom.metadata.properties ??= [];
npmBom.metadata.properties.push(
  { name: "nuigraph:git.commit", value: gitCommit },
  { name: "nuigraph:git.dirty", value: String(dirty) },
  { name: "nuigraph:source", value: "nui-frontend/package-lock.json" },
);

writeJsonNoFollow(nativeOutput, nativeBom);
writeJsonNoFollow(npmOutput, npmBom);

console.log(`wrote ${nativeOutput} (${nativeComponents.length} components)`);
console.log(`wrote ${npmOutput} (${npmBom.components?.length ?? 0} components)`);
