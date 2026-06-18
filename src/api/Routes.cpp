#include "api/Routes.hpp"

#include "domain/Diagram.hpp"
#include "domain/Templates.hpp"
#include "ui/NuiApp.hpp"
#include "utils/Json.hpp"
#include "utils/RateLimiter.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nuigraph {
namespace {

std::vector<unsigned char> hexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return {};
    }
    std::vector<unsigned char> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        unsigned int byte = 0;
        std::stringstream ss;
        ss << std::hex << hex.substr(i * 2, 2);
        ss >> byte;
        out[i] = static_cast<unsigned char>(byte);
    }
    return out;
}

std::string bytesToHex(const unsigned char* data, std::size_t len) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        out << std::setw(2) << static_cast<int>(data[i]);
    }
    return out.str();
}

std::string hmacHex(const std::string& secret, const std::string& payload) {
    unsigned int len = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, &len);
    return bytesToHex(digest, len);
}

std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) {
        throw std::runtime_error("random generator failed");
    }
    return bytesToHex(data.data(), data.size());
}

bool constantTimeEquals(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

std::vector<std::string> split(const std::string& value, char delim) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        auto pos = value.find(delim, start);
        parts.push_back(value.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}

std::string cookieValue(const httplib::Request& req, const std::string& name) {
    auto header = req.get_header_value("Cookie");
    std::size_t pos = 0;
    while (pos < header.size()) {
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == ';')) ++pos;
        auto end = header.find(';', pos);
        auto token = header.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        auto eq = token.find('=');
        if (eq != std::string::npos && token.substr(0, eq) == name) {
            return token.substr(eq + 1);
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}

std::string clientIp(const httplib::Request& req) {
    auto forwarded = req.get_header_value("X-Forwarded-For");
    if (!forwarded.empty()) {
        auto comma = forwarded.find(',');
        return forwarded.substr(0, comma == std::string::npos ? std::string::npos : comma);
    }
    auto real = req.get_header_value("X-Real-IP");
    if (!real.empty()) {
        return real;
    }
    return req.remote_addr;
}

std::unordered_map<std::string, std::vector<long long>>& loginFailures() {
    static std::unordered_map<std::string, std::vector<long long>> failures;
    return failures;
}

std::mutex& loginFailuresMutex() {
    static std::mutex mutex;
    return mutex;
}

long long epochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

RateLimiter& writeRateLimiter() {
    static RateLimiter limiter;
    return limiter;
}

RateLimiter& createRateLimiter() {
    static RateLimiter limiter;
    return limiter;
}

bool loginBlocked(const std::string& ip) {
    auto now = epochSeconds();
    std::lock_guard<std::mutex> lock(loginFailuresMutex());
    auto& items = loginFailures()[ip];
    items.erase(std::remove_if(items.begin(), items.end(), [now](long long t) {
        return now - t > 10 * 60;
    }), items.end());
    return items.size() >= 8;
}

void recordLoginFailure(const std::string& ip) {
    auto now = epochSeconds();
    std::lock_guard<std::mutex> lock(loginFailuresMutex());
    auto& items = loginFailures()[ip];
    items.erase(std::remove_if(items.begin(), items.end(), [now](long long t) {
        return now - t > 10 * 60;
    }), items.end());
    items.push_back(now);
}

void clearLoginFailures(const std::string& ip) {
    std::lock_guard<std::mutex> lock(loginFailuresMutex());
    loginFailures().erase(ip);
}

std::string contentTypeFor(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".wasm")) return "application/wasm";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".map")) return "application/json";
    return "application/octet-stream";
}

bool safeSlug(const std::string& slug) {
    return !slug.empty() && slug.size() <= 140 && std::all_of(slug.begin(), slug.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
    });
}

long idParam(const httplib::Request& req, int index) {
    return std::stol(req.matches[index].str());
}

