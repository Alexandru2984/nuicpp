#pragma once

#include <string>

namespace nuigraph {

struct Config {
    std::string appEnv = "production";
    std::string host = "127.0.0.1";
    int port = 18081;
    std::string baseUrl = "https://nuicpp.micutu.com";
    std::string databaseUrl;
    std::string authUsername = "admin";
    std::string authPasswordHash;
    std::string sessionSecret;
    bool publicAccess = false;
    int maxNodesPerDiagram = 1000;
    int maxEdgesPerDiagram = 2000;
    int maxVersionsPerDiagram = 50;
    int maxDiagramsPerGuest = 60;
    std::size_t maxImportBytes = 1048576;
    std::string projectRoot = "/home/micu/nuicpp";

    static Config load(const std::string& envPath);
};

} // namespace nuigraph
