#pragma once

#include <string>

namespace nuigraph {

std::string renderLoginPage(const std::string& error);
std::string renderEditorPage(const std::string& username, const std::string& baseUrl,
                             bool indexable = true);
std::string renderDocsPage(const std::string& baseUrl);

} // namespace nuigraph
