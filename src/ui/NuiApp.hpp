#pragma once

#include <string>

namespace nuigraph {

std::string renderLoginPage(const std::string& error);
std::string renderEditorPage(const std::string& username);
std::string renderDocsPage();

} // namespace nuigraph
