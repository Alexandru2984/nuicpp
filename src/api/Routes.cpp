#include "api/Routes.hpp"

#include "domain/Diagram.hpp"
#include "domain/Templates.hpp"
#include "ui/NuiApp.hpp"
#include "utils/Json.hpp"
#include "utils/Security.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <pqxx/except>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
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

std::string trimHeaderValue(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

// The service binds loopback only, so the sole legitimate peer is the local
// nginx, which sets X-Real-IP from $remote_addr (already rewritten to the real
// visitor by the Cloudflare real-ip config).
//
// The X-Forwarded-For fallback must take the LAST entry, not the first: nginx
// forwards it as $proxy_add_x_forwarded_for, which appends the peer address to
// whatever the client sent. The leading entries are attacker-controlled, so
// keying rate limits on them would let anyone forge a fresh bucket per request.
std::string clientIp(const httplib::Request& req) {
    auto real = trimHeaderValue(req.get_header_value("X-Real-IP"));
    if (!real.empty()) {
        return real;
    }
    auto forwarded = req.get_header_value("X-Forwarded-For");
    if (!forwarded.empty()) {
        auto comma = forwarded.rfind(',');
        auto last = trimHeaderValue(comma == std::string::npos ? forwarded : forwarded.substr(comma + 1));
        if (!last.empty()) {
            return last;
        }
    }
    return req.remote_addr;
}

// A cookie marked Secure is never sent back over plain HTTP, so hard-coding it
// made the session unusable for anything not speaking TLS: local runs and the
// integration tests got a fresh anonymous session on every request.
//
// nginx sets X-Forwarded-Proto on both vhost blocks and the service binds
// loopback, so in production this is always "https" and the attribute is kept.
// A direct plain-HTTP caller is either a developer or the test harness.
bool requestIsSecure(const httplib::Request& req) {
    auto proto = trimHeaderValue(req.get_header_value("X-Forwarded-Proto"));
    return proto == "https";
}

std::string sessionCookie(const httplib::Request& req, const std::string& value, int maxAgeSeconds) {
    std::string cookie = "ngs_session=" + value + "; Path=/; Max-Age=" + std::to_string(maxAgeSeconds) +
                         "; HttpOnly; SameSite=Strict";
    if (requestIsSecure(req)) {
        cookie += "; Secure";
    }
    return cookie;
}

constexpr long long kLoginFailureWindow = 10 * 60;
constexpr int kGuestSessionSeconds = 7 * 24 * 60 * 60;

// A page comfortably exceeds MAX_DIAGRAMS_PER_GUEST, so a guest still gets its
// whole list in one request and only the admin view ever pages.
constexpr long kDefaultListLimit = 100;
constexpr long kMaxListLimit = 200;
// OFFSET makes PostgreSQL walk and discard every skipped row, so a huge value is
// a cheap way to make the server work hard. Cap it; anyone legitimately past
// this many diagrams needs keyset pagination, not a bigger offset.
constexpr long kMaxListOffset = 100000;

// Out-of-range and unparseable values are clamped rather than rejected: this is
// a list view, and a 400 for ?limit=0 helps nobody.
long clampQueryParam(const httplib::Request& req, const char* name, long fallback, long low, long high) {
    if (!req.has_param(name)) {
        return fallback;
    }
    const auto raw = req.get_param_value(name);
    if (raw.empty() || raw.size() > 18) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        long value = std::stol(raw, &consumed);
        if (consumed != raw.size()) {
            return fallback;
        }
        return std::clamp(value, low, high);
    } catch (...) {
        return fallback;
    }
}

std::string sha256Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    return bytesToHex(digest, len);
}

std::string contentTypeFor(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".wasm")) return "application/wasm";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".map")) return "application/json";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".webmanifest")) return "application/manifest+json";
    if (path.ends_with(".xml")) return "application/xml; charset=utf-8";
    if (path.ends_with(".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::string normalizedBaseUrl(std::string baseUrl) {
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.pop_back();
    }
    return baseUrl;
}

