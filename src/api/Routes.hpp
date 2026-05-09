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
    std::string makeSessionCookie(const std::string& username) const;
    bool verifySession(const httplib::Request& req, std::string* username) const;
    std::string csrfTokenForSession(const std::string& session) const;
    bool verifyCsrf(const httplib::Request& req) const;
    bool ensureCsrf(const httplib::Request& req, httplib::Response& res);
    void sendJson(httplib::Response& res, int status, const nlohmann::json& body) const;

    const Config& cfg_;
    PostgresStorage& storage_;
};

} // namespace nuigraph
