// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_switcher_widget.h"

#include "app_globals.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_tool_switcher_widget() {
    register_widget_factory("tool_switcher", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<ToolSwitcherWidget>(ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "tool_pill_cb", ToolSwitcherWidget::tool_pill_cb);
    lv_xml_register_event_cb(nullptr, "tool_compact_cb", ToolSwitcherWidget::tool_compact_cb);
}

ToolSwitcherWidget::ToolSwitcherWidget(PrinterState& printer_state)
    : printer_state_(printer_state) {}

ToolSwitcherWidget::~ToolSwitcherWidget() {
    *alive_ = false;
}

void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;
}

void ToolSwitcherWidget::detach() {
    *alive_ = false;
    active_tool_observer_.reset();
    tool_count_observer_.reset();
    pill_buttons_.clear();
    picker_backdrop_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ToolSwitcherWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                         int /*height_px*/) {
    current_colspan_ = colspan;
    current_rowspan_ = rowspan;
}

void ToolSwitcherWidget::rebuild_pills() {
    // Stub — implemented in Task 4
}

void ToolSwitcherWidget::rebuild_compact() {
    // Stub — implemented in Task 5
}

void ToolSwitcherWidget::show_tool_picker() {
    // Stub — implemented in Task 5
}

void ToolSwitcherWidget::dismiss_tool_picker() {
    // Stub — implemented in Task 5
}

void ToolSwitcherWidget::handle_tool_selected(int /*tool_index*/) {
    // Stub — implemented in Task 4/5
}

void ToolSwitcherWidget::on_active_tool_changed(int /*tool_index*/) {
    // Stub — implemented in Task 4
}

void ToolSwitcherWidget::tool_pill_cb(lv_event_t* /*e*/) {
    // Stub — implemented in Task 4
}

void ToolSwitcherWidget::tool_compact_cb(lv_event_t* /*e*/) {
    // Stub — implemented in Task 5
}

} // namespace helix
