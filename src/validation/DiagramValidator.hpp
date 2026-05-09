#pragma once

#include "config/Config.hpp"
#include "domain/Diagram.hpp"

#include <string>
#include <vector>

namespace nuigraph {

struct ValidationResult {
    bool ok = false;
    std::vector<std::string> errors;
};

class DiagramValidator {
public:
    explicit DiagramValidator(const Config& cfg);

    ValidationResult validate(Diagram& diagram, bool importMode) const;
    bool isKnownNodeType(const std::string& type) const;

private:
    const Config& cfg_;
};

} // namespace nuigraph
