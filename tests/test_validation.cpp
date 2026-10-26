#include "config/Config.hpp"
#include "domain/Diagram.hpp"
#include "domain/Templates.hpp"
#include "observability/Metrics.hpp"
#include "ui/NuiApp.hpp"
#include "utils/Json.hpp"
#include "utils/RateLimiter.hpp"
#include "utils/Security.hpp"
#include "validation/DiagramValidator.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace nuigraph;

namespace {

Config testConfig() {
    Config cfg;
    cfg.databaseUrl = "postgresql://unused";
    cfg.authPasswordHash = "unused";
    cfg.maxNodesPerDiagram = 2;
    cfg.maxEdgesPerDiagram = 2;
    cfg.maxImportBytes = 1024;
    return cfg;
}

Diagram validDiagram() {
    Diagram d;
    d.title = "Test";
    d.nodes = {
        {"node_a", "process", "A", 10, 20, 160, 80, "#38bdf8", nlohmann::json::object()},
        {"node_b", "service", "B", 220, 20, 160, 80, "#22c55e", nlohmann::json::object()}
    };
    d.edges = {
        {"edge_a_b", "node_a", "node_b", "calls", true, "#94a3b8", nlohmann::json::object()}
    };
    return d;
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string tmpEnvPath() {
    return "/tmp/nuigraph-test-" + std::to_string(getpid()) + ".env";
}

// Keep syntactically valid fixtures without embedding token-shaped, high-entropy
// literals that secret scanners cannot distinguish from real credentials.
const std::string kValidPasswordHash =
    "pbkdf2_sha256$150000$" + std::string(32, 'a') + "$" + std::string(64, 'b');
const std::string kValidSigningSecret(64, 'c');
const std::string kValidCsrfSecret(64, 'd');
const std::string kValidOwnerSecret(64, 'e');
const std::string kValidRateLimitSecret(64, 'f');

std::string validSecrets() {
    return "SESSION_SIGNING_SECRET=" + kValidSigningSecret + "\n"
           "CSRF_SECRET=" + kValidCsrfSecret + "\n"
           "OWNER_HASH_SECRET=" + kValidOwnerSecret + "\n"
           "RATE_LIMIT_SECRET=" + kValidRateLimitSecret + "\n";
}

std::string validConfig(const std::string& extra = "") {
    return "APP_ENV=test\n"
           "APP_HOST=127.0.0.1\n"
           "APP_PORT=18081\n"
           "APP_BASE_URL=http://127.0.0.1:18081\n"
           "DATABASE_URL=postgresql://unused\n"
           "AUTH_USERNAME=admin\n"
           "AUTH_PASSWORD_HASH=" + kValidPasswordHash + "\n"
           + validSecrets() +
           "PROJECT_ROOT=/tmp\n" + extra;
}

void writeEnvFile(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

bool configThrows(const std::string& content) {
    const auto path = tmpEnvPath();
    writeEnvFile(path, content);
    bool threw = false;
    try {
        Config::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    std::remove(path.c_str());
    return threw;
}

// ===== Original tests =====

void testValidDiagramPasses() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    auto result = validator.validate(d, false);
    require(result.ok, "valid diagram should pass");
}

void testMissingEdgeReferenceFails() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.edges[0].target = "missing";
    auto result = validator.validate(d, false);
    require(!result.ok, "edge pointing to missing node should fail");
}

void testUnknownImportTypeMapsToNote() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[0].type = "mystery";
    auto result = validator.validate(d, true);
    require(result.ok, "unknown import type should be mapped");
    require(d.nodes[0].type == "note", "unknown import type should map to note");
}

void testUnknownRuntimeTypeFails() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[0].type = "mystery";
    auto result = validator.validate(d, false);
    require(!result.ok, "unknown runtime type should fail");
}

void testLimitsAndClamping() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes.push_back({"node_c", "note", "C", 9999999, -9999999, 10, 9999, "#fde047", nlohmann::json::array()});
    auto result = validator.validate(d, false);
    require(!result.ok, "node count limit should fail");

    d.nodes.pop_back();
    d.nodes[0].x = 9999999;
    d.nodes[0].width = 10;
    d.nodes[0].metadata = nlohmann::json::array();
    result = validator.validate(d, false);
    require(result.ok, "clampable coordinates/sizes should pass");
    require(d.nodes[0].x == 100000, "x should clamp");
    require(d.nodes[0].width == 72, "width should clamp");
    require(d.nodes[0].metadata.is_object(), "metadata should normalize to object");
}

void testStrictKeysAndColors() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[0].key = "../bad";
    auto result = validator.validate(d, false);
    require(!result.ok, "unsafe node key should fail");

    d = validDiagram();
    d.edges[0].color = "red";
    result = validator.validate(d, false);
    require(!result.ok, "non-hex edge color should fail");
}

void testJsonRoundTrip() {
    auto d = validDiagram();
    d.revision = 7;
    d.sharingEnabled = true;
    d.shareExpiresAt = "2030-01-02T03:04:05Z";
    d.shareExpired = false;
    auto parsed = diagramFromJson(diagramToJson(d));
    require(parsed.nodes.size() == 2, "nodes should round trip");
    require(parsed.edges.size() == 1, "edges should round trip");
    require(parsed.edges[0].source == "node_a", "edge source should round trip");
    require(parsed.revision == 7, "diagram revision should round trip");
    require(parsed.sharingEnabled, "sharing state should round trip");
    require(parsed.shareExpiresAt == "2030-01-02T03:04:05Z", "share expiry should round trip");
    require(!parsed.shareExpired, "active share should not round trip as expired");
}

