#include "config/Config.hpp"
#include "utils/Security.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nuigraph {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

std::unordered_map<std::string, std::string> readEnvFile(const std::string& path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(".env file not found: " + path);
    }
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        values[key] = value;
    }
    return values;
}

std::string getValue(const std::unordered_map<std::string, std::string>& file, const std::string& key, const std::string& fallback = "") {
    if (const char* env = std::getenv(key.c_str())) {
        return env;
    }
    auto it = file.find(key);
    return it == file.end() ? fallback : it->second;
}

int getInt(const std::unordered_map<std::string, std::string>& file, const std::string& key, int fallback) {
    auto value = getValue(file, key);
    if (value.empty()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const long long parsed = std::stoll(value, &consumed);
        if (consumed != value.size() || parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) {
            throw std::runtime_error("out of range");
        }
        return static_cast<int>(parsed);
    } catch (...) {
        throw std::runtime_error(key + " must be a valid integer");
    }
}

std::uint64_t getUint64(const std::unordered_map<std::string, std::string>& file,
                        const std::string& key, std::uint64_t fallback) {
    const auto value = getValue(file, key);
    if (value.empty()) {
        return fallback;
    }
    if (value.front() == '-' || value.front() == '+') {
        throw std::runtime_error(key + " must be a valid non-negative integer");
    }
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            throw std::runtime_error("trailing data");
        }
        return parsed;
    } catch (...) {
        throw std::runtime_error(key + " must be a valid non-negative integer");
    }
}

bool getBool(const std::unordered_map<std::string, std::string>& file, const std::string& key, bool fallback) {
    auto value = getValue(file, key);
    if (value.empty()) {
        return fallback;
    }
    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    throw std::runtime_error(key + " must be an explicit boolean");
}

bool isHex(const std::string& value) {
    return !value.empty() && value.size() % 2 == 0 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isxdigit(c) != 0;
           });
}

FrontendShell parseFrontendShell(std::string value) {
    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (value == "server") {
        return FrontendShell::Server;
    }
    if (value == "nui") {
        return FrontendShell::Nui;
    }
    throw std::runtime_error("FRONTEND_SHELL must be server or nui");
}

bool isRegularReleaseFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    return !ec && !std::filesystem::is_symlink(status) &&
           std::filesystem::is_regular_file(status);
}