Diagram defaultDiagram() {
    Diagram d;
    d.title = "Untitled diagram";
    d.description = "";
    d.nodes = {
        {"node_start", "process", "Start", 80, 80, 160, 78, "#38bdf8", nlohmann::json::object()},
        {"node_decision", "decision", "Decision", 340, 80, 150, 96, "#f59e0b", nlohmann::json::object()},
        {"node_service", "service", "Service", 610, 95, 170, 80, "#22c55e", nlohmann::json::object()}
    };
    d.edges = {
        {"edge_start_decision", "node_start", "node_decision", "next", true, "#94a3b8", nlohmann::json::object()},
        {"edge_decision_service", "node_decision", "node_service", "yes", true, "#94a3b8", nlohmann::json::object()}
    };
    return d;
}

} // namespace

Routes::Routes(const Config& cfg, PostgresStorage& storage)
    : cfg_(cfg), storage_(storage) {}

void Routes::sendJson(httplib::Response& res, int status, const nlohmann::json& body) const {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(jsonResponse(body), "application/json");
}

bool Routes::verifyPassword(const std::string& password) const {
    auto parts = split(cfg_.authPasswordHash, '$');
    if (parts.size() != 4 || parts[0] != "pbkdf2_sha256") {
        return false;
    }
    int iterations = std::stoi(parts[1]);
    auto salt = hexToBytes(parts[2]);
    auto expected = hexToBytes(parts[3]);
    if (salt.empty() || expected.empty()) {
        return false;
    }
    std::vector<unsigned char> actual(expected.size());
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()), iterations, EVP_sha256(), static_cast<int>(actual.size()), actual.data()) != 1) {
        return false;
    }
    return constantTimeEquals(actual, expected);
}

std::string Routes::makeSessionCookie(const std::string& username) const {
    return makeCookie(username, 28800);
}

std::string Routes::makeCookie(const std::string& username, int maxAgeSeconds) const {
    return "ngs_session=" + makeSessionValue(username, maxAgeSeconds) + "; Path=/; Max-Age=" + std::to_string(maxAgeSeconds) + "; HttpOnly; SameSite=Strict; Secure";
}

std::string Routes::makeSessionValue(const std::string& username, int maxAgeSeconds) const {
    auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + maxAgeSeconds;
    std::string payload = username + "." + std::to_string(expiry);
    return payload + "." + hmacHex(cfg_.sessionSecret, payload);
}

bool Routes::verifySession(const httplib::Request& req, std::string* username) const {
    auto session = cookieValue(req, "ngs_session");
    auto parts = split(session, '.');
    if (parts.size() != 3 || parts[0].empty()) {
        return false;
    }
    long long expiry = 0;
    try {
        expiry = std::stoll(parts[1]);
    } catch (...) {
        return false;
    }
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (expiry < now) {
        return false;
    }
    std::string payload = parts[0] + "." + parts[1];
    auto expected = hexToBytes(hmacHex(cfg_.sessionSecret, payload));
    auto actual = hexToBytes(parts[2]);
    if (!constantTimeEquals(actual, expected)) {
        return false;
    }
    if (username) *username = parts[0];
    return true;
}

std::string Routes::csrfTokenForSession(const std::string& session) const {
    return hmacHex(cfg_.sessionSecret, session + ".csrf");
}

bool Routes::verifyCsrf(const httplib::Request& req) const {
    auto session = cookieValue(req, "ngs_session");
    if (session.empty() || !verifySession(req, nullptr)) {
        return false;
    }
    auto supplied = req.get_header_value("X-CSRF-Token");
    if (supplied.empty()) {
        return false;
    }
    auto expected = hexToBytes(csrfTokenForSession(session));
    auto actual = hexToBytes(supplied);
    return constantTimeEquals(actual, expected);
}

bool Routes::ensureCsrf(const httplib::Request& req, httplib::Response& res) {
    if (verifyCsrf(req)) {
        return true;
    }
    sendJson(res, 403, errorJson("invalid csrf token"));
    return false;
}

bool Routes::ensureWriteRateLimit(const httplib::Request& req, httplib::Response& res, bool creation) {
    const auto ip = clientIp(req);
    const auto now = epochSeconds();
    if (!writeRateLimiter().allow("write:" + ip, 240, 10 * 60, now)) {
        sendJson(res, 429, errorJson("too many write requests"));
        return false;
    }
    if (creation && !createRateLimiter().allow("create:" + ip, 40, 10 * 60, now)) {
        sendJson(res, 429, errorJson("too many create requests"));
        return false;
    }
    return true;
}

