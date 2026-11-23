// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_settings_home_screen.h"

#include "ui_callback_helpers.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "config.h"
#include "device_display_name.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "temperature_sensor_manager.h"

#include <algorithm>
#include <map>

namespace helix::settings {

using DeviceKind = HomeScreenPreferences::DeviceKind;

HomeScreenSettingsOverlay::~HomeScreenSettingsOverlay() {
    deinit_subjects();
}

void HomeScreenSettingsOverlay::init_subjects() {
    if (!init_subjects_guarded([this] {
            UI_MANAGED_SUBJECT_INT(section_, 0, "home_settings_section", subjects_);
            UI_MANAGED_SUBJECT_STRING(title_, title_buf_, lv_tr("Home screen"),
                                      "home_settings_title", subjects_);
            UI_MANAGED_SUBJECT_INT(heaters_.count, 0, "home_heater_count", subjects_);
            UI_MANAGED_SUBJECT_INT(sensors_.count, 0, "home_sensor_count", subjects_);
            UI_MANAGED_SUBJECT_INT(fans_.count, 0, "home_fan_count", subjects_);
            UI_MANAGED_SUBJECT_INT(spool_percent_, 1, "home_spool_percent", subjects_);
            UI_MANAGED_SUBJECT_INT(spool_grams_, 1, "home_spool_grams", subjects_);
            UI_MANAGED_SUBJECT_INT(spool_metres_, 1, "home_spool_metres", subjects_);
        })) {
        return;
    }
    StaticSubjectRegistry::instance().register_deinit("HomeScreenSettingsOverlay", [] {
        if (auto* overlay = lazy_global_if_exists<HomeScreenSettingsOverlay>()) {
            overlay->deinit_subjects();
        }
    });
}

void HomeScreenSettingsOverlay::deinit_subjects() {
    stop_observers();
    for (auto* group : {&heaters_, &sensors_, &fans_}) {
        group->name.reclaim();
        group->checked.reclaim();
        group->rows.clear();
    }
    deinit_subjects_base(subjects_);
}

void HomeScreenSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_home_settings_section",
         [](lv_event_t* e) {
             get_home_screen_settings_overlay().show_section(ui::event_user_int(e).value_or(0));
         }},
        {"on_home_settings_back",
         [](lv_event_t*) { get_home_screen_settings_overlay().handle_back(); }},
        {"on_home_heater_visibility_changed",
         [](lv_event_t* e) {
             get_home_screen_settings_overlay().handle_device_changed(
                 DeviceKind::Heater, ui::event_user_int(e).value_or(-1), ui::event_checked(e));
         }},
        {"on_home_sensor_visibility_changed",
         [](lv_event_t* e) {
             get_home_screen_settings_overlay().handle_device_changed(
                 DeviceKind::PassiveSensor, ui::event_user_int(e).value_or(-1),
                 ui::event_checked(e));
         }},
        {"on_home_fan_visibility_changed",
         [](lv_event_t* e) {
             get_home_screen_settings_overlay().handle_device_changed(
                 DeviceKind::Fan, ui::event_user_int(e).value_or(-1), ui::event_checked(e));
         }},
        {"on_home_spool_percent_changed",
         [](lv_event_t* e) {
             auto& overlay = get_home_screen_settings_overlay();
             lv_subject_set_int(&overlay.spool_percent_, ui::event_checked(e) ? 1 : 0);
             overlay.handle_spool_units_changed();
         }},
        {"on_home_spool_grams_changed",
         [](lv_event_t* e) {
             auto& overlay = get_home_screen_settings_overlay();
             lv_subject_set_int(&overlay.spool_grams_, ui::event_checked(e) ? 1 : 0);
             overlay.handle_spool_units_changed();
         }},
        {"on_home_spool_metres_changed",
         [](lv_event_t* e) {
             auto& overlay = get_home_screen_settings_overlay();
             lv_subject_set_int(&overlay.spool_metres_, ui::event_checked(e) ? 1 : 0);
             overlay.handle_spool_units_changed();
         }},
    });
}

void HomeScreenSettingsOverlay::before_show() {
    show_section(0);
}

void HomeScreenSettingsOverlay::show_section(int section) {
    static constexpr const char* titles[] = {"Home screen", "Temperatures", "Fans",
                                             "Filament remaining"};
    if (section < 0 || section > 3) {
        return;
    }
    lv_subject_copy_string(&title_, lv_tr(titles[section]));
    lv_subject_set_int(&section_, section);
}

void HomeScreenSettingsOverlay::handle_back() {
    if (lv_subject_get_int(&section_) != 0) {
        show_section(0);
    } else {
        close();
    }
}

void HomeScreenSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh_inventory();

    const auto units = HomeScreenPreferences(*Config::get_instance()).spool_units();
    lv_subject_set_int(&spool_percent_, units.percent ? 1 : 0);
    lv_subject_set_int(&spool_grams_, units.grams ? 1 : 0);
    lv_subject_set_int(&spool_metres_, units.metres ? 1 : 0);

    stop_observers();
    auto& printer = get_printer_state();
    auto& sensor_manager = sensors::TemperatureSensorManager::instance();
    auto token = lifetime_.token();
    const auto refresh = [token](HomeScreenSettingsOverlay* self, int) {
        if (!token.expired() && self->is_visible()) {
            self->refresh_inventory();
        }
    };
    if (sensor_manager.subjects_initialized()) {
        sensor_observer_ = ui::observe<int>(sensor_manager.revision_subject(), this, refresh,
                                            sensor_manager.subjects_lifetime());
    }
    extruder_observer_ =
        ui::observe<int>(printer.temperature_state().get_extruder_version_subject(), this, refresh,
                         printer.temperature_state().get_subjects_lifetime());
    fan_observer_ = ui::observe<int>(printer.fan_state().get_fans_version_subject(), this, refresh,
                                     printer.get_subjects_lifetime());
}

