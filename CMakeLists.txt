cmake_minimum_required(VERSION 3.20)

project(NuiGraphStudio VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(nlohmann_json REQUIRED)
pkg_check_modules(PQXX REQUIRED "libpqxx>=7.10")
pkg_check_modules(CPPHTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)

# libpqxx 7.10 documents a shared-library double-free involving its header-
# defined type_name<T> strings. Link its distribution-provided static archive
# so those instances and their destructors live in one image. The remaining
# libraries stay dynamically linked and receive normal security updates.
find_file(PQXX_STATIC_LIBRARY
    NAMES libpqxx.a
    HINTS ${PQXX_LIBRARY_DIRS}
    REQUIRED
)

add_executable(nuigraph-studio
    src/main.cpp
    src/App.cpp
    src/domain/Diagram.cpp
    src/domain/Templates.cpp
    src/observability/Metrics.cpp
    src/config/Config.cpp
    src/ui/NuiApp.cpp
    src/validation/DiagramValidator.cpp
    src/storage/PostgresStorage.cpp
    src/storage/ConnectionPool.cpp
    src/api/Routes.cpp
    src/utils/Json.cpp
    src/utils/RateLimiter.cpp
    src/utils/Security.cpp
)

target_include_directories(nuigraph-studio PRIVATE src ${PQXX_INCLUDE_DIRS})
target_link_libraries(nuigraph-studio PRIVATE
    ${PQXX_STATIC_LIBRARY}
    PostgreSQL::PostgreSQL
    PkgConfig::CPPHTTPLIB
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
)

target_compile_options(nuigraph-studio PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(nuigraph-tests
    tests/test_validation.cpp
    src/domain/Diagram.cpp
    src/domain/Templates.cpp
    src/observability/Metrics.cpp
    src/config/Config.cpp
    src/ui/NuiApp.cpp
    src/validation/DiagramValidator.cpp
    src/utils/Json.cpp
    src/utils/RateLimiter.cpp
    src/utils/Security.cpp
)

target_include_directories(nuigraph-tests PRIVATE src)
target_link_libraries(nuigraph-tests PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)
target_compile_options(nuigraph-tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME nuigraph-validation COMMAND nuigraph-tests)
add_test(NAME nuigraph-frontend COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_frontend.sh)
add_test(NAME nuigraph-browser-e2e COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_browser_e2e.sh)
add_test(NAME nuigraph-nginx-config COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_nginx_config.sh)
add_test(NAME nuigraph-prometheus-rules COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_prometheus_rules.sh)
add_test(NAME nuigraph-incident-response COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_incident_response.sh)
add_test(NAME nuigraph-sbom COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_sbom.sh)
add_test(NAME nuigraph-secret-scan COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_secret_scanner.sh)
add_test(NAME nuigraph-openapi COMMAND node ${CMAKE_SOURCE_DIR}/tests/check_openapi.cjs)
add_test(NAME nuigraph-security-smoke COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_security_smoke.sh)
add_test(NAME nuigraph-api-integration COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_api_integration.sh)
add_test(NAME nuigraph-rate-limits COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_rate_limits.sh)
add_test(NAME nuigraph-global-quota COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_global_quota.sh)
add_test(NAME nuigraph-logical-quota COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_logical_quota.sh)
add_test(NAME nuigraph-key-rotation COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_key_rotation.sh)
add_test(NAME nuigraph-migrations COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_migrations.sh)
set_tests_properties(
    nuigraph-frontend
    nuigraph-nginx-config
    nuigraph-prometheus-rules
    nuigraph-incident-response
    nuigraph-sbom
    nuigraph-secret-scan
    nuigraph-openapi
    PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(nuigraph-prometheus-rules PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(
    nuigraph-security-smoke
    nuigraph-browser-e2e
    nuigraph-api-integration
    nuigraph-rate-limits
    nuigraph-global-quota
    nuigraph-logical-quota
    nuigraph-key-rotation
    nuigraph-migrations
    PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        SKIP_RETURN_CODE 77
)
