cmake_minimum_required(VERSION 3.20)

project(NuiGraphStudio VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(CPPHTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)

add_executable(nuigraph-studio
    src/main.cpp
    src/App.cpp
    src/domain/Diagram.cpp
    src/domain/Templates.cpp
    src/config/Config.cpp
    src/validation/DiagramValidator.cpp
    src/storage/PostgresStorage.cpp
    src/api/Routes.cpp
    src/ui/NuiApp.cpp
    src/utils/Json.cpp
    src/utils/RateLimiter.cpp
)

target_include_directories(nuigraph-studio PRIVATE src)
target_link_libraries(nuigraph-studio PRIVATE
    PkgConfig::PQXX
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
    src/config/Config.cpp
    src/validation/DiagramValidator.cpp
    src/utils/Json.cpp
    src/utils/RateLimiter.cpp
)

target_include_directories(nuigraph-tests PRIVATE src)
target_link_libraries(nuigraph-tests PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(nuigraph-tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME nuigraph-validation COMMAND nuigraph-tests)
add_test(NAME nuigraph-frontend COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_frontend.sh)
add_test(NAME nuigraph-security-smoke COMMAND bash ${CMAKE_SOURCE_DIR}/tests/check_security_smoke.sh)
set_tests_properties(nuigraph-frontend PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(nuigraph-security-smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