void testFormUrlDecode() {
    auto body = std::string("username=admin&password=a%2Bb+c");
    require(formValue(body, "password") == "a+b c", "form decoding should handle percent and plus");
}

void testPublicAccessConfigLoads() {
    auto path = tmpEnvPath();
    writeEnvFile(path, validConfig("PUBLIC_ACCESS=true\n"));
    auto cfg = Config::load(path);
    require(cfg.publicAccess, "PUBLIC_ACCESS=true should enable public access");
    std::remove(path.c_str());
}

void testTemplatesValidate() {
    auto cfg = testConfig();
    cfg.maxNodesPerDiagram = 100;
    cfg.maxEdgesPerDiagram = 200;
    DiagramValidator validator(cfg);
    require(diagramTemplates().size() >= 3, "template gallery should include multiple examples");
    for (const auto& item : diagramTemplates()) {
        auto diagram = item.diagram;
        auto result = validator.validate(diagram, false);
        require(result.ok, "template should validate: " + item.key);
        require(!diagram.nodes.empty(), "template should include nodes: " + item.key);
    }
    require(findDiagramTemplate("cloud-architecture") != nullptr, "cloud architecture template should exist");
}

void testRateLimiterWindowsRequests() {
    RateLimiter limiter;
    require(limiter.allow("ip", 2, 60, 1000), "first request should pass");
    require(limiter.allow("ip", 2, 60, 1001), "second request should pass");
    require(!limiter.allow("ip", 2, 60, 1002), "third request inside window should fail");
    require(limiter.allow("ip", 2, 60, 1060), "request after window should pass");
    require(limiter.allow("other-ip", 2, 60, 1002), "separate keys should not share limits");
}

// ===== New config tests =====

void testConfigMissingEnvThrows() {
    bool threw = false;
    try {
        Config::load("/tmp/nonexistent-env-file-that-does-not-exist.env");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "loading nonexistent .env should throw");
}

void testConfigHostRestriction() {
    require(configThrows(validConfig("APP_HOST=0.0.0.0\n")),
            "APP_HOST != 127.0.0.1 should throw");
}

