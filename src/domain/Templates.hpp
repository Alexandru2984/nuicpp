#pragma once

#include "domain/Diagram.hpp"

#include <string>
#include <vector>

namespace nuigraph {

struct DiagramTemplate {
    std::string key;
    std::string title;
    std::string description;
    std::string category;
    Diagram diagram;
};

const std::vector<DiagramTemplate>& diagramTemplates();
const DiagramTemplate* findDiagramTemplate(const std::string& key);

} // namespace nuigraph
