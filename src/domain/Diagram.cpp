#include "domain/Diagram.hpp"

#include <stdexcept>

namespace nuigraph {

nlohmann::json nodeToJson(const Node& node) {
    return {
        {"key", node.key},
        {"type", node.type},
        {"title", node.title},
        {"x", node.x},
        {"y", node.y},
        {"width", node.width},
        {"height", node.height},
        {"color", node.color},
        {"metadata", node.metadata.is_object() ? node.metadata : nlohmann::json::object()}
    };
}

nlohmann::json edgeToJson(const Edge& edge) {
    return {
        {"key", edge.key},
        {"source", edge.source},
        {"target", edge.target},
        {"label", edge.label},
        {"directed", edge.directed},
        {"color", edge.color},
        {"metadata", edge.metadata.is_object() ? edge.metadata : nlohmann::json::object()}
    };
}

nlohmann::json diagramToJson(const Diagram& diagram) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : diagram.nodes) {
        nodes.push_back(nodeToJson(node));
    }
    nlohmann::json edges = nlohmann::json::array();
    for (const auto& edge : diagram.edges) {
        edges.push_back(edgeToJson(edge));
    }
    return {
        {"id", diagram.id},
        {"title", diagram.title},
        {"slug", diagram.slug},
        {"description", diagram.description},
        {"created_at", diagram.createdAt},
        {"updated_at", diagram.updatedAt},
        {"nodes", nodes},
        {"edges", edges}
    };
}

Node nodeFromJson(const nlohmann::json& value) {
    Node node;
    node.key = value.value("key", value.value("node_key", ""));
    node.type = value.value("type", "process");
    node.title = value.value("title", "Untitled");
    node.x = value.value("x", 0.0);
    node.y = value.value("y", 0.0);
    node.width = value.value("width", 160.0);
    node.height = value.value("height", 80.0);
    node.color = value.value("color", "#38bdf8");
    node.metadata = value.contains("metadata") && value["metadata"].is_object() ? value["metadata"] : nlohmann::json::object();
    return node;
}

Edge edgeFromJson(const nlohmann::json& value) {
    Edge edge;
    edge.key = value.value("key", value.value("edge_key", ""));
    edge.source = value.value("source", value.value("source_node_key", ""));
    edge.target = value.value("target", value.value("target_node_key", ""));
    edge.label = value.value("label", "");
    edge.directed = value.value("directed", true);
    edge.color = value.value("color", "#94a3b8");
    edge.metadata = value.contains("metadata") && value["metadata"].is_object() ? value["metadata"] : nlohmann::json::object();
    return edge;
}

Diagram diagramFromJson(const nlohmann::json& value) {
    Diagram diagram;
    diagram.id = value.value("id", 0L);
    diagram.title = value.value("title", "Untitled diagram");
    diagram.slug = value.value("slug", "");
    diagram.description = value.value("description", "");
    if (!value.contains("nodes") || !value["nodes"].is_array() || !value.contains("edges") || !value["edges"].is_array()) {
        throw std::runtime_error("diagram JSON must include nodes and edges arrays");
    }
    for (const auto& item : value["nodes"]) {
        diagram.nodes.push_back(nodeFromJson(item));
    }
    for (const auto& item : value["edges"]) {
        diagram.edges.push_back(edgeFromJson(item));
    }
    return diagram;
}

} // namespace nuigraph
