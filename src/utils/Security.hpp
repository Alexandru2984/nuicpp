#pragma once

#include <string>

namespace nuigraph {

// Share slugs are bearer capabilities. Logs need the route shape for operations,
// never the capability value itself.
std::string redactRequestPath(const std::string& path);

// Correlation IDs are kept to a log-safe alphabet. nginx overwrites the public
// header; local callers may provide a valid ID for diagnostics.
std::string normalizeRequestId(const std::string& value);
std::string generateRequestId();
std::string logTimestamp();
std::string sanitizeLogField(const std::string& value, std::size_t maxLength = 160);

// Browser-managed Sec-Fetch-* headers cannot be set by page JavaScript. Keep
// non-browser clients compatible when the header is absent, but fail closed for
// any explicitly non-same-origin state-changing request.
bool shouldRejectCrossSiteMutation(const std::string& method,
                                   const std::string& fetchSite);

} // namespace nuigraph
