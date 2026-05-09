#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace nuigraph {

struct Node {
    std::string key;
    std::string type;
    std::string title;
    double x = 0;
    double y = 0;
    double width = 160;
    double height = 80;
    std::string color = "#38bdf8";
    nlohmann::json metadata = nlohmann::json::object();
};

struct Edge {
    std::string key;
    std::string source;
    std::string target;
    std::string label;
    bool directed = true;
    std::string color = "#94a3b8";
    nlohmann::json metadata = nlohmann::json::object();
};

struct Diagram {
    long id = 0;
    std::string title;
    std::string slug;
    std::string description;
    std::string createdAt;
    std::string updatedAt;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

struct VersionInfo {
    long id = 0;
    long diagramId = 0;
    int versionNumber = 0;
    std::string createdAt;
    std::string note;
};

nlohmann::json nodeToJson(const Node& node);
nlohmann::json edgeToJson(const Edge& edge);
nlohmann::json diagramToJson(const Diagram& diagram);
Node nodeFromJson(const nlohmann::json& value);
Edge edgeFromJson(const nlohmann::json& value);
Diagram diagramFromJson(const nlohmann::json& value);

} // namespace nuigraph