void validateNuiBundle(const std::string& projectRoot) {
    const auto directory = std::filesystem::path(projectRoot) / "public" / "nui";
    if (!isRegularReleaseFile(directory / "index.html") ||
        !isRegularReleaseFile(directory / "index.js")) {
        throw std::runtime_error(
            "FRONTEND_SHELL=nui requires regular public/nui/index.html and index.js files");
    }

    std::error_code ec;
    bool hasWasm = false;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().extension() == ".wasm" && isRegularReleaseFile(it->path())) {
            hasWasm = true;
            break;
        }
    }
    if (ec || !hasWasm) {
        throw std::runtime_error(
            "FRONTEND_SHELL=nui requires a regular WebAssembly module in public/nui");
    }
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto pos = value.find(delimiter, start);
        parts.push_back(value.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

void requireRange(const char* name, int value, int low, int high) {
    if (value < low || value > high) {
        throw std::runtime_error(std::string(name) + " is outside the allowed range");
    }
}

void requireRange(const char* name, std::uint64_t value,
                  std::uint64_t low, std::uint64_t high) {
    if (value < low || value > high) {
        throw std::runtime_error(std::string(name) + " is outside the allowed range");
    }
}

void validatePasswordHash(const std::string& encoded) {
    const auto parts = split(encoded, '$');
    if (parts.size() != 4 || parts[0] != "pbkdf2_sha256" || !isHex(parts[2]) ||
        parts[2].size() < 32 || !isHex(parts[3]) || parts[3].size() != 64) {
        throw std::runtime_error("AUTH_PASSWORD_HASH must be a canonical PBKDF2-SHA256 hash");
    }
    try {
        std::size_t consumed = 0;
        const long long iterations = std::stoll(parts[1], &consumed);
        if (consumed != parts[1].size() || iterations < 100000 || iterations > 5000000) {
            throw std::runtime_error("out of range");
        }
    } catch (...) {
        throw std::runtime_error("AUTH_PASSWORD_HASH iteration count is outside the allowed range");
    }
}

void validateSecret(const char* name, const std::string& value, bool optional = false) {
    if (optional && value.empty()) {
        return;
    }
    if (!isHex(value) || value.size() < 64 || value.size() > 256) {
        throw std::runtime_error(
            std::string(name) + " must contain 32 to 128 random bytes encoded as hex");
    }
}

void validateConfig(const Config& cfg) {
    if (cfg.appEnv != "production" && cfg.appEnv != "development" && cfg.appEnv != "test") {
        throw std::runtime_error("APP_ENV must be production, development or test");
    }
    if (cfg.host != "127.0.0.1") {
        throw std::runtime_error("APP_HOST must be 127.0.0.1");
    }
    requireRange("APP_PORT", cfg.port, 1024, 65535);
    requireRange("MAX_NODES_PER_DIAGRAM", cfg.maxNodesPerDiagram, 1, 10000);
    requireRange("MAX_EDGES_PER_DIAGRAM", cfg.maxEdgesPerDiagram, 1, 20000);
    requireRange("MAX_VERSIONS_PER_DIAGRAM", cfg.maxVersionsPerDiagram, 1, 1000);
    requireRange("MAX_DIAGRAMS_PER_GUEST", cfg.maxDiagramsPerGuest, 1, 10000);
    requireRange("MAX_DIAGRAMS_TOTAL", cfg.maxDiagramsTotal, 1, 1000000);
    constexpr std::uint64_t kMaxLogicalStorage = 1024ULL * 1024 * 1024 * 1024;
    requireRange("MAX_LOGICAL_BYTES_PER_GUEST", cfg.maxLogicalBytesPerGuest,
                 1024ULL, kMaxLogicalStorage);
    requireRange("MAX_LOGICAL_BYTES_TOTAL", cfg.maxLogicalBytesTotal,
                 1024ULL, kMaxLogicalStorage);
    if (cfg.maxLogicalBytesPerGuest > cfg.maxLogicalBytesTotal) {
        throw std::runtime_error(
            "MAX_LOGICAL_BYTES_PER_GUEST cannot exceed MAX_LOGICAL_BYTES_TOTAL");
    }
    requireRange("DB_POOL_SIZE", cfg.dbPoolSize, 1, 64);
    if (cfg.maxImportBytes < 1024 || cfg.maxImportBytes > 16 * 1024 * 1024) {
        throw std::runtime_error("MAX_IMPORT_BYTES is outside the allowed range");
    }
    if (cfg.databaseUrl.empty()) {
        throw std::runtime_error("DATABASE_URL is required");
    }
    if (cfg.authUsername.empty() || cfg.authUsername.size() > 64 ||
        !std::all_of(cfg.authUsername.begin(), cfg.authUsername.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_' || c == '-';
        })) {
        throw std::runtime_error("AUTH_USERNAME contains unsupported characters");
    }
    validatePasswordHash(cfg.authPasswordHash);
    validateSecret("SESSION_SIGNING_SECRET", cfg.sessionSigningSecret);
    validateSecret("SESSION_SIGNING_SECRET_PREVIOUS", cfg.previousSessionSigningSecret, true);
    validateSecret("CSRF_SECRET", cfg.csrfSecret);
    validateSecret("OWNER_HASH_SECRET", cfg.ownerHashSecret);
    validateSecret("RATE_LIMIT_SECRET", cfg.rateLimitSecret);
    std::unordered_set<std::string> secrets;
    for (const auto* secret : {
             &cfg.sessionSigningSecret,
             &cfg.csrfSecret,
             &cfg.ownerHashSecret,
             &cfg.rateLimitSecret}) {
        if (!secrets.insert(*secret).second) {
            throw std::runtime_error("security secrets must be distinct");
        }
    }
    if (!cfg.previousSessionSigningSecret.empty() &&
        (cfg.previousSessionSigningSecret == cfg.sessionSigningSecret ||
         cfg.previousSessionSigningSecret == cfg.csrfSecret ||
         cfg.previousSessionSigningSecret == cfg.rateLimitSecret)) {
        throw std::runtime_error(
            "SESSION_SIGNING_SECRET_PREVIOUS must differ from signing, CSRF and rate-limit secrets");
    }
    if (cfg.projectRoot.empty() || cfg.projectRoot.front() != '/') {
        throw std::runtime_error("PROJECT_ROOT must be an absolute path");
    }
    if (cfg.frontendShell == FrontendShell::Nui) {
        validateNuiBundle(cfg.projectRoot);
    }
    if (cfg.appEnv == "production" && !cfg.baseUrl.starts_with("https://")) {
        throw std::runtime_error("APP_BASE_URL must use HTTPS in production");
    }
}

} // namespace

