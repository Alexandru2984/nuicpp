#pragma once

#include "config/Config.hpp"
#include "observability/Metrics.hpp"
#include "storage/PostgresStorage.hpp"

#include <httplib.h>
#include <string>

namespace nuigraph {

class Routes {
public:
    Routes(const Config& cfg, PostgresStorage& storage, Metrics& metrics);
    void registerRoutes(httplib::Server& server);

private:
    bool ensureAuthenticated(const httplib::Request& req, httplib::Response& res, bool htmlResponse);
    bool verifyPassword(const std::string& password) const;
    std::string makeSessionValue(const std::string& username, int maxAgeSeconds = 28800) const;
    std::string makeSessionValueUntil(const std::string& username, long long expiry) const;
    std::string makeSessionCookie(const httplib::Request& req, const std::string& username) const;
    std::string makeCookie(const httplib::Request& req, const std::string& username, int maxAgeSeconds) const;
    bool verifySession(const httplib::Request& req, std::string* username,
                       bool* usedPreviousKey = nullptr,
                       long long* expiryAt = nullptr) const;
    std::string csrfTokenForSession(const std::string& session) const;
    bool verifyCsrf(const httplib::Request& req) const;
    bool ensureCsrf(const httplib::Request& req, httplib::Response& res);
    bool ensureWriteRateLimit(const httplib::Request& req, httplib::Response& res, bool creation);
    bool isAdmin(const httplib::Request& req) const;
    std::string ownerHashForRequest(const httplib::Request& req) const;
    bool canEditDiagram(const httplib::Request& req, long diagramId);
    bool canReadDiagramById(const httplib::Request& req, long diagramId);
    bool ensureId(const httplib::Request& req, httplib::Response& res, int index, long& out);
    bool ensureExpectedRevision(const httplib::Request& req, httplib::Response& res,
                                long diagramId, long& out) const;
    bool ensureCanRead(const httplib::Request& req, httplib::Response& res, long diagramId, const char* what);
    bool ensureCanEdit(const httplib::Request& req, httplib::Response& res, long diagramId);
    nlohmann::json diagramResponse(const httplib::Request& req, const Diagram& diagram);
    void sendDiagramJson(const httplib::Request& req, httplib::Response& res,
                         int status, const Diagram& diagram);
    void serveStatic(const httplib::Request& req, httplib::Response& res,
                     const std::string& path, bool noStore = false) const;
    void sendJson(httplib::Response& res, int status, const nlohmann::json& body) const;
    void sendStorageError(const httplib::Request& req, httplib::Response& res,
                          const std::exception& e, int status) const;
    void logSecurityEvent(const httplib::Request& req, const httplib::Response& res,
                          const std::string& event, const std::string& outcome) const;
    void applySecurityHeaders(httplib::Response& res) const;

    const Config& cfg_;
    PostgresStorage& storage_;
    Metrics& metrics_;
    std::string assetVersion_;
};

} // namespace nuigraph
