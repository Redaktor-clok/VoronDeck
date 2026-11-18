// SPDX-License-Identifier: GPL-3.0-or-later
#include "home_sensor_bank_widget.h"

#include "config.h"
#include "home_screen_preferences.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "temperature_sensor_manager.h"
#include "ui_temperature_utils.h"

#include <cmath>
#include <memory>

namespace helix {

void register_home_sensor_bank_widget() {
    register_widget_factory("home_sensor_bank", [](const std::string&) {
        return std::make_unique<HomeSensorBankWidget>();
    });
}

HomeSensorBankWidget::HomeSensorBankWidget()
    : prefix_("home_sensor_bank_sensor"), count_name_("home_sensor_bank_sensor_count"),
      names_(prefix_.c_str(), xml::IndexedSubjectPool::Type::String, 96, "_name"),
      temperatures_(prefix_.c_str(), xml::IndexedSubjectPool::Type::String, 24, "_temp") {
    UI_MANAGED_SUBJECT_INT(count_, 0, count_name_.c_str(), subjects_);
    // This widget is single-instance. Structural repeat counts resolve literal
    // subject names before component API props are substituted by the engine.
    attrs_ = {"prefix", prefix_.c_str(), nullptr};
    refresh();
}

HomeSensorBankWidget::~HomeSensorBankWidget() {
    detach();
}

void HomeSensorBankWidget::attach(lv_obj_t*, lv_obj_t*) {
    attached_ = true;
    refresh();
    auto& manager = sensors::TemperatureSensorManager::instance();
    if (!manager.subjects_initialized())
        return;
    auto token = lifetime_.token();
    revision_observer_ = ui::observe<int>(
        manager.revision_subject(), this,
        [token](HomeSensorBankWidget* self, int) {
            token.defer("HomeSensorBankWidget::refresh", [self] {
                if (self->attached_) {
                    self->refresh();
                }
            });
        },
        manager.subjects_lifetime());
}

void HomeSensorBankWidget::detach() {
    attached_ = false;
    revision_observer_.reset();
}

void HomeSensorBankWidget::on_activate() {
    refresh();
}

void HomeSensorBankWidget::refresh() {
    auto& manager = sensors::TemperatureSensorManager::instance();
    auto inventory = manager.get_sensors_sorted();
    auto* config = Config::get_instance();
    size_t count = 0;
    for (const auto& sensor : inventory) {
        if (sensor.type == sensors::TemperatureSensorType::HEATER_GENERIC) {
            continue;
        }
        if (config && !HomeScreenPreferences(*config).is_visible(
                          HomeScreenPreferences::DeviceKind::PassiveSensor, sensor.klipper_name,
                          sensor.enabled)) {
            continue;
        }
        names_.ensure_size(count + 1);
        temperatures_.ensure_size(count + 1);
        // Context already says temperature. Compact only stock wording here;
        // keep the configured display name unchanged in settings/control views.
        std::string label = sensor.display_name;
        const std::string suffix = " Temperature";
        const auto suffix_at = label.find(suffix);
        if (suffix_at != std::string::npos)
            label.erase(suffix_at, suffix.size());
        if (label == "Raspberry Pi")
            label = "Host";
        const std::string driver_prefix = "Stepper Driver ";
        if (label.rfind(driver_prefix, 0) == 0)
            label = "Driver " + label.substr(driver_prefix.size());
        names_.set_string(count, label);
        const auto reading = manager.get_sensor_state(sensor.klipper_name);
        char text[24];
        if (reading && reading->available && std::isfinite(reading->temperature)) {
            ui::temperature::format_temperature(static_cast<int>(std::lround(reading->temperature)),
                                                text, sizeof(text));
            temperatures_.set_string(count, text);
        } else {
            temperatures_.set_string(count, "--");
        }
        ++count;
    }
    // Publishing last lets reactive XML create rows with every binding in place.
    lv_subject_set_int(&count_, static_cast<int>(count));
}

} // namespace helix