void testConfigMissingRequiredFields() {
    auto path = tmpEnvPath();

    // missing DATABASE_URL
    writeEnvFile(path, "APP_ENV=test\nAPP_HOST=127.0.0.1\nAUTH_PASSWORD_HASH=" +
                       kValidPasswordHash + "\n" + validSecrets());
    bool threw = false;
    try { Config::load(path); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing DATABASE_URL should throw");

    // missing AUTH_PASSWORD_HASH
    writeEnvFile(path, "APP_ENV=test\nAPP_HOST=127.0.0.1\nDATABASE_URL=postgresql://x\n" +
                       validSecrets());
    threw = false;
    try { Config::load(path); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing AUTH_PASSWORD_HASH should throw");

    // missing one of the purpose-specific secrets
    writeEnvFile(path, "APP_ENV=test\nAPP_HOST=127.0.0.1\nDATABASE_URL=postgresql://x\nAUTH_PASSWORD_HASH=" +
                       kValidPasswordHash + "\n"
                       "CSRF_SECRET=" + kValidCsrfSecret + "\n"
                       "OWNER_HASH_SECRET=" + kValidOwnerSecret + "\n"
                       "RATE_LIMIT_SECRET=" + kValidRateLimitSecret + "\n");
    threw = false;
    try { Config::load(path); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing session signing secret should throw");

    std::remove(path.c_str());
}

void testConfigQuotedValues() {
    auto path = tmpEnvPath();
    writeEnvFile(path,
        "APP_ENV=test\n"
        "APP_HOST=127.0.0.1\n"
        "DATABASE_URL=\"postgresql://quoted\"\n"
        "AUTH_PASSWORD_HASH='" + kValidPasswordHash + "'\n"
        + validSecrets() +
        "PROJECT_ROOT=/tmp\n");
    auto cfg = Config::load(path);
    require(cfg.databaseUrl == "postgresql://quoted", "double quotes should be stripped");
    require(cfg.authPasswordHash == kValidPasswordHash, "single quotes should be stripped");
    require(cfg.sessionSigningSecret == kValidSigningSecret, "unquoted values should work");
    std::remove(path.c_str());
}

void testConfigCommentAndBlankLines() {
    auto path = tmpEnvPath();
    writeEnvFile(path,
        "# This is a comment\n"
        "\n"
        "APP_ENV=test\n"
        "APP_HOST=127.0.0.1\n"
        "# Another comment\n"
        "DATABASE_URL=postgresql://test\n"
        "\n"
        "AUTH_PASSWORD_HASH=" + kValidPasswordHash + "\n"
        + validSecrets() +
        "PROJECT_ROOT=/tmp\n");
    auto cfg = Config::load(path);
    require(cfg.databaseUrl == "postgresql://test", "comments and blanks should be ignored");
    std::remove(path.c_str());
}

void testConfigEnvOverrides() {
    auto path = tmpEnvPath();
    writeEnvFile(path, validConfig("APP_PORT=9999\n"));
    setenv("APP_PORT", "7777", 1);
    auto cfg = Config::load(path);
    unsetenv("APP_PORT");
    require(cfg.port == 7777, "env var should override file value");
    std::remove(path.c_str());
}

void testConfigRejectsAmbiguousValues() {
    require(configThrows(validConfig("PUBLIC_ACCESS=truthy\n")),
            "unknown boolean spellings should throw");
    require(configThrows(validConfig("APP_PORT=18081oops\n")),
            "integer values with trailing data should throw");
}

void testConfigRejectsUnsafeLimits() {
    require(configThrows(validConfig("MAX_IMPORT_BYTES=-1\n")),
            "negative import limits must not wrap to a huge size_t");
    require(configThrows(validConfig("MAX_DIAGRAMS_TOTAL=0\n")),
            "global diagram capacity must be positive");
    require(configThrows(validConfig("MAX_DIAGRAMS_TOTAL=1000001\n")),
            "global diagram capacity must have an operational upper bound");
    require(configThrows(validConfig("MAX_LOGICAL_BYTES_TOTAL=-1\n")),
            "negative logical storage limits must not wrap");
    require(configThrows(validConfig("MAX_LOGICAL_BYTES_TOTAL=1099511627777\n")),
            "logical storage must have an operational upper bound");
    require(configThrows(validConfig(
        "MAX_LOGICAL_BYTES_PER_GUEST=4096\nMAX_LOGICAL_BYTES_TOTAL=2048\n")),
        "per-guest logical capacity cannot exceed global capacity");
    require(configThrows(validConfig("DB_POOL_SIZE=1000\n")),
            "excessive database pools should throw");
}

void testConfigRejectsWeakSecrets() {
    require(!configThrows(validConfig(
        "SESSION_SIGNING_SECRET_PREVIOUS=" + std::string(64, '9') + "\n")),
        "a distinct previous signing key should be accepted during rotation");
    require(!configThrows(validConfig(
        "SESSION_SIGNING_SECRET_PREVIOUS=" + kValidOwnerSecret + "\n")),
        "the legacy signing key may equal the stable owner key during migration");
    require(configThrows(validConfig("SESSION_SIGNING_SECRET=abcd\n")),
            "short signing secrets should throw");
    require(configThrows(validConfig("CSRF_SECRET=" + kValidSigningSecret + "\n")),
            "reused purpose-specific secrets should throw");
    require(configThrows(validConfig("SESSION_SECRET=" + kValidSigningSecret + "\n")),
            "legacy shared secrets should fail closed");
    require(configThrows(validConfig(
        "SESSION_SIGNING_SECRET_PREVIOUS=" + kValidSigningSecret + "\n")),
        "the previous signing key must differ from the current signing key");
    require(configThrows(validConfig("AUTH_PASSWORD_HASH=pbkdf2_sha256$1$aa$bb\n")),
            "weak password hashes should throw");
}

void testProductionConfigRequiresSafePathsAndHttps() {
    require(configThrows(validConfig("APP_ENV=production\nAPP_BASE_URL=http://example.test\n")),
            "production base URL must use HTTPS");
    require(configThrows(validConfig("APP_BASE_URL=https://user@example.test\n")),
            "base URL credentials must be rejected");
    require(configThrows(validConfig("APP_BASE_URL=https://example.test/path\n")),
            "base URL paths must be rejected");
    require(configThrows(validConfig("APP_BASE_URL=https://example.test?query=1\n")),
            "base URL queries must be rejected");
    require(configThrows(validConfig("APP_BASE_URL=ftp://example.test\n")),
            "non-HTTP base URL schemes must be rejected");
    require(configThrows(validConfig("APP_BASE_URL=https://example.test:0\n")),
            "invalid base URL ports must be rejected");
    require(configThrows(validConfig("PROJECT_ROOT=relative/path\n")),
            "project root must be absolute");

    const auto path = tmpEnvPath();
    writeEnvFile(path, validConfig("APP_BASE_URL=HTTP://Example.Test:80/\n"));
    const auto cfg = Config::load(path);
    std::remove(path.c_str());
    require(cfg.baseUrl == "http://example.test",
            "base URLs should be stored as canonical origins");
}

void testFrontendShellDefaultsAndRejectsUnknownModes() {
    auto path = tmpEnvPath();
    writeEnvFile(path, validConfig());
    const auto cfg = Config::load(path);
    std::remove(path.c_str());
    require(cfg.frontendShell == FrontendShell::Server,
            "server-rendered frontend should be the safe default");
    require(std::string(frontendShellName(cfg.frontendShell)) == "server",
            "frontend shell should have a stable log name");
    require(configThrows(validConfig("FRONTEND_SHELL=automatic\n")),
            "unknown frontend shell must not silently fall back");
}

void testNuiShellRequiresACompleteRegularBundle() {
    namespace fs = std::filesystem;
    const auto root = fs::path("/tmp") /
        ("nuigraph-nui-config-test-" + std::to_string(getpid()));
    const auto bundle = root / "public" / "nui";
    fs::remove_all(root);
    fs::create_directories(bundle);
    writeEnvFile((bundle / "index.html").string(), "<!doctype html>");
    writeEnvFile((bundle / "index.js").string(), "// loader");

    const auto selected = "PROJECT_ROOT=" + root.string() + "\nFRONTEND_SHELL=nui\n";
    require(configThrows(validConfig(selected)),
            "Nui mode must reject a loader without a WebAssembly module");

    writeEnvFile((bundle / "index.wasm").string(), "wasm fixture");
    auto path = tmpEnvPath();
    writeEnvFile(path, validConfig(selected));
    const auto cfg = Config::load(path);
    std::remove(path.c_str());
    require(cfg.frontendShell == FrontendShell::Nui,
            "a complete regular Nui bundle should be selectable");

    fs::remove(bundle / "index.wasm");
    writeEnvFile((bundle / "module.bin").string(), "wasm fixture");
    fs::create_symlink(bundle / "module.bin", bundle / "index.wasm");
    require(configThrows(validConfig(selected)),
            "Nui mode must reject symlinked release artifacts");
    fs::remove_all(root);
}

void testMetricsStayAggregateAndBounded() {
    Metrics metrics;
    metrics.observeHttp("GET", 200, 0.02);
    metrics.observeHttp("PATCH", 503, 2.0);
    metrics.recordSecurityEvent("admin_login", "failed");
    metrics.recordSecurityEvent("fetch_metadata_rejected", "blocked");
    metrics.recordSecurityEvent("origin_rejected", "blocked");
    metrics.recordSecurityEvent("unsupported_media_type", "blocked");
    metrics.recordSecurityEvent("session_cookie_upgrade", "succeeded");
    metrics.recordSecurityEvent("idempotency_rejected", "blocked");
    metrics.recordSecurityEvent("idempotency_conflict", "blocked");
    metrics.recordSecurityEvent("diagram_trash", "moved");
    metrics.recordSecurityEvent("diagram_trash", "restored");
    metrics.recordSecurityEvent("diagram_trash", "permanently_deleted");
    metrics.recordSecurityEvent("attacker-controlled-label", "ignored");
    metrics.recordDiagramCreated("import");
    metrics.recordDiagramCreated("attacker-controlled-source");
    metrics.recordReadiness(true);

    OperationalMetrics operational;
    operational.databaseUp = true;
    operational.diagrams = 7;
    operational.trashedDiagrams = 2;
    operational.nodes = 21;
    operational.logicalBytes = 2048;
    operational.logicalByteCapacity = 8192;
    operational.databaseBytes = 4096;
    operational.poolMax = 8;
    operational.poolIdle = 3;
    operational.poolOutstanding = 1;
    operational.poolTimeouts = 2;

    const auto body = metrics.render(FrontendShell::Server, operational);
    require(body.find("nuigraph_build_info{frontend=\"server\"} 1") != std::string::npos,
            "metrics should identify the selected shell");
    require(body.find("method=\"GET\",status=\"2xx\"} 1") != std::string::npos,
            "metrics should count successful GET requests");
    require(body.find("method=\"OTHER\",status=\"5xx\"} 1") != std::string::npos,
            "metrics should collapse unknown methods into a bounded label");
    require(body.find("event=\"admin_login\",outcome=\"failed\"} 1") != std::string::npos,
            "metrics should count known security outcomes");
    require(body.find("event=\"fetch_metadata_rejected\",outcome=\"blocked\"} 1") !=
                std::string::npos,
            "metrics should count rejected cross-site mutations");
    require(body.find("event=\"origin_rejected\",outcome=\"blocked\"} 1") !=
                std::string::npos,
            "metrics should count rejected request origins");
    require(body.find("event=\"unsupported_media_type\",outcome=\"blocked\"} 1") !=
                std::string::npos,
            "metrics should count rejected request media types");
    require(body.find("event=\"session_cookie_upgrade\",outcome=\"succeeded\"} 1") !=
                std::string::npos,
            "metrics should count legacy session-cookie upgrades");
    require(body.find("event=\"idempotency_rejected\",outcome=\"blocked\"} 1") !=
                std::string::npos,
            "metrics should count malformed idempotency keys");
    require(body.find("event=\"idempotency_conflict\",outcome=\"blocked\"} 1") !=
                std::string::npos,
            "metrics should count conflicting idempotency-key reuse");
    require(body.find("event=\"diagram_trash\",outcome=\"moved\"} 1") !=
                std::string::npos,
            "metrics should count soft deletion");
    require(body.find("event=\"diagram_trash\",outcome=\"restored\"} 1") !=
                std::string::npos,
            "metrics should count trash restoration");
    require(body.find("event=\"diagram_trash\",outcome=\"permanently_deleted\"} 1") !=
                std::string::npos,
            "metrics should count permanent deletion");
    require(body.find("nuigraph_storage_objects{kind=\"trashed_diagrams\"} 2") !=
                std::string::npos,
            "metrics should expose the aggregate trash backlog");
    require(body.find("source=\"import\"} 1") != std::string::npos,
            "metrics should count successful import creations");
    require(body.find("nuigraph_database_size_bytes 4096") != std::string::npos,
            "metrics should include aggregate database size");
    require(body.find("nuigraph_logical_storage_bytes 2048") != std::string::npos,
            "metrics should expose aggregate logical storage usage");
    require(body.find("nuigraph_logical_storage_capacity_bytes 8192") != std::string::npos,
            "metrics should expose configured logical storage capacity");
    require(body.find("attacker-controlled") == std::string::npos,
            "network-derived labels must never create metric cardinality");
}

// ===== New validation edge case tests =====

void testEmptyTitleFails() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.title = "";
    auto result = validator.validate(d, false);
    require(!result.ok, "empty title should fail");
}

void testDuplicateNodeKeysFails() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[1].key = d.nodes[0].key;
    auto result = validator.validate(d, false);
    require(!result.ok, "duplicate node keys should fail");
}

void testDuplicateEdgeKeysFails() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.edges.push_back({"edge_a_b", "node_b", "node_a", "back", true, "#94a3b8", nlohmann::json::object()});
    auto result = validator.validate(d, false);
    require(!result.ok, "duplicate edge keys should fail");
}

