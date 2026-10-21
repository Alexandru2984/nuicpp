#include "config/Config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
    if (!isHex(cfg.sessionSecret) || cfg.sessionSecret.size() < 64 || cfg.sessionSecret.size() > 256) {
        throw std::runtime_error("SESSION_SECRET must contain 32 to 128 random bytes encoded as hex");
    }
    if (cfg.projectRoot.empty() || cfg.projectRoot.front() != '/') {
        throw std::runtime_error("PROJECT_ROOT must be an absolute path");
    }
    if (cfg.appEnv == "production" && !cfg.baseUrl.starts_with("https://")) {
        throw std::runtime_error("APP_BASE_URL must use HTTPS in production");
    }
}

} // namespace

Config Config::load(const std::string& envPath) {
    auto file = readEnvFile(envPath);
    Config cfg;
    cfg.appEnv = getValue(file, "APP_ENV", cfg.appEnv);
    cfg.host = getValue(file, "APP_HOST", cfg.host);
    cfg.port = getInt(file, "APP_PORT", cfg.port);
    cfg.baseUrl = getValue(file, "APP_BASE_URL", cfg.baseUrl);
    cfg.databaseUrl = getValue(file, "DATABASE_URL");
    cfg.authUsername = getValue(file, "AUTH_USERNAME", cfg.authUsername);
    cfg.authPasswordHash = getValue(file, "AUTH_PASSWORD_HASH");
    cfg.sessionSecret = getValue(file, "SESSION_SECRET");
    cfg.publicAccess = getBool(file, "PUBLIC_ACCESS", cfg.publicAccess);
    cfg.maxNodesPerDiagram = getInt(file, "MAX_NODES_PER_DIAGRAM", cfg.maxNodesPerDiagram);
    cfg.maxEdgesPerDiagram = getInt(file, "MAX_EDGES_PER_DIAGRAM", cfg.maxEdgesPerDiagram);
    cfg.maxVersionsPerDiagram = getInt(file, "MAX_VERSIONS_PER_DIAGRAM", cfg.maxVersionsPerDiagram);
    cfg.maxDiagramsPerGuest = getInt(file, "MAX_DIAGRAMS_PER_GUEST", cfg.maxDiagramsPerGuest);
    cfg.maxDiagramsTotal = getInt(file, "MAX_DIAGRAMS_TOTAL", cfg.maxDiagramsTotal);
    cfg.dbPoolSize = getInt(file, "DB_POOL_SIZE", cfg.dbPoolSize);
    cfg.maxImportBytes = static_cast<std::size_t>(getInt(file, "MAX_IMPORT_BYTES", static_cast<int>(cfg.maxImportBytes)));
    cfg.projectRoot = getValue(file, "PROJECT_ROOT", cfg.projectRoot);

    validateConfig(cfg);
    return cfg;
}

} // namespace nuigraph
