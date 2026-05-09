#pragma once

#include "config/Config.hpp"
#include "storage/PostgresStorage.hpp"

#include <httplib.h>
#include <string>

namespace nuigraph {

class Routes {
public:
    Routes(const Config& cfg, PostgresStorage& storage);
    void registerRoutes(httplib::Server& server);

private:
    bool ensureAuthenticated(const httplib::Request& req, httplib::Response& res, bool htmlResponse);
    bool verifyPassword(const std::string& password) const;
    std::string makeSessionValue(const std::string& username, int maxAgeSeconds = 28800) const;
    std::string makeSessionCookie(const std::string& username) const;
    std::string makeCookie(const std::string& username, int maxAgeSeconds) const;
    bool verifySession(const httplib::Request& req, std::string* username) const;
    std::string csrfTokenForSession(const std::string& session) const;
    bool verifyCsrf(const httplib::Request& req) const;
    bool ensureCsrf(const httplib::Request& req, httplib::Response& res);
    bool ensureWriteRateLimit(const httplib::Request& req, httplib::Response& res, bool creation);
    bool isAdmin(const httplib::Request& req) const;
    std::string ownerHashForRequest(const httplib::Request& req) const;
    bool canEditDiagram(const httplib::Request& req, long diagramId);
    nlohmann::json diagramResponse(const httplib::Request& req, const Diagram& diagram);
    void sendJson(httplib::Response& res, int status, const nlohmann::json& body) const;

    const Config& cfg_;
    PostgresStorage& storage_;
};

} // namespace nuigraph
