// SPDX-License-Identifier: GPL-3.0-or-later
#include "cooldown_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_temperature_utils.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

namespace helix {
void register_cooldown_widget() {
    register_widget_factory("cooldown",
                            [](const std::string&) { return std::make_unique<CooldownWidget>(); });
    lv_xml_register_event_cb(nullptr, "cooldown_widget_clicked_cb", CooldownWidget::clicked_cb);
}

CooldownWidget::CooldownWidget()
    : TiledPanelWidget("cooldown", TileSizing::Content{"", "", "Cool Down", false}) {
    UI_MANAGED_SUBJECT_INT(direction_, 0, "cooldown_layout_direction", subjects_);
    sizing_.add_subject_attr("cooldown_dir_subject", "cooldown_layout_direction");
}

void CooldownWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    // A clearly wide grant reads across; near-square cells retain the
    // column the native fitter budgets for. At 115x103 the Russian caption
    // and glyph overflow a forced row by 13px on each side.
    lv_subject_set_int(&direction_, width_px * 2 >= height_px * 3 ? 1 : 0);
    TiledPanelWidget::on_size_changed(colspan, rowspan, width_px, height_px);
}

void CooldownWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[CooldownWidget] clicked_cb");
    auto* button = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (button && !lv_obj_has_state(button, LV_STATE_DISABLED)) {
        if (auto* api = get_moonraker_api()) {
            api->execute_gcode(
                ui::temperature::resolve_cooldown_gcode(get_printer_state().temperature_state()),
                []() { ui::notify_tr(ToastSeverity::SUCCESS, TR_NOOP("Heaters off")); },
                [](const MoonrakerError& error) {
                    ui::notify_error_tr(TR_NOOP("Failed to cool down: {}"), error);
                });
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}
} // namespace helix