void replaceAll(std::string& value, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    std::size_t offset = 0;
    while ((offset = value.find(from, offset)) != std::string::npos) {
        value.replace(offset, from.size(), to);
        offset += to.size();
    }
}

std::string nuiShellPage(const std::string& projectRoot, const std::string& baseUrl,
                         bool indexable) {
    auto page = readTextFile(projectRoot + "/public/nui/index.html");
    replaceAll(page, "https://nuicpp.micutu.com", htmlEscape(normalizedBaseUrl(baseUrl)));
    if (!indexable) {
        replaceAll(page, "content=\"index, follow\"",
                   "content=\"noindex, nofollow, noarchive\"");
    }
    return page;
}

// The Nui shell is an Emscripten bundle: index.js is only the loader and calls
// createWasm(), which fetches the compiled module. build_nui_frontend.sh used to
// install the loader without the .wasm beside it, so the module could never
// instantiate, Module.main() never ran, and the #app element bootstrap.js waits
// for was never created. The result was a blank page.
//
// Serve the shell only when the bundle is actually complete, otherwise fall
// back to the server-rendered editor, which needs no WebAssembly at all.
bool nuiShellReady(const std::string& projectRoot) {
    std::error_code ec;
    const auto dir = projectRoot + "/public/nui";
    if (!std::filesystem::exists(dir + "/index.html", ec)) {
        return false;
    }
    if (!std::filesystem::exists(dir + "/index.js", ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!ec && entry.is_regular_file(ec) && entry.path().extension() == ".wasm") {
            return true;
        }
    }
    return false;
}

bool safeSlug(const std::string& slug) {
    return !slug.empty() && slug.size() <= 140 && std::all_of(slug.begin(), slug.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
    });
}