void testSelfLoopEdgePasses() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.edges[0].source = "node_a";
    d.edges[0].target = "node_a";
    auto result = validator.validate(d, false);
    require(result.ok, "self-loop edge should pass");
}

void testEdgeLimitExceeded() {
    auto cfg = testConfig();
    cfg.maxEdgesPerDiagram = 1;
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.edges.push_back({"edge_b_a", "node_b", "node_a", "back", true, "#94a3b8", nlohmann::json::object()});
    auto result = validator.validate(d, false);
    require(!result.ok, "too many edges should fail");
}

void testTitleTruncation() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.title = std::string(200, 'X');
    auto result = validator.validate(d, false);
    require(result.ok, "long title should be truncated, not rejected");
    require(d.title.size() == 120, "title should truncate to 120 chars");
}

void testDescriptionTruncation() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.description = std::string(1500, 'D');
    auto result = validator.validate(d, false);
    require(result.ok, "long description should be truncated");
    require(d.description.size() == 1000, "description should truncate to 1000 chars");
}

void testNodeTitleTruncation() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[0].title = std::string(300, 'N');
    auto result = validator.validate(d, false);
    require(result.ok, "long node title should be truncated");
    require(d.nodes[0].title.size() == 160, "node title should truncate to 160 chars");
}

void testInfiniteCoordinatesClamped() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[0].x = std::numeric_limits<double>::infinity();
    d.nodes[0].y = -std::numeric_limits<double>::infinity();
    d.nodes[1].x = std::numeric_limits<double>::quiet_NaN();
    auto result = validator.validate(d, false);
    require(result.ok, "infinite/NaN coords should be clamped");
    require(d.nodes[0].x == 0, "Inf x should clamp to 0");
    require(d.nodes[0].y == 0, "-Inf y should clamp to 0");
    require(d.nodes[1].x == 0, "NaN x should clamp to 0");
}

