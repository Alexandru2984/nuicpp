#include "App.hpp"
#include "config/Config.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    std::string envPath = "/home/micu/nuicpp/.env";
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
