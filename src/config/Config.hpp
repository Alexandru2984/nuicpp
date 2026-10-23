#pragma once

#include <cstdint>
#include <string>

namespace nuigraph {

enum class FrontendShell {
    Server,
    Nui
};

const char* frontendShellName(FrontendShell shell);

struct Config {
    std::string appEnv = "production";
    std::string host = "127.0.0.1";
    int port = 18081;
    std::string baseUrl = "https://nuicpp.micutu.com";
    std::string databaseUrl;
    std::string authUsername = "admin";
    std::string authPasswordHash;
    std::string sessionSigningSecret;
    std::string previousSessionSigningSecret;
    std::string csrfSecret;
    std::string ownerHashSecret;
    std::string rateLimitSecret;
    bool publicAccess = false;
    FrontendShell frontendShell = FrontendShell::Server;
    int maxNodesPerDiagram = 1000;
    int maxEdgesPerDiagram = 2000;
    int maxVersionsPerDiagram = 50;
    int maxDiagramsPerGuest = 60;
    int maxDiagramsTotal = 5000;
    std::uint64_t maxLogicalBytesPerGuest = 32ULL * 1024 * 1024;
    std::uint64_t maxLogicalBytesTotal = 512ULL * 1024 * 1024;
    int dbPoolSize = 8;
    std::size_t maxImportBytes = 1048576;
    std::string projectRoot = "/opt/nuigraph-studio/current";

    static Config load(const std::string& envPath);
};

} // namespace nuigraph