void testNodeWidthHeightClamping() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[0].width = 10;
    d.nodes[0].height = 900;
    d.nodes[1].width = 1000;
    d.nodes[1].height = 20;
    auto result = validator.validate(d, false);
    require(result.ok, "extreme width/height should be clamped");
    require(d.nodes[0].width == 72, "width < 72 should clamp to 72");
    require(d.nodes[0].height == 400, "height > 400 should clamp to 400");
    require(d.nodes[1].width == 600, "width > 600 should clamp to 600");
    require(d.nodes[1].height == 48, "height < 48 should clamp to 48");
}

void testNonObjectMetadataNormalized() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    auto d = validDiagram();
    d.nodes[0].metadata = nlohmann::json::array();
    d.nodes[1].metadata = "string";
    d.edges[0].metadata = 42;
    auto result = validator.validate(d, false);
    require(result.ok, "non-object metadata should be normalized");
    require(d.nodes[0].metadata.is_object(), "array metadata should become object");
    require(d.nodes[1].metadata.is_object(), "string metadata should become object");
    require(d.edges[0].metadata.is_object(), "int metadata should become object");
}

void testTrustZoneValidation() {
    auto cfg = testConfig();
    DiagramValidator validator(cfg);
    for (const auto& zone : {"internet", "edge", "application", "data", "management"}) {
        auto d = validDiagram();
        d.nodes[0].metadata["trust_zone"] = zone;
        require(validator.validate(d, false).ok, "known trust zone should pass: " + std::string(zone));
    }

    auto invalidName = validDiagram();
    invalidName.nodes[0].metadata["trust_zone"] = "dmz";
    require(!validator.validate(invalidName, false).ok, "unknown trust zone should fail");

    auto invalidType = validDiagram();
    invalidType.nodes[0].metadata["trust_zone"] = 42;
    require(!validator.validate(invalidType, true).ok, "non-string trust zone should fail on import");

    auto invalidLock = validDiagram();
    invalidLock.nodes[0].metadata["geometry_locked"] = "yes";
    require(!validator.validate(invalidLock, false).ok, "non-boolean geometry lock should fail");
}

void testAllNodeTypesAccepted() {
    auto cfg = testConfig();
    cfg.maxNodesPerDiagram = 100;
    DiagramValidator validator(cfg);
    std::vector<std::string> types = {
        "process", "decision", "database", "service", "api", "note", "external",
        "queue", "cache", "container", "cloud"
    };
    for (const auto& t : types) {
        auto d = validDiagram();
        d.nodes[0].type = t;
        auto result = validator.validate(d, false);
        require(result.ok, "node type " + t + " should be accepted");
    }
}

// ===== New JSON/utility tests =====

