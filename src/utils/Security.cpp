#include "utils/Security.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

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

bool isSafeHttpMethod(const std::string& method) {
    const auto normalized = normalizedToken(method);
    return normalized == "get" || normalized == "head" || normalized == "options";
}

std::vector<unsigned char> decodeBase32(const std::string& encoded) {
    if (encoded.empty() || encoded.size() > 104) {
        return {};
    }
    std::vector<unsigned char> decoded;
    decoded.reserve((encoded.size() * 5) / 8);
    unsigned int buffer = 0;
    int bits = 0;
    for (const unsigned char character : encoded) {
        unsigned int value = 0;
        if (character >= 'A' && character <= 'Z') {
            value = character - 'A';
        } else if (character >= '2' && character <= '7') {
            value = character - '2' + 26;
        } else {
            return {};
        }
        buffer = (buffer << 5) | value;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<unsigned char>((buffer >> bits) & 0xffU));
            buffer &= bits == 0 ? 0U : (1U << bits) - 1U;
        }
    }
    if (bits > 0 && buffer != 0) {
        return {};
    }
    const auto canonicalLength = (decoded.size() * 8 + 4) / 5;
    if (encoded.size() != canonicalLength) {
        return {};
    }
    return decoded;
}

std::optional<std::string> totpAtCounter(
    const std::vector<unsigned char>& secret, std::uint64_t counter) {
    std::array<unsigned char, 8> message{};
    for (int index = 7; index >= 0; --index) {
        message[static_cast<std::size_t>(index)] =
            static_cast<unsigned char>(counter & 0xffU);
        counter >>= 8;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    if (HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()),
             message.data(), message.size(), digest.data(), &digestLength) == nullptr ||
        digestLength != 20) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(digest[19] & 0x0fU);
    if (offset + 3 >= digestLength) {
        return std::nullopt;
    }
    const std::uint32_t binary =
        (static_cast<std::uint32_t>(digest[offset] & 0x7fU) << 24) |
        (static_cast<std::uint32_t>(digest[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(digest[offset + 2]) << 8) |
        static_cast<std::uint32_t>(digest[offset + 3]);
    std::ostringstream rendered;
    rendered << std::setw(6) << std::setfill('0') << (binary % 1000000U);
    return rendered.str();
}

bool constantTimeTextEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index]) ^
                      static_cast<unsigned char>(right[index]);
    }
    return difference == 0;
}

bool isValidDnsHost(const std::string& host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') {
        return false;
    }
    std::size_t start = 0;
    while (start < host.size()) {
        const auto end = host.find('.', start);
        const auto length = (end == std::string::npos ? host.size() : end) - start;
        if (length == 0 || length > 63 || host[start] == '-' ||
            host[start + length - 1] == '-') {
            return false;
        }
        for (std::size_t i = start; i < start + length; ++i) {
            const auto c = static_cast<unsigned char>(host[i]);
            if (std::isalnum(c) == 0 && c != '-') {
                return false;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

std::string canonicalHost(const std::string& host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        const auto literal = host.substr(1, host.size() - 2);
        in6_addr address{};
        if (inet_pton(AF_INET6, literal.c_str(), &address) != 1) {
            return {};
        }
        char rendered[INET6_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET6, &address, rendered, sizeof(rendered)) == nullptr) {
            return {};
        }
        return "[" + std::string(rendered) + "]";
    }

    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) == 1) {
        char rendered[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &address, rendered, sizeof(rendered)) == nullptr) {
            return {};
        }
        return rendered;
    }
    if (std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return std::isdigit(c) != 0 || c == '.';
        })) {
        return {};
    }
    return isValidDnsHost(host) ? host : std::string{};
}

} // namespace

