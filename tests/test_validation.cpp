#include "config/Config.hpp"
#include "domain/Diagram.hpp"
#include "domain/Templates.hpp"
#include "utils/Json.hpp"
#include "utils/RateLimiter.hpp"
#include "validation/DiagramValidator.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    cfg.sessionSecret = "unused";
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

void writeEnvFile(const std::string& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
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
    auto parsed = diagramFromJson(diagramToJson(d));
    require(parsed.nodes.size() == 2, "nodes should round trip");
    require(parsed.edges.size() == 1, "edges should round trip");
    require(parsed.edges[0].source == "node_a", "edge source should round trip");
}

void testFormUrlDecode() {
    auto body = std::string("username=admin&password=a%2Bb+c");
    require(formValue(body, "password") == "a+b c", "form decoding should handle percent and plus");
}

void testPublicAccessConfigLoads() {
    auto path = tmpEnvPath();
    writeEnvFile(path,
        "APP_HOST=127.0.0.1\n"
        "APP_PORT=18081\n"
        "DATABASE_URL=postgresql://unused\n"
        "AUTH_PASSWORD_HASH=unused\n"
        "SESSION_SECRET=unused\n"
        "PUBLIC_ACCESS=true\n");
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
    auto path = tmpEnvPath();
    writeEnvFile(path,
        "APP_HOST=0.0.0.0\n"
        "DATABASE_URL=postgresql://unused\n"
        "AUTH_PASSWORD_HASH=unused\n"
        "SESSION_SECRET=unused\n");
    bool threw = false;
    try {
        Config::load(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    std::remove(path.c_str());
    require(threw, "APP_HOST != 127.0.0.1 should throw");
}

void testConfigMissingRequiredFields() {
    auto path = tmpEnvPath();

    // missing DATABASE_URL
    writeEnvFile(path, "APP_HOST=127.0.0.1\nAUTH_PASSWORD_HASH=x\nSESSION_SECRET=x\n");
    bool threw = false;
    try { Config::load(path); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing DATABASE_URL should throw");

    // missing AUTH_PASSWORD_HASH
    writeEnvFile(path, "APP_HOST=127.0.0.1\nDATABASE_URL=postgresql://x\nSESSION_SECRET=x\n");
    threw = false;
    try { Config::load(path); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing AUTH_PASSWORD_HASH should throw");

    // missing SESSION_SECRET
    writeEnvFile(path, "APP_HOST=127.0.0.1\nDATABASE_URL=postgresql://x\nAUTH_PASSWORD_HASH=x\n");
    threw = false;
    try { Config::load(path); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing SESSION_SECRET should throw");

    std::remove(path.c_str());
}

void testConfigQuotedValues() {
    auto path = tmpEnvPath();
    writeEnvFile(path,
        "APP_HOST=127.0.0.1\n"
        "DATABASE_URL=\"postgresql://quoted\"\n"
        "AUTH_PASSWORD_HASH='single-quoted'\n"
        "SESSION_SECRET=unquoted\n");
    auto cfg = Config::load(path);
    require(cfg.databaseUrl == "postgresql://quoted", "double quotes should be stripped");
    require(cfg.authPasswordHash == "single-quoted", "single quotes should be stripped");
    require(cfg.sessionSecret == "unquoted", "unquoted values should work");
    std::remove(path.c_str());
}

void testConfigCommentAndBlankLines() {
    auto path = tmpEnvPath();
    writeEnvFile(path,
        "# This is a comment\n"
        "\n"
        "APP_HOST=127.0.0.1\n"
        "# Another comment\n"
        "DATABASE_URL=postgresql://test\n"
        "\n"
        "AUTH_PASSWORD_HASH=hash\n"
        "SESSION_SECRET=secret\n");
    auto cfg = Config::load(path);
    require(cfg.databaseUrl == "postgresql://test", "comments and blanks should be ignored");
    std::remove(path.c_str());
}

void testConfigEnvOverrides() {
    auto path = tmpEnvPath();
    writeEnvFile(path,
        "APP_HOST=127.0.0.1\n"
        "DATABASE_URL=postgresql://file\n"
        "AUTH_PASSWORD_HASH=hash\n"
        "SESSION_SECRET=secret\n"
        "APP_PORT=9999\n");
    setenv("APP_PORT", "7777", 1);
    auto cfg = Config::load(path);
    unsetenv("APP_PORT");
    require(cfg.port == 7777, "env var should override file value");
    std::remove(path.c_str());
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

void testAllNodeTypesAccepted() {
    auto cfg = testConfig();
    cfg.maxNodesPerDiagram = 100;
    DiagramValidator validator(cfg);
    std::vector<std::string> types = {"process", "decision", "database", "service", "api", "note", "external"};
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
        testRateLimiterSlidingWindow();
        testRateLimiterMultipleKeys();
        testRateLimiterEvictsExpiredKeys();
        testRateLimiterEvictionKeepsThrottledKeys();
    } catch (const std::exception& e) {
        std::cerr << "test failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "nuigraph validation tests passed (38 tests)\n";
    return EXIT_SUCCESS;
}