void testUrlDecodeEdgeCases() {
    require(urlDecode("") == "", "empty string should decode to empty");
    require(urlDecode("hello") == "hello", "plain text should pass through");
    require(urlDecode("trailing%") == "trailing%", "trailing % should be preserved");
    // urlDecode silently drops % when followed by invalid hex chars
    require(urlDecode("bad%ZZ") == "badZZ", "invalid hex drops the percent sign");
}

void testFormValueMissingKey() {
    require(formValue("a=b&c=d", "x") == "", "missing key should return empty");
}

void testFormValueMultipleKeys() {
    require(formValue("a=1&b=2&c=3", "b") == "2", "correct value for middle key");
    require(formValue("x=10", "x") == "10", "single key extraction");
    require(formValue("", "a") == "", "empty body returns empty");
}

void testHtmlEscapeSpecialChars() {
    require(htmlEscape("&") == "&amp;", "ampersand should be escaped");
    require(htmlEscape("<") == "&lt;", "less-than should be escaped");
    require(htmlEscape(">") == "&gt;", "greater-than should be escaped");
    require(htmlEscape("\"") == "&quot;", "double quote should be escaped");
    require(htmlEscape("'") == "&#39;", "single quote should be escaped");
    require(htmlEscape("safe text") == "safe text", "safe text should pass through");
    require(htmlEscape("<script>alert('xss')</script>") == "&lt;script&gt;alert(&#39;xss&#39;)&lt;/script&gt;",
            "XSS payload should be fully escaped");
}

void testJsonRoundTripEmptyDiagram() {
    Diagram d;
    d.title = "Empty";
    d.description = "No nodes or edges";
    auto json = diagramToJson(d);
    auto parsed = diagramFromJson(json);
    require(parsed.title == "Empty", "title should survive round trip");
    require(parsed.description == "No nodes or edges", "description should survive round trip");
    require(parsed.nodes.empty(), "empty nodes should survive round trip");
    require(parsed.edges.empty(), "empty edges should survive round trip");
}

void testJsonRoundTripPreservesMetadata() {
    auto d = validDiagram();
    d.nodes[0].metadata = {{"key", "value"}, {"count", 42}};
    d.edges[0].metadata = {{"weight", 3.14}};
    auto parsed = diagramFromJson(diagramToJson(d));
    require(parsed.nodes[0].metadata["key"] == "value", "node metadata key should survive");
    require(parsed.nodes[0].metadata["count"] == 42, "node metadata count should survive");
    require(parsed.edges[0].metadata["weight"] == 3.14, "edge metadata weight should survive");
}

// ===== New RateLimiter tests =====

void testRateLimiterClear() {
    RateLimiter limiter;
    limiter.allow("ip", 1, 60, 1000);
    require(!limiter.allow("ip", 1, 60, 1001), "should be rate limited");
    limiter.clear();
    require(limiter.allow("ip", 1, 60, 1002), "after clear, should allow again");
}

void testRateLimiterClearsOneKey() {
    RateLimiter limiter;
    limiter.allow("login:a", 1, 60, 1000);
    limiter.allow("login:b", 1, 60, 1000);
    limiter.clear("login:a");
    require(limiter.allow("login:a", 1, 60, 1001), "cleared key should be allowed again");
    require(!limiter.allow("login:b", 1, 60, 1001), "other key must remain limited");
}

void testSensitivePathsAreRedacted() {
    require(redactRequestPath("/d/private-capability-123") == "/d/<redacted>",
            "public share capability should be redacted");
    require(redactRequestPath("/api/diagrams/slug/private-capability-123") ==
                "/api/diagrams/slug/<redacted>",
            "API share capability should be redacted");
    require(redactRequestPath("/api/diagrams/42") == "/api/diagrams/42",
            "ordinary route should be preserved");
    require(redactRequestPath("/d/") == "/d/", "empty share route should be preserved");
    require(redactRequestPath("/d/token/extra") == "/d/token/extra",
            "non-matching nested path should be preserved");
}

void testRequestIdsAndLogFieldsAreSafe() {
    require(normalizeRequestId("0123456789abcdef") == "0123456789abcdef",
            "valid proxy request ids should be preserved");
    require(normalizeRequestId("short").empty(), "short request ids should be rejected");
    require(normalizeRequestId("request-id-with-newline\n").empty(),
            "control characters must invalidate request ids");
    const auto generated = generateRequestId();
    require(normalizeRequestId(generated) == generated,
            "generated request ids should satisfy the accepted format");
    require(sanitizeLogField("ok\nforged=value") == "ok_forged_value",
            "log fields must not permit line or key injection");
}

void testFetchMetadataMutationPolicy() {
    require(!shouldRejectCrossSiteMutation("GET", "cross-site"),
            "cross-site reads should remain available");
    require(!shouldRejectCrossSiteMutation("HEAD", "same-site"),
            "cross-site HEAD requests should remain available");
    require(!shouldRejectCrossSiteMutation("OPTIONS", "cross-site"),
            "preflight requests should reach normal routing");
    require(!shouldRejectCrossSiteMutation("POST", ""),
            "API clients without Fetch Metadata should remain compatible");
    require(!shouldRejectCrossSiteMutation(" post ", " Same-Origin "),
            "same-origin browser mutations should be accepted case-insensitively");
    require(shouldRejectCrossSiteMutation("POST", "cross-site"),
            "cross-site form submissions should be rejected");
    require(shouldRejectCrossSiteMutation("PUT", "same-site"),
            "sibling origins should not be trusted with writes");
    require(shouldRejectCrossSiteMutation("DELETE", "none"),
            "unsafe browser requests need an explicit same-origin signal");
    require(shouldRejectCrossSiteMutation("PATCH", "malformed"),
            "unknown Fetch Metadata values should fail closed");
}

