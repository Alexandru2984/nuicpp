#include "App.hpp"

#include <iostream>

namespace nuigraph {

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
    routes_.registerRoutes(server);

    std::cout << "nuigraph-studio listening on " << cfg_.host << ":" << cfg_.port << "\n";
    if (!server.listen(cfg_.host, cfg_.port)) {
        std::cerr << "failed to bind " << cfg_.host << ":" << cfg_.port << "\n";
        return 3;
    }
    return 0;
}

} // namespace nuigraph
