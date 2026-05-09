#include <frontend/main_page.hpp>

#include <nui/frontend/attributes.hpp>
#include <nui/frontend/elements.hpp>
#include <nui/frontend/svg.hpp>

Nui::ElementRenderer MainPage::render() {
    using namespace Nui;
    using namespace Nui::Attributes;
    using Nui::Elements::a;
    using Nui::Elements::aside;
    using Nui::Elements::body;
    using Nui::Elements::button;
    using Nui::Elements::div;
    using Nui::Elements::form;
    using Nui::Elements::h2;
    using Nui::Elements::header;
    using Nui::Elements::input;
    using Nui::Elements::label;
    using Nui::Elements::main_;
    using Nui::Elements::nav;
    using Nui::Elements::option;
    using Nui::Elements::section;
    using Nui::Elements::select;
    using Nui::Elements::span;
    using Nui::Elements::strong;

    namespace se = Nui::Elements::Svg;
    namespace sa = Nui::Attributes::Svg;

    return body{}(
        div{id = "app"}(
            header{class_ = "topbar"}(
                div{class_ = "app-title"}(
                    strong{}("NuiGraph Studio"),
                    input{id = "diagram-title", maxLength = "120", value = "Untitled diagram", AttributeFactory("aria-label") = "Diagram name"}()
                ),
                div{class_ = "top-actions"}(
                    button{id = "new-diagram", title = "New diagram"}("New"),
                    button{id = "save-diagram", title = "Save"}("Save"),
                    button{id = "undo", title = "Undo"}("Undo"),
                    button{id = "redo", title = "Redo"}("Redo"),
                    button{id = "export-json", title = "Export JSON"}("Export"),
                    button{id = "import-json", title = "Import JSON"}("Import"),
                    button{id = "zoom-out", title = "Zoom out"}("-"),
                    span{id = "zoom-label"}("100%"),
                    button{id = "zoom-in", title = "Zoom in"}("+"),
                    a{href = "/docs"}("Docs"),
                    form{method = "post", action = "/logout"}(button{type = "submit"}("Logout"))
                )
            ),
            aside{class_ = "project-panel"}(
                div{class_ = "panel-head"}(
                    h2{}("Diagrams"),
                    button{id = "refresh-diagrams", title = "Refresh"}("Refresh")
                ),
                div{id = "diagram-list", class_ = "diagram-list"}()
            ),
            nav{class_ = "toolbox"}(
                button{AttributeFactory("data-tool") = "select", class_ = "active"}("Select"),
                button{AttributeFactory("data-tool") = "pan"}("Pan"),
                button{AttributeFactory("data-tool") = "node"}("Node"),
                button{AttributeFactory("data-tool") = "edge"}("Edge"),
                button{id = "delete-selected"}("Delete"),
                select{id = "node-type", AttributeFactory("aria-label") = "Node type"}(
                    option{value = "process"}("Process"),
                    option{value = "decision"}("Decision"),
                    option{value = "database"}("Database"),
                    option{value = "service"}("Service"),
                    option{value = "api"}("API"),
                    option{value = "note"}("Note"),
                    option{value = "external"}("External")
                )
            ),
            main_{class_ = "canvas-wrap"}(
                se::svg{id = "canvas", role = "application", AttributeFactory("aria-label") = "Diagram canvas"}(
                    se::defs{}(
                        se::pattern{id = "grid", sa::width = "32", sa::height = "32", sa::patternUnits = "userSpaceOnUse"}(
                            se::path{sa::d = "M 32 0 L 0 0 0 32", sa::fill = "none", sa::stroke = "rgba(148,163,184,.18)", sa::strokeWidth = "1"}()
                        ),
                        se::marker{id = "arrow", sa::viewBox = "0 0 10 10", sa::refX = "9", sa::refY = "5", sa::markerWidth = "8", sa::markerHeight = "8", sa::orient = "auto-start-reverse"}(
                            se::path{sa::d = "M 0 0 L 10 5 L 0 10 z", sa::fill = "#94a3b8"}()
                        )
                    ),
                    se::rect{id = "grid-bg", sa::x = "-50000", sa::y = "-50000", sa::width = "100000", sa::height = "100000", sa::fill = "url(#grid)"}(),
                    se::g{id = "viewport"}(
                        se::g{id = "edges"}(),
                        se::g{id = "nodes"}()
                    )
                )
            ),
            aside{class_ = "properties"}(
                h2{}("Properties"),
                div{id = "selection-empty", class_ = "muted"}("No selection"),
                div{id = "node-props", class_ = "prop-block hidden"}(
                    label{}(span{}("Title"), input{id = "prop-title", maxLength = "160"}()),
                    label{}(span{}("Type"), select{id = "prop-type"}()),
                    label{}(span{}("Color"), input{id = "prop-color", type = "color"}()),
                    label{}(span{}("Width"), input{id = "prop-width", type = "number", min = "72", max = "600"}()),
                    label{}(span{}("Height"), input{id = "prop-height", type = "number", min = "48", max = "400"}())
                ),
                div{id = "edge-props", class_ = "prop-block hidden"}(
                    label{}(span{}("Label"), input{id = "edge-label", maxLength = "160"}()),
                    label{}(span{}("Color"), input{id = "edge-color", type = "color"}()),
                    label{class_ = "check-row"}(input{id = "edge-directed", type = "checkbox"}(), span{}(" Directed"))
                ),
                section{class_ = "versions"}(
                    div{class_ = "panel-head"}(
                        h2{}("Versions"),
                        button{id = "make-version"}("Snapshot")
                    ),
                    div{id = "version-list", class_ = "version-list"}()
                )
            )
        ),
        input{id = "import-file", type = "file", accept = "application/json,.json", hidden = true}()
    );
}