void testContentTypeMatching() {
    require(contentTypeMatches("application/json", "application/json"),
            "exact JSON media types should match");
    require(contentTypeMatches(" Application/JSON ; charset=utf-8 ", "application/json"),
            "media type matching should ignore case, surrounding whitespace and parameters");
    require(contentTypeMatches("application/x-www-form-urlencoded; charset=UTF-8",
                               "application/x-www-form-urlencoded"),
            "form media types should allow standard parameters");
    require(!contentTypeMatches("", "application/json"),
            "missing content types should not match");
    require(!contentTypeMatches("application/jsonp", "application/json"),
            "media type prefixes should not match");
    require(!contentTypeMatches("application/ld+json", "application/json"),
            "undeclared structured JSON types should not match");
    require(!contentTypeMatches("application/json, text/plain", "application/json"),
            "comma-joined content types should fail closed");
}

void testRequestOriginPolicy() {
    require(canonicalHttpOrigin("HTTPS://Example.Test:443/") == "https://example.test",
            "origins should normalize scheme, host, default port and trailing slash");
    require(canonicalHttpOrigin("http://127.0.0.1:18123") ==
                "http://127.0.0.1:18123",
            "non-default ports should be retained");
    require(canonicalHttpOrigin("http://[0:0:0:0:0:0:0:1]:8080") ==
                "http://[::1]:8080",
            "IPv6 origins should be validated and canonicalized");
    for (const auto& invalid : {
             "null", "ftp://example.test", "https://user@example.test",
             "https://example.test/path", "https://example.test?x=1",
             "https://example.test:0", "https://example.test:",
             "https://999.999.999.999", "https://bad..example"}) {
        require(canonicalHttpOrigin(invalid).empty(),
                "malformed or non-origin URL should be rejected: " + std::string(invalid));
    }

    constexpr auto expected = "https://example.test";
    require(!shouldRejectRequestOrigin("GET", "https://evil.test", 1, expected),
            "safe reads should not be origin-gated");
    require(!shouldRejectRequestOrigin("POST", "", 0, expected),
            "API clients without Origin should remain compatible");
    require(!shouldRejectRequestOrigin("POST", "HTTPS://EXAMPLE.TEST:443", 1, expected),
            "the canonical application origin should be accepted");
    require(shouldRejectRequestOrigin("POST", "https://evil.test", 1, expected),
            "foreign origins should be rejected");
    require(shouldRejectRequestOrigin("PUT", "null", 1, expected),
            "opaque origins should be rejected");
    require(shouldRejectRequestOrigin("DELETE", expected, 2, expected),
            "duplicate Origin headers should fail closed");
}

void testRenderedPagesVersionStaticAssets() {
    constexpr auto version = "0123456789abcdef";
    const auto editor = renderEditorPage("guest", "https://example.test", true, version);
    require(editor.find("/styles.css?v=0123456789abcdef") != std::string::npos,
            "editor stylesheet should use the release asset version");
    require(editor.find("/app.js?v=0123456789abcdef") != std::string::npos,
            "editor bootstrap should use the release asset version");
    require(renderLoginPage("", version).find("/styles.css?v=0123456789abcdef") != std::string::npos,
            "login stylesheet should use the release asset version");
    require(renderDocsPage("https://example.test", version).find(
                "/styles.css?v=0123456789abcdef") != std::string::npos,
            "docs stylesheet should use the release asset version");
}

void testRateLimiterSlidingWindow() {
    RateLimiter limiter;
    limiter.allow("ip", 3, 10, 100);
    limiter.allow("ip", 3, 10, 103);
    limiter.allow("ip", 3, 10, 106);
    require(!limiter.allow("ip", 3, 10, 109), "window full at t=109");
    // at t=110, the first request (t=100) should have expired
    require(limiter.allow("ip", 3, 10, 110), "first request should have expired at t=110");
}

void testRateLimiterMultipleKeys() {
    RateLimiter limiter;
    limiter.allow("a", 1, 60, 100);
    require(!limiter.allow("a", 1, 60, 101), "key 'a' should be limited");
    require(limiter.allow("b", 1, 60, 101), "key 'b' should be independent");
    require(limiter.allow("c", 1, 60, 101), "key 'c' should be independent");
}

// Keys arrive from the network, so the map must not grow without bound.
void testRateLimiterEvictsExpiredKeys() {
    RateLimiter limiter;
    const std::size_t flood = RateLimiter::kMaxTrackedKeys + 500;
    for (std::size_t i = 0; i < flood; ++i) {
        limiter.allow("ip-" + std::to_string(i), 5, 60, 1000);
    }
    require(limiter.trackedKeys() <= RateLimiter::kMaxTrackedKeys,
            "tracked keys should stay at or below the cap while filling");

    // Well past the window: the next call sweeps the stale entries away.
    limiter.allow("fresh", 5, 60, 100000);
    require(limiter.trackedKeys() < RateLimiter::kMaxTrackedKeys,
            "expired keys should be evicted once the window passes");
}

