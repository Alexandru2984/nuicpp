#!/usr/bin/env node
"use strict";

const { readFileSync, statSync } = require("node:fs");
const path = require("node:path");
const { createSarifReport, ruleIds } = require("../public/diagram-review.js");

const MAX_INPUT_BYTES = 16 * 1024 * 1024;
const MAX_POLICY_BYTES = 64 * 1024;
const FAIL_LEVELS = new Set(["error", "warning", "info", "none"]);
const REVIEW_SEVERITIES = new Set(["error", "warning", "info"]);
const NAMING_CONVENTIONS = new Set(["none", "snake_case", "kebab-case", "camelCase"]);
const POLICY_FIELDS = new Set(["naming_convention", "disabled_rules", "severity_overrides"]);
const RULE_IDS = new Set(ruleIds);

function usage(stream) {
  stream.write("Usage: node scripts/review_diagram.cjs [--config policy.json] [--fail-on error|warning|info|none] <diagram.json|->\n");
}

function parseArguments(argv) {
  let input = null;
  let failOn = "error";
  let configPath = null;
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--help" || argument === "-h") return { help: true };
    if (argument === "--fail-on") {
      failOn = argv[index + 1] || "";
      index += 1;
      continue;
    }
    if (argument === "--config") {
      configPath = argv[index + 1] || "";
      index += 1;
      continue;
    }
    if (argument.startsWith("--")) throw new Error("unknown option");
    if (input !== null) throw new Error("only one input file is allowed");
    input = argument;
  }
  if (input === null) throw new Error("an input file is required");
  if (!FAIL_LEVELS.has(failOn)) throw new Error("invalid --fail-on level");
  if (configPath === "") throw new Error("--config requires a file");
  return { help: false, input, failOn, configPath };
}

function readBoundedInput(input) {
  if (input !== "-" && statSync(input).size > MAX_INPUT_BYTES) throw new Error("input exceeds the 16 MiB limit");
  const bytes = readFileSync(input === "-" ? 0 : input);
  if (bytes.length > MAX_INPUT_BYTES) throw new Error("input exceeds the 16 MiB limit");
  return bytes.toString("utf8");
}

function loadPolicy(configPath) {
  if (configPath === null) return {};
  if (statSync(configPath).size > MAX_POLICY_BYTES) throw new Error("policy exceeds the 64 KiB limit");
  const bytes = readFileSync(configPath);
  if (bytes.length > MAX_POLICY_BYTES) throw new Error("policy exceeds the 64 KiB limit");
  let value;
  try {
    value = JSON.parse(bytes.toString("utf8"));
  } catch {
    throw new Error("policy is not valid JSON");
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error("policy must be a JSON object");
  const unknownFields = Object.keys(value).filter((field) => !POLICY_FIELDS.has(field));
  if (unknownFields.length) throw new Error("policy contains an unknown field");

  const namingConvention = value.naming_convention ?? "none";
  if (!NAMING_CONVENTIONS.has(namingConvention)) throw new Error("invalid naming_convention");
  const disabledRules = value.disabled_rules ?? [];
  if (!Array.isArray(disabledRules) || disabledRules.some((ruleId) => typeof ruleId !== "string" || !RULE_IDS.has(ruleId))) {
    throw new Error("disabled_rules contains an unknown rule");
  }
  if (new Set(disabledRules).size !== disabledRules.length) throw new Error("disabled_rules must not contain duplicates");
  const severityOverrides = value.severity_overrides ?? {};
  if (!severityOverrides || typeof severityOverrides !== "object" || Array.isArray(severityOverrides) ||
      Object.entries(severityOverrides).some(([ruleId, severity]) =>
        !RULE_IDS.has(ruleId) || !REVIEW_SEVERITIES.has(severity))) {
    throw new Error("severity_overrides contains an unknown rule or severity");
  }
  return { namingConvention, disabledRules, severityOverrides };
}

function shouldFail(summary, level) {
  if (level === "none") return false;
  if (summary.error > 0) return true;
  if (level === "error") return false;
  if (summary.warning > 0) return true;
  return level === "info" && summary.info > 0;
}

function main() {
  let args;
  try {
    args = parseArguments(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`review failed: ${error.message}\n`);
    usage(process.stderr);
    return 2;
  }
  if (args.help) {
    usage(process.stdout);
    return 0;
  }

  let payload;
  let policy;
  try {
    payload = readBoundedInput(args.input);
    policy = loadPolicy(args.configPath);
  } catch (error) {
    process.stderr.write(`review failed: ${error.message}\n`);
    return 2;
  }

  let diagram;
  try {
    diagram = JSON.parse(payload);
  } catch {
    process.stderr.write("review failed: input is not valid JSON\n");
    return 2;
  }
  if (!diagram || typeof diagram !== "object" || !Array.isArray(diagram.nodes) || !Array.isArray(diagram.edges)) {
    process.stderr.write("review failed: input must contain nodes and edges arrays\n");
    return 2;
  }

  const artifactUri = args.input === "-"
    ? "diagram.json"
    : path.relative(process.cwd(), path.resolve(args.input)).replaceAll("\\", "/");
  const report = createSarifReport(diagram, { artifactUri, policy });
  const summary = report.runs[0].invocations[0].properties.summary;
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
  process.stderr.write(`NuiGraph review: ${summary.error} error(s), ${summary.warning} warning(s), ${summary.info} info finding(s)\n`);
  return shouldFail(summary, args.failOn) ? 1 : 0;
}

process.exitCode = main();
