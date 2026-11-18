// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget.h"
#include "spool_display_picker.h"
#include "ui_widget_ref.h"

namespace helix {

/// Home-only wrapper: presentation selection stays out of shared AMS controls.
class AmsStatusWidget : public PanelWidget {
  public:
    AmsStatusWidget();
    ~AmsStatusWidget() override { detach(); }
    const char* id() const override { return "ams"; }
    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget, lv_obj_t* screen) override;
    void detach() override;
    void on_activate() override;
    void on_size_changed(int, int, int width, int) override;
    bool has_edit_configure() const override { return true; }
    bool on_edit_configure() override;
  private:
    nlohmann::json config_ = nlohmann::json::object();
    bool simplified_ = false;
    ui::WidgetRef widget_;
    ui::WidgetRef screen_;
    ui::WidgetRef mini_;
    SpoolDisplayPicker picker_;
};

} // namespace helix
