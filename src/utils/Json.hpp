#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace nuigraph {

std::string jsonResponse(const nlohmann::json& value);
nlohmann::json errorJson(const std::string& message);
std::string readTextFile(const std::string& path);
std::string htmlEscape(const std::string& input);
std::string urlDecode(const std::string& value);
std::string formValue(const std::string& body, const std::string& key);

} // namespace nuigraph
