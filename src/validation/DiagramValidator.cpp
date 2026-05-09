#include "validation/DiagramValidator.hpp"

#include <algorithm>
#include <cmath>
#include <regex>
#include <unordered_set>

namespace nuigraph {
namespace {

const std::unordered_set<std::string> kTypes = {
    "process", "decision", "database", "service", "api", "note", "external"
};

bool isKey(const std::string& key) {
    static const std::regex pattern("^[A-Za-z0-9_-]{1,64}$");
    return std::regex_match(key, pattern);
}

bool isColor(const std::string& color) {
    static const std::regex pattern("^#[0-9A-Fa-f]{6}$");
    return std::regex_match(color, pattern);
}

double clamp(double value, double lo, double hi) {
    if (!std::isfinite(value)) return 0;
    return std::max(lo, std::min(value, hi));
}

void truncate(std::string& value, std::size_t max) {
    if (value.size() > max) {
        value.resize(max);
    }
}

} // namespace

DiagramValidator::DiagramValidator(const Config& cfg) : cfg_(cfg) {}

bool DiagramValidator::isKnownNodeType(const std::string& type) const {
    return kTypes.contains(type);
}

ValidationResult DiagramValidator::validate(Diagram& diagram, bool importMode) const {
    ValidationResult result;
    truncate(diagram.title, 120);
    truncate(diagram.description, 1000);
    if (diagram.title.empty()) {
        result.errors.push_back("title is required");
    }
    if (diagram.nodes.size() > static_cast<std::size_t>(cfg_.maxNodesPerDiagram)) {
        result.errors.push_back("too many nodes");
    }
    if (diagram.edges.size() > static_cast<std::size_t>(cfg_.maxEdgesPerDiagram)) {
        result.errors.push_back("too many edges");
    }

    std::unordered_set<std::string> nodeKeys;
    for (auto& node : diagram.nodes) {
        truncate(node.title, 160);
        if (!isKey(node.key)) {
            result.errors.push_back("invalid node key: " + node.key);
        }
        if (nodeKeys.contains(node.key)) {
            result.errors.push_back("duplicate node key: " + node.key);
        }
        nodeKeys.insert(node.key);
        if (!isKnownNodeType(node.type)) {
            if (importMode) {
                node.type = "note";
            } else {
                result.errors.push_back("unknown node type: " + node.type);
            }
        }
        node.x = clamp(node.x, -100000, 100000);
        node.y = clamp(node.y, -100000, 100000);
        node.width = clamp(node.width, 72, 600);
        node.height = clamp(node.height, 48, 400);
        if (!isColor(node.color)) {
            result.errors.push_back("invalid node color for " + node.key);
        }
        if (!node.metadata.is_object()) {
            node.metadata = nlohmann::json::object();
        }
    }

    std::unordered_set<std::string> edgeKeys;
    for (auto& edge : diagram.edges) {
        truncate(edge.label, 160);
        if (!isKey(edge.key)) {
            result.errors.push_back("invalid edge key: " + edge.key);
        }
        if (edgeKeys.contains(edge.key)) {
            result.errors.push_back("duplicate edge key: " + edge.key);
        }
        edgeKeys.insert(edge.key);
        if (!nodeKeys.contains(edge.source) || !nodeKeys.contains(edge.target)) {
            result.errors.push_back("edge references missing node: " + edge.key);
        }
        if (!isColor(edge.color)) {
            result.errors.push_back("invalid edge color for " + edge.key);
        }
        if (!edge.metadata.is_object()) {
            edge.metadata = nlohmann::json::object();
        }
    }

    result.ok = result.errors.empty();
    return result;
}

} // namespace nuigraph