void HomeScreenSettingsOverlay::on_deactivating(DeactivateReason) {
    stop_observers();
}

void HomeScreenSettingsOverlay::stop_observers() {
    sensor_observer_.reset();
    extruder_observer_.reset();
    fan_observer_.reset();
}

HomeScreenSettingsOverlay::RowSubjects& HomeScreenSettingsOverlay::row_subjects(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Heater:
        return heaters_;
    case DeviceKind::PassiveSensor:
        return sensors_;
    case DeviceKind::Fan:
        return fans_;
    }
    return heaters_;
}

void HomeScreenSettingsOverlay::refresh_inventory() {
    auto& printer = get_printer_state();
    std::map<std::string, std::string> heaters;
    for (const auto& id : printer.get_discovery().heaters()) {
        heaters[id] = get_display_name(id, DeviceType::HEATER);
    }
    for (const auto& [id, extruder] : printer.temperature_state().extruders()) {
        heaters[id] = extruder.display_name;
    }

    std::vector<DeviceRow> passive_rows;
    for (const auto& sensor : sensors::TemperatureSensorManager::instance().get_sensors_sorted()) {
        if (sensor.type == sensors::TemperatureSensorType::HEATER_GENERIC) {
            heaters[sensor.klipper_name] = sensor.display_name;
        } else {
            passive_rows.push_back({sensor.klipper_name, sensor.display_name, sensor.enabled});
        }
    }
    std::vector<DeviceRow> heater_rows;
    const auto tile_config =
        PanelWidgetManager::instance().get_widget_config("home").get_widget_config("heater_bank");
    for (const auto& [id, name] : heaters) {
        bool fallback = true;
        if (tile_config.contains("heaters") && tile_config["heaters"].is_object()) {
            const auto it = tile_config["heaters"].find(id);
            if (it != tile_config["heaters"].end() && it->is_boolean()) {
                fallback = it->get<bool>();
            }
        }
        heater_rows.push_back({id, name, fallback});
    }
    std::vector<DeviceRow> fan_rows;
    for (const auto& fan : printer.fan_state().get_fans()) {
        fan_rows.push_back({fan.object_name, fan.display_name});
    }
    const auto by_name = [](const DeviceRow& a, const DeviceRow& b) {
        return a.name == b.name ? a.id < b.id : a.name < b.name;
    };
    std::sort(heater_rows.begin(), heater_rows.end(), by_name);
    std::sort(passive_rows.begin(), passive_rows.end(), by_name);
    std::sort(fan_rows.begin(), fan_rows.end(), by_name);
    publish_rows(heaters_, heater_rows, DeviceKind::Heater);
    publish_rows(sensors_, passive_rows, DeviceKind::PassiveSensor);
    publish_rows(fans_, fan_rows, DeviceKind::Fan);
}

void HomeScreenSettingsOverlay::publish_rows(RowSubjects& group, const std::vector<DeviceRow>& rows,
                                             DeviceKind kind) {
    // Revision subjects also change with readings. Only inventory changes need
    // new row labels; checkbox values always reflect the active printer's choices.
    if (group.rows != rows) {
        group.rows = rows;
        group.name.ensure_size(rows.size());
        group.checked.ensure_size(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            group.name.set_string(i, rows[i].name);
        }
    }
    const HomeScreenPreferences preferences(*Config::get_instance());
    for (size_t i = 0; i < rows.size(); ++i) {
        group.checked.set_int(
            i, preferences.is_visible(kind, rows[i].id, rows[i].default_visible) ? 1 : 0);
    }
    // Publish count last: repeat-created rows must find all subjects already registered.
    lv_subject_set_int(&group.count, static_cast<int>(rows.size()));
}

void HomeScreenSettingsOverlay::handle_device_changed(DeviceKind kind, int index, bool visible) {
    auto& group = row_subjects(kind);
    if (index < 0 || static_cast<size_t>(index) >= group.rows.size()) {
        return;
    }
    HomeScreenPreferences preferences(*Config::get_instance());
    preferences.set_visible(kind, group.rows[static_cast<size_t>(index)].id, visible);
    group.checked.set_int(static_cast<size_t>(index), visible ? 1 : 0);
    persist_changes();
}

void HomeScreenSettingsOverlay::handle_spool_units_changed() {
    HomeScreenPreferences preferences(*Config::get_instance());
    preferences.set_spool_units({lv_subject_get_int(&spool_percent_) != 0,
                                 lv_subject_get_int(&spool_grams_) != 0,
                                 lv_subject_get_int(&spool_metres_) != 0});
    persist_changes();
}

void HomeScreenSettingsOverlay::persist_changes() {
    if (HomeScreenPreferences(*Config::get_instance()).save()) {
        PanelWidgetManager::instance().notify_config_changed("home");
    } else {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to save settings"));
    }
}

} // namespace helix::settings
