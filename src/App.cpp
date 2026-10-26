#include "App.hpp"
#include "utils/Security.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

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
      metrics_(),
      validator_(cfg_),
      storage_(cfg_, validator_),
      routes_(cfg_, storage_, metrics_) {
    metrics_.configureTrashRetention(
        cfg_.trashRetentionDays, cfg_.trashPurgeIntervalSeconds);
}

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
    server.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
        // Behind nginx this is the visitor address; a direct caller has no such
        // header, so fall back to the peer rather than logging a blank field.
        auto ip = req.get_header_value("X-Real-IP");
        if (ip.empty()) {
            ip = req.remote_addr;
        }
        double durationSeconds = 0.0;
        if (req.start_time_ != (std::chrono::steady_clock::time_point::min)()) {
            durationSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - req.start_time_).count();
        }
        metrics_.observeHttp(req.method, res.status, durationSeconds);
        std::cout << logTimestamp()
                  << " level=info method=" << sanitizeLogField(req.method)
                  << " path=" << sanitizeLogField(redactRequestPath(req.path))
                  << " status=" << res.status
                  << " ip=" << sanitizeLogField(ip)
                  << " request_id=" << sanitizeLogField(res.get_header_value("X-Request-ID"))
                  << " len=" << res.body.size() << "\n";
    });

    routes_.registerRoutes(server);

    std::jthread trashPurgeWorker;
    if (cfg_.trashRetentionDays > 0) {
        trashPurgeWorker = std::jthread([this, &server](std::stop_token stopToken) {
            // Never mutate retained data if the listener failed to bind. Once
            // the service is live, the database row coordinates the interval
            // across all application instances.
            while (!stopToken.stop_requested() && !server.is_running()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            while (!stopToken.stop_requested()) {
                try {
                    const auto result = storage_.purgeExpiredTrash(
                        cfg_.trashRetentionDays,
                        cfg_.trashPurgeBatchSize,
                        cfg_.trashPurgeIntervalSeconds);
                    if (result.claimed) {
                        metrics_.recordTrashPurge(result.deleted);
                        std::cout << logTimestamp()
                                  << " level=info category=maintenance event=trash_purge"
                                  << " outcome=succeeded deleted=" << result.deleted
                                  << " retention_days=" << cfg_.trashRetentionDays
                                  << std::endl;
                    }
                } catch (const std::exception&) {
                    metrics_.recordTrashPurgeError();
                    std::cerr << logTimestamp()
                              << " level=error category=maintenance event=trash_purge"
                              << " outcome=error" << std::endl;
                }

                for (int elapsed = 0;
                     elapsed < cfg_.trashPurgeIntervalSeconds &&
                     !stopToken.stop_requested();
                     ++elapsed) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        });
    }

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
        if (trashPurgeWorker.joinable()) {
            trashPurgeWorker.request_stop();
            trashPurgeWorker.join();
        }
        return 3;
    }

    g_server.store(nullptr);
    if (trashPurgeWorker.joinable()) {
        trashPurgeWorker.request_stop();
        trashPurgeWorker.join();
    }
    std::cout << logTimestamp() << " level=info msg=stopped_cleanly" << std::endl;
    return 0;
}

} // namespace nuigraph