// Route regexes match \d+ with no length bound, so a request for
// /api/diagrams/99999999999999999999 used to reach std::stol and throw
// std::out_of_range straight out of the handler. Parse defensively instead and
// let callers turn a bad id into a 400.
bool parseIdParam(const httplib::Request& req, int index, long& out) {
    const auto text = req.matches[index].str();
    if (text.empty() || text.size() > 18) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        long value = std::stol(text, &consumed);
        if (consumed != text.size() || value <= 0) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

std::string diagramEtag(const Diagram& diagram) {
    return "\"diagram-" + std::to_string(diagram.id) + "-r" +
           std::to_string(diagram.revision) + "\"";
}

bool parseDiagramEtag(const std::string& raw, long diagramId, long& revision) {
    const auto value = trimHeaderValue(raw);
    const auto prefix = "\"diagram-" + std::to_string(diagramId) + "-r";
    if (!value.starts_with(prefix) || value.size() <= prefix.size() + 1 || value.back() != '"') {
        return false;
    }
    const auto number = value.substr(prefix.size(), value.size() - prefix.size() - 1);
    if (number.empty() || number.size() > 18 ||
        !std::all_of(number.begin(), number.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stol(number, &consumed);
        if (consumed != number.size() || parsed <= 0) {
            return false;
        }
        revision = parsed;
        return true;
    } catch (...) {
        return false;
    }
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

// The vhost sets these too, but the app must not depend on a proxy staying
// configured correctly to be safe. httplib keeps the first value for a header,
// so nginx's copies win where both are present.
void Routes::applySecurityHeaders(httplib::Response& res) const {
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
    res.set_header("Cross-Origin-Opener-Policy", "same-origin");
    res.set_header("Cross-Origin-Resource-Policy", "same-origin");
    res.set_header("Permissions-Policy", "geolocation=(), microphone=(), camera=(), interest-cohort=()");
    // Cloudflare injects its Web Analytics beacon at the edge, so the script
    // host has to be allowed even though nothing in this repo references it.
    // 'unsafe-inline' is deliberately NOT granted: the beacon's inline loader
    // stays blocked, which costs some reporting fidelity but keeps the policy
    // meaningful against injected script.
    res.set_header("Content-Security-Policy",
                   "default-src 'self'; "
                   "script-src 'self' 'wasm-unsafe-eval' https://static.cloudflareinsights.com; "
                   "style-src 'self'; "
                   "img-src 'self' data: blob:; "
                   "connect-src 'self' https://cloudflareinsights.com; "
                   "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
}

// Static assets carry no content hash in their names, and the app sent no
// caching headers at all, so Cloudflare applied its own default and a CSS or JS
// deploy could take hours to reach visitors.
//
// no-cache does not mean "do not store": caches may keep the response but must
// revalidate, so an unchanged file still costs only a 304 while a changed one
// is picked up on the next request. The CDN-specific headers keep that contract
// separate from browser caching. Cache Rules can still override origin headers,
// which is why the matching Cloudflare rule is documented in DEPLOY.md.
void Routes::serveStatic(const httplib::Request& req, httplib::Response& res,
                         const std::string& path, bool noStore) const {
    std::string body;
    try {
        body = readTextFile(path);
    } catch (const std::exception&) {
        res.status = 404;
        res.set_content("not found", "text/plain");
        return;
    }

    const auto etag = "\"" + sha256Hex(body).substr(0, 32) + "\"";
    res.set_header("ETag", etag);
    if (noStore) {
        res.set_header("Cache-Control", "no-store, max-age=0");
        res.set_header("CDN-Cache-Control", "no-store");
        res.set_header("Cloudflare-CDN-Cache-Control", "no-store");
    } else {
        res.set_header("Cache-Control", "public, no-cache, must-revalidate");
        res.set_header("CDN-Cache-Control", "no-cache");
        res.set_header("Cloudflare-CDN-Cache-Control", "no-cache");
    }

    if (req.get_header_value("If-None-Match") == etag) {
        res.status = 304;
        return;
    }
    res.set_content(body, contentTypeFor(path));
}

void Routes::sendJson(httplib::Response& res, int status, const nlohmann::json& body) const {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Robots-Tag", "noindex, nofollow, nosnippet");
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

std::string Routes::makeSessionCookie(const httplib::Request& req, const std::string& username) const {
    return makeCookie(req, username, 28800);
}

std::string Routes::makeCookie(const httplib::Request& req, const std::string& username, int maxAgeSeconds) const {
    return sessionCookie(req, makeSessionValue(username, maxAgeSeconds), maxAgeSeconds);
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
    logSecurityEvent(req, res, "csrf_rejected", "blocked");
    sendJson(res, 403, errorJson("invalid csrf token"));
    return false;
}

bool Routes::ensureWriteRateLimit(const httplib::Request& req, httplib::Response& res, bool creation) {
    const auto ip = clientIp(req);
    try {
        const auto writeKey = hmacHex(cfg_.sessionSecret, "rate-limit:v1:write:" + ip);
        if (!storage_.consumeRateLimit(writeKey, 240, 10 * 60)) {
            logSecurityEvent(req, res, "write_rate_limited", "blocked");
            sendJson(res, 429, errorJson("too many write requests"));
            return false;
        }
        if (creation) {
            const auto createKey = hmacHex(cfg_.sessionSecret, "rate-limit:v1:create:" + ip);
            if (!storage_.consumeRateLimit(createKey, 40, 10 * 60)) {
                logSecurityEvent(req, res, "create_rate_limited", "blocked");
                sendJson(res, 429, errorJson("too many create requests"));
                return false;
            }
        }
    } catch (const std::exception&) {
        logSecurityEvent(req, res, "rate_limit_storage", "error");
        sendJson(res, 503, errorJson("write validation temporarily unavailable"));
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

bool Routes::ensureId(const httplib::Request& req, httplib::Response& res, int index, long& out) {
    if (parseIdParam(req, index, out)) {
        return true;
    }
    sendJson(res, 400, errorJson("invalid id"));
    return false;
}

bool Routes::ensureExpectedRevision(const httplib::Request& req, httplib::Response& res,
                                    long diagramId, long& out) const {
    const auto value = req.get_header_value("If-Match");
    if (value.empty()) {
        sendJson(res, 428, errorJson("If-Match is required for this operation"));
        return false;
    }
    if (!parseDiagramEtag(value, diagramId, out)) {
        sendJson(res, 400, errorJson("If-Match must contain this diagram's current ETag"));
        return false;
    }
    return true;
}

bool Routes::ensureCanRead(const httplib::Request& req, httplib::Response& res, long diagramId, const char* what) {
    if (canReadDiagramById(req, diagramId)) {
        return true;
    }
    logSecurityEvent(req, res, "authorization_read_denied", "blocked");
    sendJson(res, 403, errorJson(std::string(what) + " is not available for this visitor"));
    return false;
}

bool Routes::ensureCanEdit(const httplib::Request& req, httplib::Response& res, long diagramId) {
    if (canEditDiagram(req, diagramId)) {
        return true;
    }
    logSecurityEvent(req, res, "authorization_write_denied", "blocked");
    sendJson(res, 403, errorJson("diagram is read-only for this visitor"));
    return false;
}

// Validation failures carry messages meant for the caller; anything escaping the
// storage layer may embed SQL text or connection strings, so it is logged and
// replaced with a generic message.
void Routes::sendStorageError(const httplib::Request& req, httplib::Response& res,
                              const std::exception& e, int status) const {
    if (const auto* conflict = dynamic_cast<const DiagramRevisionConflict*>(&e)) {
        logSecurityEvent(req, res, "revision_conflict", "blocked");
        sendJson(res, 412, {
            {"error", e.what()},
            {"current_revision", conflict->currentRevision()}
        });
        return;
    }
    if (dynamic_cast<const DiagramCapacityExceeded*>(&e) != nullptr) {
        logSecurityEvent(req, res, "global_capacity_exhausted", "blocked");
        sendJson(res, 503, errorJson(e.what()));
        return;
    }
    if (dynamic_cast<const DiagramQuotaExceeded*>(&e) != nullptr) {
        logSecurityEvent(req, res, "owner_quota_exhausted", "blocked");
        sendJson(res, 403, errorJson(e.what()));
        return;
    }
    if (dynamic_cast<const std::invalid_argument*>(&e) != nullptr) {
        sendJson(res, 400, errorJson(e.what()));
        return;
    }
    if (dynamic_cast<const pqxx::failure*>(&e) != nullptr) {
        logSecurityEvent(req, res, "storage_failure", "error");
        sendJson(res, status == 400 ? 500 : status, errorJson("storage unavailable"));
        return;
    }
    sendJson(res, status, errorJson(e.what()));
}

nlohmann::json Routes::diagramResponse(const httplib::Request& req, const Diagram& diagram) {
    auto body = diagramToJson(diagram);
    body["can_edit"] = canEditDiagram(req, diagram.id);
    body["share_url"] = cfg_.baseUrl + "/d/" + diagram.slug;
    return body;
}

void Routes::sendDiagramJson(const httplib::Request& req, httplib::Response& res,
                             int status, const Diagram& diagram) {
    res.set_header("ETag", diagramEtag(diagram));
    sendJson(res, status, diagramResponse(req, diagram));
}

void Routes::logSecurityEvent(const httplib::Request& req, const httplib::Response& res,
                              const std::string& event, const std::string& outcome) const {
    std::string username;
    std::string actor = "anonymous";
    if (verifySession(req, &username)) {
        actor = username == cfg_.authUsername ? "admin" : "guest";
    }
    if (event == "admin_login" && outcome == "succeeded") {
        actor = "admin";
    }
    auto requestId = normalizeRequestId(res.get_header_value("X-Request-ID"));
    if (requestId.empty()) {
        requestId = normalizeRequestId(req.get_header_value("X-Request-ID"));
    }
    std::cerr << logTimestamp()
              << " level=" << (outcome == "allowed" || outcome == "succeeded" ? "info" : "warn")
              << " category=security event=" << sanitizeLogField(event)
              << " outcome=" << sanitizeLogField(outcome)
              << " actor=" << actor
              << " method=" << sanitizeLogField(req.method)
              << " path=" << sanitizeLogField(redactRequestPath(req.path))
              << " ip=" << sanitizeLogField(clientIp(req))
              << " request_id=" << sanitizeLogField(requestId.empty() ? "missing" : requestId)
              << "\n";
}

bool Routes::ensureAuthenticated(const httplib::Request& req, httplib::Response& res, bool htmlResponse) {
    if (cfg_.publicAccess) {
        return true;
    }
    if (verifySession(req, nullptr)) {
        return true;
    }
    logSecurityEvent(req, res, "authentication_required", "blocked");
    if (htmlResponse) {
        res.status = 302;
        res.set_header("Location", "/login");
    } else {
        sendJson(res, 401, errorJson("authentication required"));
    }
    return false;
}

void Routes::registerRoutes(httplib::Server& server) {
    server.set_post_routing_handler([this](const httplib::Request&, httplib::Response& res) {
        applySecurityHeaders(res);
    });

    server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, {{"status", "live"}, {"service", "nuigraph-studio"}});
    });

    server.Get("/live", [this](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, {{"status", "live"}, {"service", "nuigraph-studio"}});
    });

    server.Get("/ready", [this](const httplib::Request&, httplib::Response& res) {
        try {
            if (storage_.ping()) {
                sendJson(res, 200, {{"status", "ready"}, {"service", "nuigraph-studio"}});
                return;
            }
        } catch (const std::exception&) {
            // A dependency failure is expected operational state, not a reason
            // to disclose connection details or SQL errors to a public probe.
        }
        sendJson(res, 503, {{"status", "unavailable"}, {"service", "nuigraph-studio"}});
    });

    server.Get("/robots.txt", [this](const httplib::Request&, httplib::Response& res) {
        std::string body =
            "User-agent: *\n"
            "Allow: /\n"
            "Disallow: /api/\n"
            "Disallow: /d/\n"
            "Disallow: /login\n"
            "Disallow: /logout\n"
            "Disallow: /health\n"
            "Disallow: /live\n"
            "Disallow: /ready\n"
            "Disallow: /nui/\n"
            "Disallow: /offline.html\n"
            "Sitemap: " + normalizedBaseUrl(cfg_.baseUrl) + "/sitemap.xml\n";
        res.set_header("Cache-Control", "public, max-age=3600");
        res.set_content(body, "text/plain; charset=utf-8");
    });
    server.Get("/sitemap.xml", [this](const httplib::Request&, httplib::Response& res) {
        const auto baseUrl = htmlEscape(normalizedBaseUrl(cfg_.baseUrl));
        const auto body = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") +
            "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n"
            "  <url><loc>" + baseUrl + "/</loc></url>\n"
            "  <url><loc>" + baseUrl + "/docs</loc></url>\n"
            "</urlset>\n";
        res.set_header("Cache-Control", "public, max-age=3600");
        res.set_content(body, "application/xml; charset=utf-8");
    });

    server.Get("/styles.css", [this](const httplib::Request& req, httplib::Response& res) {
        serveStatic(req, res, cfg_.projectRoot + "/public/styles.css");
    });
    server.Get("/app.js", [this](const httplib::Request& req, httplib::Response& res) {
        serveStatic(req, res, cfg_.projectRoot + "/public/app.js");
    });
    server.Get("/editor.js", [this](const httplib::Request& req, httplib::Response& res) {
        serveStatic(req, res, cfg_.projectRoot + "/public/editor.js");
    });
    server.Get(R"(/nui/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("X-Robots-Tag", "noindex, nofollow");
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
        serveStatic(req, res, path);
    });

    server.Get("/manifest.webmanifest", [this](const httplib::Request& req, httplib::Response& res) {
        serveStatic(req, res, cfg_.projectRoot + "/public/manifest.webmanifest");
    });
    server.Get("/offline.html", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("X-Robots-Tag", "noindex, nofollow");
        serveStatic(req, res, cfg_.projectRoot + "/public/offline.html");
    });
    server.Get("/offline.js", [this](const httplib::Request& req, httplib::Response& res) {
        serveStatic(req, res, cfg_.projectRoot + "/public/offline.js");
    });
    server.Get("/sw.js", [this](const httplib::Request& req, httplib::Response& res) {
        // A worker may only control the scope it is served from, so this has to
        // stay at the root rather than living under a static prefix.
        res.set_header("Service-Worker-Allowed", "/");
        // A stale worker can keep stale shell assets alive, so neither browsers
        // nor shared caches may reuse this script without fetching it again.
        serveStatic(req, res, cfg_.projectRoot + "/public/sw.js", true);
    });
    server.Get(R"(/icons/([A-Za-z0-9._-]+))", [this](const httplib::Request& req, httplib::Response& res) {
        const auto name = req.matches[1].str();
        if (name.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("bad icon path", "text/plain");
            return;
        }
        serveStatic(req, res, cfg_.projectRoot + "/public/icons/" + name);
    });

    server.Get("/login", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("X-Robots-Tag", "noindex, nofollow, noarchive");
        res.set_content(renderLoginPage(""), "text/html; charset=utf-8");
    });
    server.Post("/login", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("X-Robots-Tag", "noindex, nofollow, noarchive");
        auto ip = clientIp(req);
        const auto limiterKey = hmacHex(cfg_.sessionSecret, "rate-limit:v1:login:" + ip);
        try {
            if (!storage_.consumeRateLimit(limiterKey, 8, kLoginFailureWindow)) {
                res.status = 429;
                res.set_content(renderLoginPage("Too many login attempts. Try again later."), "text/html; charset=utf-8");
                logSecurityEvent(req, res, "admin_login", "rate_limited");
                return;
            }
        } catch (const std::exception&) {
            res.status = 503;
            res.set_content(renderLoginPage("Sign-in validation is temporarily unavailable."), "text/html; charset=utf-8");
            logSecurityEvent(req, res, "rate_limit_storage", "error");
            return;
        }
        auto username = formValue(req.body, "username");
        auto password = formValue(req.body, "password");
        if (username == cfg_.authUsername && verifyPassword(password)) {
            res.status = 302;
            res.set_header("Set-Cookie", makeSessionCookie(req, username));
            res.set_header("Location", "/");
            logSecurityEvent(req, res, "admin_login", "succeeded");
            try {
                storage_.clearRateLimit(limiterKey);
            } catch (const std::exception&) {
                logSecurityEvent(req, res, "rate_limit_storage", "clear_error");
            }
        } else {
            res.status = 401;
            res.set_content(renderLoginPage("Invalid credentials"), "text/html; charset=utf-8");
            logSecurityEvent(req, res, "admin_login", "failed");
        }
    });
    server.Post("/logout", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("X-Robots-Tag", "noindex, nofollow, noarchive");
        res.status = 302;
        res.set_header("Set-Cookie", sessionCookie(req, "", 0));
        res.set_header("Location", "/login");
        logSecurityEvent(req, res, "logout", "succeeded");
    });

    server.Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        std::string username;
        if (!cfg_.publicAccess && !verifySession(req, &username)) {
            res.status = 302;
            res.set_header("Location", "/login");
            return;
        }
        if (nuiShellReady(cfg_.projectRoot)) {
            res.set_content(nuiShellPage(cfg_.projectRoot, cfg_.baseUrl, true), "text/html; charset=utf-8");
            return;
        }
        res.set_content(renderEditorPage(username, cfg_.baseUrl), "text/html; charset=utf-8");
    });
    server.Get(R"(/d/([A-Za-z0-9-]+))", [this](const httplib::Request& req, httplib::Response& res) {
        // Share slugs are bearer capabilities. Search indexing would both leak
        // them to third parties and make revocation by obscurity impossible.
        res.set_header("X-Robots-Tag", "noindex, nofollow, noarchive");
        logSecurityEvent(req, res, "share_view", "allowed");
        if (nuiShellReady(cfg_.projectRoot)) {
            res.set_content(nuiShellPage(cfg_.projectRoot, cfg_.baseUrl, false), "text/html; charset=utf-8");
            return;
        }
        res.set_content(renderEditorPage("guest", cfg_.baseUrl, false), "text/html; charset=utf-8");
    });
    server.Get("/docs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, true)) return;
        res.set_content(renderDocsPage(cfg_.baseUrl), "text/html; charset=utf-8");
    });

    server.Get("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
        std::string username;
        if (!verifySession(req, &username)) {
            if (!cfg_.publicAccess) {
                sendJson(res, 401, errorJson("authentication required"));
                return;
            }
            username = "guest_" + randomHex(12);
            // Guest sessions are the ownership token for anything the visitor
            // creates and cannot be revoked individually, so they are kept
            // short rather than the six months this used to issue.
            auto maxAge = kGuestSessionSeconds;
            auto session = makeSessionValue(username, maxAge);
            res.set_header("Set-Cookie", sessionCookie(req, session, maxAge));
            sendJson(res, 200, {{"username", username}, {"public_access", true}, {"csrf_token", csrfTokenForSession(session)}});
            return;
        }
        auto session = cookieValue(req, "ngs_session");
        sendJson(res, 200, {{"username", username}, {"public_access", cfg_.publicAccess}, {"csrf_token", csrfTokenForSession(session)}});
    });

    // The list used to return every row. A guest is capped by the create quota,
    // but the admin view grows with the whole table, and each row costs two
    // correlated subqueries, so one request could end up counting every node and
    // edge in the database.
    server.Get("/api/diagrams", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        const long limit = clampQueryParam(req, "limit", kDefaultListLimit, 1, kMaxListLimit);
        const long offset = clampQueryParam(req, "offset", 0, 0, kMaxListOffset);
        try {
            const bool admin = isAdmin(req);
            const auto owner = admin ? std::string{} : ownerHashForRequest(req);
            auto diagrams = admin ? storage_.listDiagrams(limit, offset)
                                  : storage_.listDiagramsForOwner(owner, limit, offset);
            const long total = admin ? storage_.countDiagrams() : storage_.countDiagramsForOwner(owner);
            sendJson(res, 200, {
                {"diagrams", diagrams},
                {"total", total},
                {"limit", limit},
                {"offset", offset},
                {"has_more", offset + static_cast<long>(diagrams.size()) < total}
            });
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 500);
        }
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
            sendDiagramJson(req, res, 201, created);
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 400);
        }
    });

    server.Post("/api/diagrams", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, true)) return;
        if (!ensureCsrf(req, res)) return;
        try {
            Diagram d = req.body.empty() ? defaultDiagram() : diagramFromJson(nlohmann::json::parse(req.body));
            auto created = storage_.createDiagram(d, false, ownerHashForRequest(req));
            sendDiagramJson(req, res, 201, created);
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 400);
        }
    });

    server.Get(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        long id = 0;
        if (!ensureId(req, res, 1, id)) return;
        if (!ensureCanRead(req, res, id, "diagram")) return;
        try {
            sendDiagramJson(req, res, 200, storage_.getDiagram(id));
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
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
            sendDiagramJson(req, res, 200, diagram);
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
        }
    });

    server.Put(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        long id = 0;
        if (!ensureId(req, res, 1, id)) return;
        if (!ensureCanEdit(req, res, id)) return;
        long expectedRevision = 0;
        if (!ensureExpectedRevision(req, res, id, expectedRevision)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            auto d = diagramFromJson(body);
            auto note = body.value("note", "saved snapshot");
            sendDiagramJson(req, res, 200,
                            storage_.updateDiagram(id, d, note, false, expectedRevision));
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 400);
        }
    });

    server.Delete(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        long id = 0;
        if (!ensureId(req, res, 1, id)) return;
        if (!ensureCanEdit(req, res, id)) return;
        long expectedRevision = 0;
        if (!ensureExpectedRevision(req, res, id, expectedRevision)) return;
        try {
            storage_.deleteDiagram(id, expectedRevision);
            sendJson(res, 200, {{"ok", true}});
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 400);
        }
    });

    server.Post(R"(/api/diagrams/(\d+)/duplicate)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, true)) return;
        if (!ensureCsrf(req, res)) return;
        long id = 0;
        if (!ensureId(req, res, 1, id)) return;
        // Duplicating reads the whole source diagram and hands the caller an owned
        // copy, so it needs the same read check as GET /api/diagrams/{id}.
        if (!ensureCanRead(req, res, id, "diagram")) return;
        try {
            auto copy = storage_.duplicateDiagram(id, ownerHashForRequest(req));
            sendDiagramJson(req, res, 201, copy);
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
        }
    });

    server.Get(R"(/api/diagrams/(\d+)/versions)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        long id = 0;
        if (!ensureId(req, res, 1, id)) return;
        if (!ensureCanRead(req, res, id, "diagram versions")) return;
        try {
            nlohmann::json items = nlohmann::json::array();
            for (const auto& v : storage_.listVersions(id)) {
                items.push_back({{"id", v.id}, {"diagram_id", v.diagramId}, {"version_number", v.versionNumber}, {"created_at", v.createdAt}, {"note", v.note}});
            }
            sendJson(res, 200, {{"versions", items}});
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
        }
    });

    server.Post(R"(/api/diagrams/(\d+)/versions)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        long id = 0;
        if (!ensureId(req, res, 1, id)) return;
        if (!ensureCanEdit(req, res, id)) return;
        auto note = std::string("manual snapshot");
        if (!req.body.empty()) {
            try { note = nlohmann::json::parse(req.body).value("note", note); } catch (...) {}
        }
        try {
            auto v = storage_.createVersion(id, note);
            sendJson(res, 201, {{"id", v.id}, {"diagram_id", v.diagramId}, {"version_number", v.versionNumber}, {"created_at", v.createdAt}, {"note", v.note}});
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
        }
    });

    // A snapshot is diagram content, so this needs the same read check as GET
    // on the diagram itself, not just the weaker "may list versions".
    server.Get(R"(/api/diagrams/(\d+)/versions/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        long id = 0;
        long versionId = 0;
        if (!ensureId(req, res, 1, id) || !ensureId(req, res, 2, versionId)) return;
        if (!ensureCanRead(req, res, id, "diagram version")) return;
        try {
            sendJson(res, 200, diagramToJson(storage_.getVersion(id, versionId)));
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
        }
    });

    server.Post(R"(/api/diagrams/(\d+)/restore/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (!ensureWriteRateLimit(req, res, false)) return;
        if (!ensureCsrf(req, res)) return;
        long id = 0;
        long versionId = 0;
        if (!ensureId(req, res, 1, id) || !ensureId(req, res, 2, versionId)) return;
        if (!ensureCanEdit(req, res, id)) return;
        long expectedRevision = 0;
        if (!ensureExpectedRevision(req, res, id, expectedRevision)) return;
        try {
            sendDiagramJson(req, res, 200,
                            storage_.restoreVersion(id, versionId, expectedRevision));
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
        }
    });

    server.Get(R"(/api/diagrams/(\d+)/export\.json)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        long id = 0;
        if (!ensureId(req, res, 1, id)) return;
        if (!ensureCanRead(req, res, id, "diagram export")) return;
        try {
            auto diagram = storage_.getDiagram(id);
            res.set_header("Content-Disposition", "attachment; filename=\"diagram-" + std::to_string(id) + ".json\"");
            sendJson(res, 200, diagramToJson(diagram));
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 404);
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
            sendDiagramJson(req, res, 201, imported);
        } catch (const std::exception& e) {
            sendStorageError(req, res, e, 400);
        }
    });
}

} // namespace nuigraph
