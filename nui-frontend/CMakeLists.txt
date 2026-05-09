cmake_minimum_required(VERSION 3.21)

project(nuigraph-nui-frontend VERSION 1.0.0 LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/_cmake/dependencies/nui.cmake)
set(NUI_MODULE_BUILD_DIR ${CMAKE_BINARY_DIR} CACHE INTERNAL "Nui standalone frontend build dir")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/package.json ${CMAKE_BINARY_DIR}/package.json COPYONLY)

add_executable(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend/source/frontend/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend/source/frontend/main_page.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE frontend/include)
target_link_libraries(${PROJECT_NAME} PRIVATE nui-frontend)

set(PREJS_FILE ${CMAKE_SOURCE_DIR}/frontend/source/frontend/js/module.js)
set(PARCEL_ARGS "--no-source-maps")

nui_prepare_emscripten_target(
    TARGET
        ${PROJECT_NAME}
    NO_INLINE
    PARCEL_ARGS
        ${PARCEL_ARGS}
    NPM_INSTALL_ARGS
    PREJS
        ${PREJS_FILE}
    EMSCRIPTEN_LINK_OPTIONS
        -sEXPORTED_FUNCTIONS=_frontendMain
        -sALLOW_MEMORY_GROWTH=1
        -sWASM_BIGINT
        -fexceptions
        -O2
    EMSCRIPTEN_COMPILE_OPTIONS
        -fexceptions
        -O2
)
