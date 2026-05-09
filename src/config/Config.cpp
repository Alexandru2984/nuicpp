#include "config/Config.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

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
    return value.empty() ? fallback : std::stoi(value);
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
    cfg.maxNodesPerDiagram = getInt(file, "MAX_NODES_PER_DIAGRAM", cfg.maxNodesPerDiagram);
    cfg.maxEdgesPerDiagram = getInt(file, "MAX_EDGES_PER_DIAGRAM", cfg.maxEdgesPerDiagram);
    cfg.maxVersionsPerDiagram = getInt(file, "MAX_VERSIONS_PER_DIAGRAM", cfg.maxVersionsPerDiagram);
    cfg.maxImportBytes = static_cast<std::size_t>(getInt(file, "MAX_IMPORT_BYTES", static_cast<int>(cfg.maxImportBytes)));
    cfg.projectRoot = getValue(file, "PROJECT_ROOT", cfg.projectRoot);

    if (cfg.host != "127.0.0.1") {
        throw std::runtime_error("APP_HOST must be 127.0.0.1");
    }
    if (cfg.databaseUrl.empty() || cfg.authPasswordHash.empty() || cfg.sessionSecret.empty()) {
        throw std::runtime_error("DATABASE_URL, AUTH_PASSWORD_HASH and SESSION_SECRET are required");
    }
    return cfg;
}

} // namespace nuigraph
