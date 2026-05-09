#include "api/Routes.hpp"

#include "domain/Diagram.hpp"
#include "ui/NuiApp.hpp"
#include "utils/Json.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <chrono>
#include <iomanip>
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
    auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 8 * 60 * 60;
    std::string payload = username + "." + std::to_string(expiry);
    unsigned int len = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), cfg_.sessionSecret.data(), static_cast<int>(cfg_.sessionSecret.size()),
         reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, &len);
    return "ngs_session=" + payload + "." + bytesToHex(digest, len) + "; Path=/; Max-Age=28800; HttpOnly; SameSite=Strict; Secure";
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
    unsigned int len = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), cfg_.sessionSecret.data(), static_cast<int>(cfg_.sessionSecret.size()),
         reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, &len);
    auto expected = hexToBytes(bytesToHex(digest, len));
    auto actual = hexToBytes(parts[2]);
    if (!constantTimeEquals(actual, expected)) {
        return false;
    }
    if (username) *username = parts[0];
    return true;
}

bool Routes::ensureAuthenticated(const httplib::Request& req, httplib::Response& res, bool htmlResponse) {
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

    server.Get("/login", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(renderLoginPage(""), "text/html; charset=utf-8");
    });
    server.Post("/login", [this](const httplib::Request& req, httplib::Response& res) {
        auto username = formValue(req.body, "username");
        auto password = formValue(req.body, "password");
        if (username == cfg_.authUsername && verifyPassword(password)) {
            res.status = 302;
            res.set_header("Set-Cookie", makeSessionCookie(username));
            res.set_header("Location", "/");
        } else {
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
        if (!verifySession(req, &username)) {
            res.status = 302;
            res.set_header("Location", "/login");
            return;
        }
        res.set_content(renderEditorPage(username), "text/html; charset=utf-8");
    });
    server.Get("/docs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, true)) return;
        res.set_content(renderDocsPage(), "text/html; charset=utf-8");
    });

    server.Get("/api/diagrams", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        sendJson(res, 200, {{"diagrams", storage_.listDiagrams()}});
    });

    server.Post("/api/diagrams", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        try {
            Diagram d = req.body.empty() ? defaultDiagram() : diagramFromJson(nlohmann::json::parse(req.body));
            auto created = storage_.createDiagram(d, false);
            sendJson(res, 201, diagramToJson(created));
        } catch (const std::exception& e) {
            sendJson(res, 400, errorJson(e.what()));
        }
    });

    server.Get(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        try {
            sendJson(res, 200, diagramToJson(storage_.getDiagram(idParam(req, 1))));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Put(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        try {
            auto body = nlohmann::json::parse(req.body);
            auto d = diagramFromJson(body);
            auto note = body.value("note", "saved snapshot");
            sendJson(res, 200, diagramToJson(storage_.updateDiagram(idParam(req, 1), d, note, false)));
        } catch (const std::exception& e) {
            sendJson(res, 400, errorJson(e.what()));
        }
    });

    server.Delete(R"(/api/diagrams/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        storage_.deleteDiagram(idParam(req, 1));
        sendJson(res, 200, {{"ok", true}});
    });

    server.Post(R"(/api/diagrams/(\d+)/duplicate)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        try {
            sendJson(res, 201, diagramToJson(storage_.duplicateDiagram(idParam(req, 1))));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Get(R"(/api/diagrams/(\d+)/versions)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        nlohmann::json items = nlohmann::json::array();
        for (const auto& v : storage_.listVersions(idParam(req, 1))) {
            items.push_back({{"id", v.id}, {"diagram_id", v.diagramId}, {"version_number", v.versionNumber}, {"created_at", v.createdAt}, {"note", v.note}});
        }
        sendJson(res, 200, {{"versions", items}});
    });

    server.Post(R"(/api/diagrams/(\d+)/versions)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        auto note = std::string("manual snapshot");
        if (!req.body.empty()) {
            try { note = nlohmann::json::parse(req.body).value("note", note); } catch (...) {}
        }
        auto v = storage_.createVersion(idParam(req, 1), note);
        sendJson(res, 201, {{"id", v.id}, {"diagram_id", v.diagramId}, {"version_number", v.versionNumber}, {"created_at", v.createdAt}, {"note", v.note}});
    });

    server.Post(R"(/api/diagrams/(\d+)/restore/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        try {
            sendJson(res, 200, diagramToJson(storage_.restoreVersion(idParam(req, 1), idParam(req, 2))));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Get(R"(/api/diagrams/(\d+)/export\.json)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        try {
            res.set_header("Content-Disposition", "attachment; filename=\"diagram-" + req.matches[1].str() + ".json\"");
            sendJson(res, 200, diagramToJson(storage_.getDiagram(idParam(req, 1))));
        } catch (const std::exception& e) {
            sendJson(res, 404, errorJson(e.what()));
        }
    });

    server.Post("/api/diagrams/import", [this](const httplib::Request& req, httplib::Response& res) {
        if (!ensureAuthenticated(req, res, false)) return;
        if (req.body.size() > cfg_.maxImportBytes) {
            sendJson(res, 413, errorJson("import body too large"));
            return;
        }
        try {
            auto imported = storage_.createDiagram(diagramFromJson(nlohmann::json::parse(req.body)), true);
            sendJson(res, 201, diagramToJson(imported));
        } catch (const std::exception& e) {
            sendJson(res, 400, errorJson(e.what()));
        }
    });
}

} // namespace nuigraph
