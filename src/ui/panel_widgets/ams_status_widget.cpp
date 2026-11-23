// SPDX-License-Identifier: GPL-3.0-or-later
#include "ams_status_widget.h"

#include "ui_ams_mini_status.h"

#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

namespace helix {
void register_ams_status_widget() {
    register_widget_factory("ams",
                            [](const std::string&) { return std::make_unique<AmsStatusWidget>(); });
}

AmsStatusWidget::AmsStatusWidget() {
    picker_.set_action_callback([this](int action, int) {
        if (action < 0)
            return;
        config_["simplified"] = action == 1;
        simplified_ = action == 1;
        save_widget_config(config_);
        if (mini_)
            ui_ams_mini_status_set_simplified(mini_, simplified_);
    });
}

void AmsStatusWidget::set_config(const nlohmann::json& config) {
    config_ = config.is_object() ? config : nlohmann::json::object();
    simplified_ = config.contains("simplified") && config["simplified"].is_boolean() &&
                  config["simplified"].get<bool>();
}

void AmsStatusWidget::attach(lv_obj_t* widget, lv_obj_t* screen) {
    widget_ = widget;
    screen_ = screen;
    mini_ = widget ? lv_obj_find_by_name(widget, "ams_mini_status_widget") : nullptr;
    if (mini_)
        ui_ams_mini_status_set_simplified(mini_, simplified_);
}

void AmsStatusWidget::detach() {
    picker_.hide();
    mini_ = nullptr;
    widget_ = nullptr;
    screen_ = nullptr;
}

void AmsStatusWidget::on_activate() {
    if (mini_)
        ui_ams_mini_status_refresh(mini_);
}

void AmsStatusWidget::on_size_changed(int, int, int width, int) {
    if (mini_)
        ui_ams_mini_status_set_width(mini_, width);
}

bool AmsStatusWidget::on_edit_configure() {
    if (widget_ && screen_ && !picker_.is_visible())
        picker_.show_below_widget(screen_, widget_);
    return false;
}
} // namespace helix