bool Routes::isAdmin(const httplib::Request& req) const {
    std::string username;
    return verifySession(req, &username) && username == cfg_.authUsername;
}

std::string Routes::ownerHashForRequest(const httplib::Request& req) const {
    std::string username;
    if (!verifySession(req, &username) || username == cfg_.authUsername) {
        return {};
    }
    return hmacHex(cfg_.sessionSecret, "owner:" + username);
}

bool Routes::canEditDiagram(const httplib::Request& req, long diagramId) {
    if (isAdmin(req)) {
        return true;
    }
    std::string owner;
    try {
        owner = storage_.ownerHashForDiagram(diagramId);
    } catch (...) {
        return false;
    }
    if (owner.empty()) {
        return false;
    }
    return owner == ownerHashForRequest(req);
}

bool Routes::canReadDiagramById(const httplib::Request& req, long diagramId) {
    if (isAdmin(req)) {
        return true;
    }
    std::string owner;
    try {
        owner = storage_.ownerHashForDiagram(diagramId);
    } catch (...) {
        return false;
    }
    return !owner.empty() && owner == ownerHashForRequest(req);
}

nlohmann::json Routes::diagramResponse(const httplib::Request& req, const Diagram& diagram) {
    auto body = diagramToJson(diagram);
    body["can_edit"] = canEditDiagram(req, diagram.id);
    body["share_url"] = cfg_.baseUrl + "/d/" + diagram.slug;
    return body;
}

bool Routes::ensureAuthenticated(const httplib::Request& req, httplib::Response& res, bool htmlResponse) {
    if (cfg_.publicAccess) {
        return true;
    }
    if (verifySession(req, nullptr)) {
        return true;
    }
    if (htmlResponse) {
        res.status = 302;
        res.set_header("Location", "/login");
    } else {
        sendJson(res, 401, errorJson("authentication required"));
    }
    return false;
}