const char* frontendShellName(FrontendShell shell) {
    switch (shell) {
        case FrontendShell::Server:
            return "server";
        case FrontendShell::Nui:
            return "nui";
    }
    return "unknown";
}

Config Config::load(const std::string& envPath) {
    auto file = readEnvFile(envPath);
    Config cfg;
    cfg.appEnv = getValue(file, "APP_ENV", cfg.appEnv);
    cfg.host = getValue(file, "APP_HOST", cfg.host);
    cfg.port = getInt(file, "APP_PORT", cfg.port);
    cfg.baseUrl = getValue(file, "APP_BASE_URL", cfg.baseUrl);
    cfg.baseUrl = canonicalHttpOrigin(cfg.baseUrl);
    if (cfg.baseUrl.empty()) {
        throw std::runtime_error(
            "APP_BASE_URL must be an HTTP(S) origin without credentials, path, query or fragment");
    }
    cfg.databaseUrl = getValue(file, "DATABASE_URL");
    cfg.authUsername = getValue(file, "AUTH_USERNAME", cfg.authUsername);
    cfg.authPasswordHash = getValue(file, "AUTH_PASSWORD_HASH");
    if (!getValue(file, "SESSION_SECRET").empty()) {
        throw std::runtime_error(
            "SESSION_SECRET is no longer supported; configure the four purpose-specific secrets");
    }
    cfg.sessionSigningSecret = getValue(file, "SESSION_SIGNING_SECRET");
    cfg.previousSessionSigningSecret = getValue(file, "SESSION_SIGNING_SECRET_PREVIOUS");
    cfg.csrfSecret = getValue(file, "CSRF_SECRET");
    cfg.ownerHashSecret = getValue(file, "OWNER_HASH_SECRET");
    cfg.rateLimitSecret = getValue(file, "RATE_LIMIT_SECRET");
    cfg.publicAccess = getBool(file, "PUBLIC_ACCESS", cfg.publicAccess);
    cfg.frontendShell = parseFrontendShell(
        getValue(file, "FRONTEND_SHELL", frontendShellName(cfg.frontendShell)));
    cfg.maxNodesPerDiagram = getInt(file, "MAX_NODES_PER_DIAGRAM", cfg.maxNodesPerDiagram);
    cfg.maxEdgesPerDiagram = getInt(file, "MAX_EDGES_PER_DIAGRAM", cfg.maxEdgesPerDiagram);
    cfg.maxVersionsPerDiagram = getInt(file, "MAX_VERSIONS_PER_DIAGRAM", cfg.maxVersionsPerDiagram);
    cfg.maxDiagramsPerGuest = getInt(file, "MAX_DIAGRAMS_PER_GUEST", cfg.maxDiagramsPerGuest);
    cfg.maxDiagramsTotal = getInt(file, "MAX_DIAGRAMS_TOTAL", cfg.maxDiagramsTotal);
    cfg.maxLogicalBytesPerGuest = getUint64(
        file, "MAX_LOGICAL_BYTES_PER_GUEST", cfg.maxLogicalBytesPerGuest);
    cfg.maxLogicalBytesTotal = getUint64(
        file, "MAX_LOGICAL_BYTES_TOTAL", cfg.maxLogicalBytesTotal);
    cfg.dbPoolSize = getInt(file, "DB_POOL_SIZE", cfg.dbPoolSize);
    cfg.maxImportBytes = static_cast<std::size_t>(getInt(file, "MAX_IMPORT_BYTES", static_cast<int>(cfg.maxImportBytes)));
    cfg.projectRoot = getValue(file, "PROJECT_ROOT", cfg.projectRoot);

    validateConfig(cfg);
    return cfg;
}

} // namespace nuigraph
