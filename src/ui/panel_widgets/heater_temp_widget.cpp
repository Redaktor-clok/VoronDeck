// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "heater_temp_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "device_display_name.h"
#include "home_heater_visibility.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"
#include "temperature_service.h"

#include <spdlog/spdlog.h>

using namespace helix;

namespace helix {

const HeaterTempWidget::Config& nozzle_temp_config() {
    static const HeaterTempWidget::Config cfg{
        "temperature",
        "temp_btn",
        "nozzle_icon_glyph",
        "[NozzleTempWidget]",
        TempGraphOverlay::Mode::Nozzle,
        [](PrinterState& p) { return p.temperature_state().get_active_extruder_temp_subject(); },
        [](PrinterState& p) { return p.temperature_state().get_active_extruder_target_subject(); },
        HeaterType::Nozzle};
    return cfg;
}

const HeaterTempWidget::Config& bed_temp_config() {
    static const HeaterTempWidget::Config cfg{
        "bed_temperature",
        "bed_temp_btn",
        "bed_icon_glyph",
        "[BedTempWidget]",
        TempGraphOverlay::Mode::Bed,
        [](PrinterState& p) { return p.temperature_state().get_bed_temp_subject(); },
        [](PrinterState& p) { return p.temperature_state().get_bed_target_subject(); },
        HeaterType::Bed};
    return cfg;
}

const HeaterTempWidget::Config& chamber_temp_config() {
    static const HeaterTempWidget::Config cfg{
        "chamber_temperature",
        "chamber_temp_btn",
        "chamber_icon_glyph",
        "[ChamberTempWidget]",
        TempGraphOverlay::Mode::Chamber,
        [](PrinterState& p) { return p.temperature_state().get_chamber_temp_subject(); },
        [](PrinterState& p) { return p.temperature_state().get_chamber_target_subject(); },
        HeaterType::Chamber};
    return cfg;
}

// Factory + XML-callback registration. Each heater registers its own XML
// callback name against the shared HeaterTempWidget::clicked_cb.
static void register_heater_temp_widget(const char* widget_id, const char* xml_callback,
                                        const HeaterTempWidget::Config& (*config)()) {
    register_widget_factory(widget_id, [config](const std::string&) {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TemperatureService>();
        return std::make_unique<HeaterTempWidget>(ps, tcp, config());
    });
    lv_xml_register_event_cb(nullptr, xml_callback, HeaterTempWidget::clicked_cb);
}

void register_temperature_widget() {
    register_heater_temp_widget("temperature", "temp_clicked_cb", nozzle_temp_config);
    lv_xml_register_event_cb(nullptr, "home_cooldown_clicked", [](lv_event_t*) {
        if (auto* api = get_moonraker_api()) {
            const auto gcode = helix::ui::temperature::resolve_cooldown_gcode(
                get_printer_state().temperature_state());
            api->execute_gcode(
                gcode, []() {},
                [](const MoonrakerError& error) {
                    spdlog::warn("[Home] Cooldown failed: {}", error.message);
                });
        }
    });
}

void register_bed_temperature_widget() {
    register_heater_temp_widget("bed_temperature", "bed_temp_clicked_cb", bed_temp_config);
}

void register_chamber_temperature_widget() {
    register_heater_temp_widget("chamber_temperature", "chamber_temp_clicked_cb",
                                chamber_temp_config);
}

} // namespace helix

