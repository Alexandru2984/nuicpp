#pragma once

#include <string>

namespace nuigraph {

// Share slugs are bearer capabilities. Logs need the route shape for operations,
// never the capability value itself.
std::string redactRequestPath(const std::string& path);

} // namespace nuigraph
