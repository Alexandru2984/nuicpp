#include "App.hpp"
#include "utils/Security.hpp"

#include <atomic>
#include <csignal>
#include <iostream>

namespace nuigraph {
namespace {

// The signal handler may only touch the server through an async-signal-safe
// path, so it stops the listener via this pointer and nothing else.
std::atomic<httplib::Server*> g_server{nullptr};

extern "C" void handleStopSignal(int) {
    if (auto* server = g_server.load()) {
        server->stop();
    }
}

} // namespace

App::App(Config cfg)
    : cfg_(std::move(cfg)),
      validator_(cfg_),
      storage_(cfg_, validator_),
      routes_(cfg_, storage_) {}

int App::run() {
    if (!storage_.ping()) {
        std::cerr << "database ping failed\n";
        return 2;
    }

    httplib::Server server;
    server.set_read_timeout(10, 0);
    server.set_write_timeout(10, 0);
    server.set_idle_interval(1, 0);
    server.set_payload_max_length(cfg_.maxImportBytes + 65536);

    server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        auto requestId = normalizeRequestId(req.get_header_value("X-Request-ID"));
        if (requestId.empty()) {
            requestId = generateRequestId();
        }
        res.set_header("X-Request-ID", requestId);
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Without this an uncaught exception in a handler tears the connection down
    // with no record of what happened.
    server.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception&) {
        } catch (...) {
        }
        std::cerr << logTimestamp() << " level=error event=handler_exception method="
                  << sanitizeLogField(req.method)
                  << " path=" << sanitizeLogField(redactRequestPath(req.path))
                  << " request_id=" << sanitizeLogField(res.get_header_value("X-Request-ID"))
                  << "\n";
        res.status = 500;
        res.set_header("Cache-Control", "no-store");
        res.set_content("{\"error\":\"internal error\"}", "application/json");
    });

    // One line per request. There was no access log at all, so abuse could not
    // be investigated after the fact from anything the service itself recorded.
    server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        // Behind nginx this is the visitor address; a direct caller has no such
        // header, so fall back to the peer rather than logging a blank field.
        auto ip = req.get_header_value("X-Real-IP");
        if (ip.empty()) {
            ip = req.remote_addr;
        }
        std::cout << logTimestamp()
                  << " level=info method=" << sanitizeLogField(req.method)
                  << " path=" << sanitizeLogField(redactRequestPath(req.path))
                  << " status=" << res.status
                  << " ip=" << sanitizeLogField(ip)
                  << " request_id=" << sanitizeLogField(res.get_header_value("X-Request-ID"))
                  << " len=" << res.body.size() << "\n";
    });

    routes_.registerRoutes(server);

    g_server.store(&server);
    // systemd sends SIGTERM on stop and restart. The default disposition kills
    // the process mid-response and drops every in-flight request; stopping the
    // listener lets the current ones finish.
    std::signal(SIGTERM, handleStopSignal);
    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << logTimestamp() << " level=info msg=listening host=" << cfg_.host
              << " port=" << cfg_.port << " pool=" << cfg_.dbPoolSize
              << " frontend=" << frontendShellName(cfg_.frontendShell) << std::endl;

    if (!server.listen(cfg_.host, cfg_.port)) {
        std::cerr << "failed to bind " << cfg_.host << ":" << cfg_.port << "\n";
        g_server.store(nullptr);
        return 3;
    }

    g_server.store(nullptr);
    std::cout << logTimestamp() << " level=info msg=stopped_cleanly" << std::endl;
    return 0;
}

} // namespace nuigraph