// The three heaters draw the same shape, so one worst-case budget covers them.
// temp_display draws the unit as its own label beside the value, so the budget
// carries it too: a value measured without the unit is narrower than the row
// that renders. Below 100 the reading carries a decimal, which makes "88.8" the
// widest current. No label is drawn. The nozzle glyph carries a tool digit
// whenever a second tool appears, which can happen after this tile was sized,
// so it is always budgeted. The glyph pulses while heating, so it is never
// scaled.
HeaterTempWidget::HeaterTempWidget(PrinterState& printer_state, TemperatureService* temp_panel,
                                   const Config& config)
    : TiledPanelWidget(config.widget_id,
                       TileSizing::Content{"88.8 / 888\u00B0C", "88.8\u00B0C", "Chamber", true,
                                           config.heater == HeaterType::Nozzle ? "8" : "",
                                           /*label_always_drawn=*/true, TileSizing::IconBox::Glyph,
                                           /*icon_animates=*/true}),
      printer_state_(printer_state), temp_control_panel_(temp_panel), cfg_(config) {
    const std::string subject_name = std::string(cfg_.widget_id) + "_home_visible";
    UI_MANAGED_SUBJECT_INT(home_visible_, 1, subject_name.c_str(), home_subjects_);
    sizing_.add_subject_attr("home_visible_subject", subject_name);
    const std::string name_subject = std::string(cfg_.widget_id) + "_home_name";
    UI_MANAGED_SUBJECT_STRING(home_name_, home_name_buf_, "", name_subject.c_str(), home_subjects_);
    sizing_.add_subject_attr("home_name_subject", name_subject);
    refresh_home_visibility();
}

void HeaterTempWidget::refresh_home_visibility() {
    auto* config = helix::Config::get_instance();
    const bool visible = panel_id() != "home" || !config ||
                         home_heater_visible(HomeScreenPreferences(*config),
                                             printer_state_.temperature_state(), cfg_.heater);
    lv_subject_set_int(&home_visible_, visible ? 1 : 0);
    const auto& temperatures = printer_state_.temperature_state();
    std::string object;
    switch (cfg_.heater) {
    case HeaterType::Nozzle:
        object = temperatures.active_extruder_name();
        break;
    case HeaterType::Bed:
        object = "heater_bed";
        break;
    case HeaterType::Chamber:
        object = temperatures.chamber_temperature_source();
        break;
    }
    const std::string label = get_display_name(object, DeviceType::HEATER);
    lv_subject_copy_string(&home_name_, label.c_str());
}

HeaterTempWidget::~HeaterTempWidget() {
    detach();
}

void HeaterTempWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    refresh_home_visibility();
    auto refresh = [](HeaterTempWidget* self, int) { self->refresh_home_visibility(); };
    auto& temperatures = printer_state_.temperature_state();
    home_extruder_observer_ =
        helix::ui::observe<int>(temperatures.get_active_extruder_temp_subject(), this, refresh,
                                temperatures.get_subjects_lifetime());
    auto& sensor_manager = sensors::TemperatureSensorManager::instance();
    if (sensor_manager.subjects_initialized())
        home_inventory_observer_ = helix::ui::observe<int>(
            sensor_manager.revision_subject(), this, refresh, sensor_manager.subjects_lifetime());

    temp_btn_ = lv_obj_find_by_name(widget_obj_, cfg_.button_name);
    if (temp_btn_) {
        lv_obj_add_event_cb(temp_btn_, clicked_cb, LV_EVENT_CLICKED, this);
    }

    // Bind the heating icon animator. The binder owns its own temperature
    // observers, so this widget no longer needs temp_observer_/target_observer_
    // just to feed an icon tint.
    icon_binder_.bind(widget_obj_, printer_state_, cfg_.heater);

    spdlog::debug("{} Attached", cfg_.log_tag);
}

void HeaterTempWidget::detach() {
    lifetime_.invalidate();
    home_extruder_observer_.reset();
    home_inventory_observer_.reset();
    icon_binder_.unbind();

    temp_btn_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("{} Detached", cfg_.log_tag);
}

void HeaterTempWidget::handle_temp_clicked() {
    spdlog::info("{} Temperature icon clicked - opening temp graph overlay", cfg_.log_tag);
    get_global_temp_graph_overlay().open(cfg_.mode, parent_screen_);
}

void HeaterTempWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HeaterTempWidget] clicked_cb");
    auto* self = static_cast<HeaterTempWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_temp_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}