// Eviction resets whichever counter it drops, so it must not be usable as a way
// to clear an existing throttle by flooding the map with fresh keys.
void testRateLimiterEvictionKeepsThrottledKeys() {
    RateLimiter limiter;
    for (int i = 0; i < 3; ++i) {
        limiter.allow("throttled", 3, 60, 1000 + i);
    }
    require(!limiter.allow("throttled", 3, 60, 1004), "key should be at its limit before the flood");

    for (std::size_t i = 0; i < RateLimiter::kMaxTrackedKeys + 100; ++i) {
        limiter.allow("flood-" + std::to_string(i), 5, 60, 1005);
    }

    require(limiter.trackedKeys() <= RateLimiter::kMaxTrackedKeys, "map should stay bounded through the flood");
    require(!limiter.allow("throttled", 3, 60, 1006),
            "a throttled key should survive eviction and stay limited");
}

void testSeoMetadata() {
    const auto editor = renderEditorPage("guest", "https://example.test/");
    require(editor.find("rel=\"canonical\" href=\"https://example.test/\"") != std::string::npos,
            "public editor should have one normalized canonical URL");
    require(editor.find("property=\"og:title\"") != std::string::npos,
            "public editor should carry Open Graph metadata");
    require(editor.find("name=\"twitter:card\"") != std::string::npos,
            "public editor should carry Twitter metadata");
    require(editor.find("itemtype=\"https://schema.org/SoftwareApplication\"") != std::string::npos,
            "public editor should expose SoftwareApplication microdata");
    require(editor.find("itemprop=\"applicationCategory\" content=\"DesignApplication\"") != std::string::npos,
            "public editor should classify the application");
    require(editor.find("property=\"og:locale\" content=\"en_US\"") != std::string::npos,
            "public editor should identify its Open Graph locale");
    require(editor.find("<h1>NuiGraph Studio</h1>") != std::string::npos,
            "public editor should have one semantic primary heading");

    const auto privateShare = renderEditorPage("guest", "https://example.test", false);
    require(privateShare.find("noindex, nofollow, noarchive") != std::string::npos,
            "bearer share page should opt out of indexing");
    require(privateShare.find("rel=\"canonical\"") == std::string::npos,
            "bearer share page should not advertise a canonical URL");
    require(privateShare.find("schema.org/SoftwareApplication") == std::string::npos,
            "bearer share page should not advertise public application microdata");
    require(privateShare.find("itemprop=") == std::string::npos,
            "bearer share page should omit structured application properties");

    const auto docs = renderDocsPage("https://example.test/");
    require(docs.find("rel=\"canonical\" href=\"https://example.test/docs\"") != std::string::npos,
            "documentation should have a normalized canonical URL");
    require(renderLoginPage("").find("noindex, nofollow, noarchive") != std::string::npos,
            "login page should opt out of indexing");
}

} // namespace

int main() {
    try {
        // original tests
        testValidDiagramPasses();
        testMissingEdgeReferenceFails();
        testUnknownImportTypeMapsToNote();
        testUnknownRuntimeTypeFails();
        testLimitsAndClamping();
        testStrictKeysAndColors();
        testJsonRoundTrip();
        testFormUrlDecode();
        testPublicAccessConfigLoads();
        testTemplatesValidate();
        testRateLimiterWindowsRequests();
        // config tests
        testConfigMissingEnvThrows();
        testConfigHostRestriction();
        testConfigMissingRequiredFields();
        testConfigQuotedValues();
        testConfigCommentAndBlankLines();
        testConfigEnvOverrides();
        testConfigRejectsAmbiguousValues();
        testConfigRejectsUnsafeLimits();
        testConfigRejectsWeakSecrets();
        testProductionConfigRequiresSafePathsAndHttps();
        testFrontendShellDefaultsAndRejectsUnknownModes();
        testNuiShellRequiresACompleteRegularBundle();
        testMetricsStayAggregateAndBounded();
        // validation edge cases
        testEmptyTitleFails();
        testDuplicateNodeKeysFails();
        testDuplicateEdgeKeysFails();
        testSelfLoopEdgePasses();
        testEdgeLimitExceeded();
        testTitleTruncation();
        testDescriptionTruncation();
        testNodeTitleTruncation();
        testInfiniteCoordinatesClamped();
        testNodeWidthHeightClamping();
        testNonObjectMetadataNormalized();
        testTrustZoneValidation();
        testAllNodeTypesAccepted();
        // json/utility tests
        testUrlDecodeEdgeCases();
        testFormValueMissingKey();
        testFormValueMultipleKeys();
        testHtmlEscapeSpecialChars();
        testJsonRoundTripEmptyDiagram();
        testJsonRoundTripPreservesMetadata();
        // rate limiter tests
        testRateLimiterClear();
        testRateLimiterClearsOneKey();
        testRateLimiterSlidingWindow();
        testRateLimiterMultipleKeys();
        testRateLimiterEvictsExpiredKeys();
        testRateLimiterEvictionKeepsThrottledKeys();
        testSensitivePathsAreRedacted();
        testRequestIdsAndLogFieldsAreSafe();
        testFetchMetadataMutationPolicy();
        testContentTypeMatching();
        testRequestOriginPolicy();
        testRenderedPagesVersionStaticAssets();
        testSeoMetadata();
    } catch (const std::exception& e) {
        std::cerr << "test failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "nuigraph validation tests passed (56 tests)\n";
    return EXIT_SUCCESS;
}
