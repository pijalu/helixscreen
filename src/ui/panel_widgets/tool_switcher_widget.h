// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget.h"
#include "ui_observer_guard.h"
#include <memory>
#include <vector>

namespace helix {

class PrinterState;

class ToolSwitcherWidget : public PanelWidget {
  public:
    explicit ToolSwitcherWidget(PrinterState& printer_state);
    ~ToolSwitcherWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override { return "tool_switcher"; }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

  private:
    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* picker_backdrop_ = nullptr;

    int current_colspan_ = 1;
    int current_rowspan_ = 1;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    ObserverGuard active_tool_observer_;
    ObserverGuard tool_count_observer_;

    std::vector<lv_obj_t*> pill_buttons_;

    void rebuild_pills();
    void rebuild_compact();
    void show_tool_picker();
    void dismiss_tool_picker();
    void handle_tool_selected(int tool_index);
    void on_active_tool_changed(int tool_index);

  public:
    static void tool_pill_cb(lv_event_t* e);
    static void tool_compact_cb(lv_event_t* e);
};

void register_tool_switcher_widget();

} // namespace helix