void Routes::registerRoutes(httplib::Server& server) {
    server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, {{"status", "ok"}, {"service", "nuigraph-studio"}});
    });

    server.Get("/styles.css", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(readTextFile(cfg_.projectRoot + "/public/styles.css"), "text/css");
    });
    server.Get("/app.js", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(readTextFile(cfg_.projectRoot + "/public/app.js"), "application/javascript");
    });
    server.Get("/editor.js", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(readTextFile(cfg_.projectRoot + "/public/editor.js"), "application/javascript");
    });
    server.Get(R"(/nui/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        auto rel = req.matches[1].str();
        if (rel.find("..") != std::string::npos || rel.starts_with('/')) {
            res.status = 400;
            res.set_content("bad asset path", "text/plain");
            return;
        }
        auto path = cfg_.projectRoot + "/public/nui/" + rel;
        if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }
        if (path.ends_with(".wasm")) {
            res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        }
        res.set_content(readTextFile(path), contentTypeFor(path));
    });

    server.Get("/login", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(renderLoginPage(""), "text/html; charset=utf-8");
    });
    server.Post("/login", [this](const httplib::Request& req, httplib::Response& res) {
        auto ip = clientIp(req);
        if (loginBlocked(ip)) {
            res.status = 429;
            res.set_content(renderLoginPage("Too many login attempts. Try again later."), "text/html; charset=utf-8");
            return;
        }
        auto username = formValue(req.body, "username");
        auto password = formValue(req.body, "password");
        if (username == cfg_.authUsername && verifyPassword(password)) {
            clearLoginFailures(ip);
            res.status = 302;
            res.set_header("Set-Cookie", makeSessionCookie(username));
            res.set_header("Location", "/");
        } else {
            recordLoginFailure(ip);
            res.status = 401;
            res.set_content(renderLoginPage("Invalid credentials"), "text/html; charset=utf-8");
        }
    });
    server.Post("/logout", [](const httplib::Request&, httplib::Response& res) {
        res.status = 302;
        res.set_header("Set-Cookie", "ngs_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict; Secure");
        res.set_header("Location", "/login");
    });

    server.Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        std::string username;
        if (!cfg_.publicAccess && !verifySession(req, &username)) {
            res.status = 302;
            res.set_header("Location", "/login");
            return;
        }
        auto nuiIndex = cfg_.projectRoot + "/public/nui/index.html";
        if (std::filesystem::exists(nuiIndex)) {
            res.set_content(readTextFile(nuiIndex), "text/html; charset=utf-8");
            return;
        }
        res.set_content(renderEditorPage(username), "text/html; charset=utf-8");
    });
    server.Get(R"(/d/([A-Za-z0-9-]+))", [this](const httplib::Request&, httplib::Response& res) {
        auto nuiIndex = cfg_.projectRoot + "/public/nui/index.html";
        if (std::filesystem::exists(nuiIndex)) {
            res.set_content(readTextFile(nuiIndex), "text/html; charset=utf-8");
            return;
        }
        res.set_content(renderEditorPage("guest"), "text/html; charset=utf-8");
    });
    server.Get("/docs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, true)) return;
        res.set_content(renderDocsPage(), "text/html; charset=utf-8");
    });

    server.Get("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
        std::string username;
        if (!verifySession(req, &username)) {
            if (!cfg_.publicAccess) {
                sendJson(res, 401, errorJson("authentication required"));
                return;
            }
            username = "guest_" + randomHex(12);
            auto maxAge = 180 * 24 * 60 * 60;
            auto session = makeSessionValue(username, maxAge);
            res.set_header("Set-Cookie", "ngs_session=" + session + "; Path=/; Max-Age=" + std::to_string(maxAge) + "; HttpOnly; SameSite=Strict; Secure");
            sendJson(res, 200, {{"username", username}, {"public_access", true}, {"csrf_token", csrfTokenForSession(session)}});
            return;
        }
        auto session = cookieValue(req, "ngs_session");
        sendJson(res, 200, {{"username", username}, {"public_access", cfg_.publicAccess}, {"csrf_token", csrfTokenForSession(session)}});
    });

    server.Get("/api/diagrams", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        auto diagrams = isAdmin(req) ? storage_.listDiagrams() : storage_.listDiagramsForOwner(ownerHashForRequest(req));
        sendJson(res, 200, {{"diagrams", diagrams}});
    });

    server.Get("/api/templates", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        nlohmann::json items = nlohmann::json::array();
        for (const auto& item : diagramTemplates()) {
            items.push_back({
                {"key", item.key},
                {"title", item.title},
                {"description", item.description},
                {"category", item.category},
                {"node_count", item.diagram.nodes.size()},
                {"edge_count", item.diagram.edges.size()}
            });
        }
        sendJson(res, 200, {{"templates", items}});
    });

    server.Post(R"(/api/templates/([A-Za-z0-9-]+)/create)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, true)) return;
        if (!ensureCsrf(req, res)) return;
        const auto* item = findDiagramTemplate(req.matches[1].str());
        if (!item) {
            sendJson(res, 404, errorJson("template not found"));
            return;
        }
        try {
            auto created = storage_.createDiagram(item->diagram, true, ownerHashForRequest(req));
            sendJson(res, 201, diagramResponse(req, created));
        } catch (const std::exception& e) {
            sendJson(res, 400, errorJson(e.what()));
        }
    });

    server.Post("/api/diagrams", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, true)) return;
        if (!ensureCsrf(req, res)) return;
        try {
            Diagram d = req.body.empty() ? defaultDiagram() : diagramFromJson(nlohmann::json::parse(req.body));
            auto created = storage_.createDiagram(d, false, ownerHashForRequest(req));
            sendJson(res, 201, diagramResponse(req, created));
        } catch (const std::exception& e) {
            sendJson(res, 400, errorJson(e.what()));
        }
    });

    server.Get(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!canReadDiagramById(req, idParam(req, 1))) {
            sendJson(res, 403, errorJson("diagram is not available by id for this visitor"));
            return;
        }
        try {
            auto diagram = storage_.getDiagram(idParam(req, 1));
            sendJson(res, 200, diagramResponse(req, diagram));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Get(R"(/api/diagrams/slug/([A-Za-z0-9-]+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        auto slug = req.matches[1].str();
        if (!safeSlug(slug)) {
            sendJson(res, 400, errorJson("invalid slug"));
            return;
        }
        try {
            auto diagram = storage_.getDiagramBySlug(slug);
            sendJson(res, 200, diagramResponse(req, diagram));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Put(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        if (!canEditDiagram(req, idParam(req, 1))) {
            sendJson(res, 403, errorJson("diagram is read-only for this visitor"));
            return;
        }
        try {
            auto body = nlohmann::json::parse(req.body);
            auto d = diagramFromJson(body);
            auto note = body.value("note", "saved snapshot");
            sendJson(res, 200, diagramResponse(req, storage_.updateDiagram(idParam(req, 1), d, note, false)));
        } catch (const std::exception& e) {
            sendJson(res, 400, errorJson(e.what()));
        }
    });

    server.Delete(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        if (!canEditDiagram(req, idParam(req, 1))) {
            sendJson(res, 403, errorJson("diagram is read-only for this visitor"));
            return;
        }
        storage_.deleteDiagram(idParam(req, 1));
        sendJson(res, 200, {{"ok", true}});
    });

    server.Post(R"(/api/diagrams/(\d+)/duplicate)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, true)) return;
        if (!ensureCsrf(req, res)) return;
        try {
            auto copy = storage_.duplicateDiagram(idParam(req, 1), ownerHashForRequest(req));
            sendJson(res, 201, diagramResponse(req, copy));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Get(R"(/api/diagrams/(\d+)/versions)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!canReadDiagramById(req, idParam(req, 1))) {
            sendJson(res, 403, errorJson("diagram versions are not available for this visitor"));
            return;
        }
        nlohmann::json items = nlohmann::json::array();
        for (const auto& v : storage_.listVersions(idParam(req, 1))) {
            items.push_back({{"id", v.id}, {"diagram_id", v.diagramId}, {"version_number", v.versionNumber}, {"created_at", v.createdAt}, {"note", v.note}});
        }
        sendJson(res, 200, {{"versions", items}});
    });

    server.Post(R"(/api/diagrams/(\d+)/versions)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        if (!canEditDiagram(req, idParam(req, 1))) {
            sendJson(res, 403, errorJson("diagram is read-only for this visitor"));
            return;
        }
        auto note = std::string("manual snapshot");
        if (!req.body.empty()) {
            try { note = nlohmann::json::parse(req.body).value("note", note); } catch (...) {}
        }
        auto v = storage_.createVersion(idParam(req, 1), note);
        sendJson(res, 201, {{"id", v.id}, {"diagram_id", v.diagramId}, {"version_number", v.versionNumber}, {"created_at", v.createdAt}, {"note", v.note}});
    });

    server.Post(R"(/api/diagrams/(\d+)/restore/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        if (!canEditDiagram(req, idParam(req, 1))) {
            sendJson(res, 403, errorJson("diagram is read-only for this visitor"));
            return;
        }
        try {
            sendJson(res, 200, diagramResponse(req, storage_.restoreVersion(idParam(req, 1), idParam(req, 2))));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Get(R"(/api/diagrams/(\d+)/export\.json)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!canReadDiagramById(req, idParam(req, 1))) {
            sendJson(res, 403, errorJson("diagram export is not available for this visitor"));
            return;
        }
        try {
            res.set_header("Content-Disposition", "attachment; filename=\"diagram-" + req.matches[1].str() + ".json\"");
            sendJson(res, 200, diagramToJson(storage_.getDiagram(idParam(req, 1))));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Post("/api/diagrams/import", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, true)) return;
        if (!ensureCsrf(req, res)) return;
        if (req.body.size() > cfg_.maxImportBytes) {
            sendJson(res, 413, errorJson("import body too large"));
            return;
        }
        try {
            auto imported = storage_.createDiagram(diagramFromJson(nlohmann::json::parse(req.body)), true, ownerHashForRequest(req));
            sendJson(res, 201, diagramResponse(req, imported));
        } catch (const std::exception& e) {
            sendJson(res, 400, errorJson(e.what()));
        }
    });
}

} // namespace nuigraph
