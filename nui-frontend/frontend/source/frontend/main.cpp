#include <frontend/main_page.hpp>

#include <nui/core.hpp>
#include <nui/frontend/bindings.hpp>
#include <nui/frontend/dom/dom.hpp>
#include <nui/window.hpp>

#include <emscripten/bind.h>
#include <memory>

static std::unique_ptr<MainPage> mainPage{};
static std::unique_ptr<Nui::Dom::Dom> dom{};

extern "C" void frontendMain() {
    mainPage = std::make_unique<MainPage>();
    dom = std::make_unique<Nui::Dom::Dom>();
    dom->setBody(mainPage->render());
}

EMSCRIPTEN_BINDINGS(nuigraph_nui_frontend) {
    emscripten::function("main", &frontendMain);
}