std::string redactRequestPath(const std::string& path) {
    constexpr std::string_view publicPrefix = "/d/";
    constexpr std::string_view apiPrefix = "/api/diagrams/slug/";
    constexpr std::string_view unlockSuffix = "/unlock";
    constexpr std::string_view forkSuffix = "/fork";

    const std::string_view view{path};
    if (view.starts_with(publicPrefix) && view.ends_with(unlockSuffix)) {
        const auto capability = view.substr(
            publicPrefix.size(),
            view.size() - publicPrefix.size() - unlockSuffix.size());
        if (isSinglePathSegment(capability)) {
            return "/d/<redacted>/unlock";
        }
    }
    if (view.starts_with(publicPrefix) && isSinglePathSegment(view.substr(publicPrefix.size()))) {
        return "/d/<redacted>";
    }
    if (view.starts_with(apiPrefix) && view.ends_with(forkSuffix)) {
        const auto capability = view.substr(
            apiPrefix.size(),
            view.size() - apiPrefix.size() - forkSuffix.size());
        if (isSinglePathSegment(capability)) {
            return "/api/diagrams/slug/<redacted>/fork";
        }
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
    if (isSafeHttpMethod(method)) {
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

bool contentTypeMatches(const std::string& contentType,
                        const std::string& expectedMediaType) {
    if (contentType.empty() || expectedMediaType.empty() ||
        contentType.find(',') != std::string::npos) {
        return false;
    }
    const auto separator = contentType.find(';');
    const auto mediaType = normalizedToken(contentType.substr(0, separator));
    return mediaType == normalizedToken(expectedMediaType);
}

std::string canonicalHttpOrigin(const std::string& value) {
    auto normalized = normalizedToken(value);
    const auto schemeEnd = normalized.find("://");
    if (schemeEnd == std::string::npos) {
        return {};
    }
    const auto scheme = normalized.substr(0, schemeEnd);
    if (scheme != "http" && scheme != "https") {
        return {};
    }

    auto authority = normalized.substr(schemeEnd + 3);
    if (authority.ends_with('/')) {
        authority.pop_back();
    }
    if (authority.empty() || authority.find_first_of("/?#@") != std::string::npos) {
        return {};
    }

    std::string host;
    std::string portText;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            return {};
        }
        host = authority.substr(0, close + 1);
        const auto remainder = authority.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':' || remainder.size() == 1) {
                return {};
            }
            portText = remainder.substr(1);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon == std::string::npos) {
            host = authority;
        } else {
            if (authority.find(':', colon + 1) != std::string::npos) {
                return {};
            }
            host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
            if (portText.empty()) {
                return {};
            }
        }
    }

    host = canonicalHost(host);
    if (host.empty()) {
        return {};
    }

    unsigned int port = 0;
    if (!portText.empty()) {
        if (!std::all_of(portText.begin(), portText.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return {};
        }
        for (const auto c : portText) {
            port = port * 10U + static_cast<unsigned int>(c - '0');
            if (port > 65535U) {
                return {};
            }
        }
        if (port == 0U) {
            return {};
        }
    }

    const auto defaultPort = (scheme == "http" && port == 80U) ||
                             (scheme == "https" && port == 443U);
    return scheme + "://" + host +
        (!portText.empty() && !defaultPort ? ":" + std::to_string(port) : "");
}

bool shouldRejectRequestOrigin(const std::string& method,
                               const std::string& origin,
                               std::size_t originCount,
                               const std::string& expectedOrigin) {
    if (isSafeHttpMethod(method) || originCount == 0) {
        return false;
    }
    if (originCount != 1) {
        return true;
    }
    const auto canonical = canonicalHttpOrigin(origin);
    return canonical.empty() || canonical != expectedOrigin;
}

bool isValidTotpSecret(const std::string& base32Secret) {
    const auto decoded = decodeBase32(base32Secret);
    return decoded.size() >= 20 && decoded.size() <= 64;
}

std::optional<std::int64_t> matchingTotpCounter(
    const std::string& base32Secret, const std::string& code,
    std::int64_t unixTimeSeconds) {
    if (unixTimeSeconds < 0 || code.size() != 6 ||
        !std::all_of(code.begin(), code.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return std::nullopt;
    }
    const auto secret = decodeBase32(base32Secret);
    if (secret.size() < 20 || secret.size() > 64) {
        return std::nullopt;
    }

    const auto current = unixTimeSeconds / 30;
    std::optional<std::int64_t> matched;
    for (const auto offset : {-1, 0, 1}) {
        const auto candidate = current + offset;
        if (candidate < 0) {
            continue;
        }
        const auto expected = totpAtCounter(
            secret, static_cast<std::uint64_t>(candidate));
        if (expected.has_value() && constantTimeTextEquals(*expected, code)) {
            // A collision inside the three-step window is unlikely, but using
            // the greatest match keeps the monotonic replay state fail-closed.
            if (!matched.has_value() || candidate > *matched) {
                matched = candidate;
            }
        }
    }
    return matched;
}

} // namespace nuigraph
