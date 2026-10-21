#include "utils/Security.hpp"

#include <string_view>

namespace nuigraph {
namespace {

bool isSinglePathSegment(std::string_view value) {
    return !value.empty() && value.find('/') == std::string_view::npos;
}

} // namespace

std::string redactRequestPath(const std::string& path) {
    constexpr std::string_view publicPrefix = "/d/";
    constexpr std::string_view apiPrefix = "/api/diagrams/slug/";

    const std::string_view view{path};
    if (view.starts_with(publicPrefix) && isSinglePathSegment(view.substr(publicPrefix.size()))) {
        return "/d/<redacted>";
    }
    if (view.starts_with(apiPrefix) && isSinglePathSegment(view.substr(apiPrefix.size()))) {
        return "/api/diagrams/slug/<redacted>";
    }
    return path;
}

} // namespace nuigraph
