#pragma once

#include "api/Routes.hpp"
#include "config/Config.hpp"
#include "observability/Metrics.hpp"
#include "storage/PostgresStorage.hpp"
#include "validation/DiagramValidator.hpp"

#include <httplib.h>

namespace nuigraph {

class App {
public:
    explicit App(Config cfg);
    int run();

private:
    Config cfg_;
    Metrics metrics_;
    DiagramValidator validator_;
    PostgresStorage storage_;
    Routes routes_;
};

} // namespace nuigraph
