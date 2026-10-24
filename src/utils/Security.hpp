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

// Match only the media type token, case-insensitively, while allowing standard
// parameters such as `charset=utf-8`. Prefix matches and comma-joined values
// are deliberately rejected.
bool contentTypeMatches(const std::string& contentType,
                        const std::string& expectedMediaType);

// Return a canonical scheme/host/optional-port origin, or an empty string for
// paths, credentials, malformed hosts and unsupported schemes.
std::string canonicalHttpOrigin(const std::string& value);

// Origin is a browser defence-in-depth signal. Missing headers stay compatible
// with non-browser API clients; present, duplicate or opaque values fail closed
// on state-changing methods unless they match the configured application origin.
bool shouldRejectRequestOrigin(const std::string& method,
                               const std::string& origin,
                               std::size_t originCount,
                               const std::string& expectedOrigin);

} // namespace nuigraph
