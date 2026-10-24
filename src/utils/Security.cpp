#include "utils/Security.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace nuigraph {
namespace {

bool isSinglePathSegment(std::string_view value) {
    return !value.empty() && value.find('/') == std::string_view::npos;
}

std::string normalizedToken(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    std::string out(first, last);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
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

std::string normalizeRequestId(const std::string& value) {
    if (value.size() < 8 || value.size() > 64) {
        return {};
    }
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '-' || c == '_';
        })) {
        return {};
    }
    return value;
}

std::string generateRequestId() {
    unsigned char bytes[16]{};
    if (RAND_bytes(bytes, sizeof(bytes)) == 1) {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const auto byte : bytes) {
            out << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return out.str();
    }

    static std::atomic<unsigned long long> counter{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return "fallback-" + std::to_string(ticks) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

std::string logTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string sanitizeLogField(const std::string& value, std::size_t maxLength) {
    std::string out;
    out.reserve(std::min(value.size(), maxLength));
    for (const unsigned char c : value) {
        if (out.size() >= maxLength) break;
        const bool safe = std::isalnum(c) != 0 || c == ':' || c == '.' || c == '_' ||
                          c == '-' || c == '/' || c == '<' || c == '>';
        out.push_back(safe ? static_cast<char>(c) : '_');
    }
    return out;
}

bool shouldRejectCrossSiteMutation(const std::string& method,
                                   const std::string& fetchSite) {
    const auto normalizedMethod = normalizedToken(method);
    if (normalizedMethod == "get" || normalizedMethod == "head" ||
        normalizedMethod == "options") {
        return false;
    }

    // Sec-Fetch-Site is not universally sent by API clients. Existing CSRF
    // checks remain authoritative for those callers; when a browser does send
    // the header, only the application's exact origin may mutate state.
    if (fetchSite.empty()) {
        return false;
    }
    return normalizedToken(fetchSite) != "same-origin";
}

} // namespace nuigraph
