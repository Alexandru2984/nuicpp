#include "App.hpp"
#include "config/Config.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "check-config") {
        try {
            (void)nuigraph::Config::load(argv[2]);
            std::cout << "configuration valid\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "invalid configuration: " << e.what() << "\n";
            return 1;
        }
    }

    std::string envPath = "/etc/nuigraph-studio/app.env";
    if (argc > 1) {
        envPath = argv[1];
    }
    try {
        auto cfg = nuigraph::Config::load(envPath);
        return nuigraph::App(std::move(cfg)).run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
