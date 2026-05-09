#include "config/Config.hpp"
#include "domain/Diagram.hpp"
#include "utils/Json.hpp"
#include "validation/DiagramValidator.hpp"

#include <cstdlib>
#include <iostream>
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

} // namespace

int main() {
    try {
        testValidDiagramPasses();
        testMissingEdgeReferenceFails();
        testUnknownImportTypeMapsToNote();
        testUnknownRuntimeTypeFails();
        testLimitsAndClamping();
        testStrictKeysAndColors();
        testJsonRoundTrip();
        testFormUrlDecode();
    } catch (const std::exception& e) {
        std::cerr << "test failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "nuigraph validation tests passed\n";
    return EXIT_SUCCESS;
}
