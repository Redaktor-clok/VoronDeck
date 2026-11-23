// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_wizard_devices.h"

#include "ui_error_reporting.h"
#include "ui_wizard.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "hardware_role_registry.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "i_moonraker_api.h"
#include "printer_discovery.h"
#include "static_panel_registry.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <set>

using namespace helix;

namespace {
const char* sensor_key(HardwareRoleId role) {
    if (role == HardwareRoleId::BedHeater)
        return wizard::BED_SENSOR;
    if (role == HardwareRoleId::HotendHeater)
        return wizard::HOTEND_SENSOR;
    return nullptr;
}
const char* role_label(HardwareRoleId role) {
    switch (role) {
    case HardwareRoleId::HotendHeater:
        return "Hotend";
    case HardwareRoleId::BedHeater:
        return "Heated Bed";
    case HardwareRoleId::PartFan:
        return "Part Cooling Fan";
    case HardwareRoleId::HotendFan:
        return "Hotend Fan";
    case HardwareRoleId::ChamberFan:
        return "Chamber Fan";
    case HardwareRoleId::ExhaustFan:
        return "Exhaust Fan";
    case HardwareRoleId::AuxFan:
        return "Auxiliary Fan";
    }
    return "Unknown";
}
void append_line(std::string& text, const std::string& line) {
    if (!text.empty())
        text += "\n";
    text += line;
}
std::string list_names(const std::vector<std::string>& names) {
    std::string text;
    for (const auto& name : names)
        append_line(text, name);
    return text.empty() ? lv_tr("Not detected") : text;
}
} // namespace

std::vector<WizardDeviceDefault> helix::wizard_device_defaults(const Config& config,
                                                               const PrinterDiscovery& hardware) {
    std::vector<WizardDeviceDefault> result;
    if (!hardware.objects_reported())
        return result;
    const std::string prefix = config.df();
    std::set<std::string> used_fans;
    for (const auto& role : hardware_role_registry()) {
        if (role.category == HardwareCategory::Fan && config.exists(prefix + role.config_key))
            used_fans.insert(config.get<std::string>(prefix + role.config_key, ""));
    }
    for (const auto& role : hardware_role_registry()) {
        const auto& objects =
            role.category == HardwareCategory::Fan ? hardware.fans() : hardware.heaters();
        const std::string key = prefix + role.config_key;
        const bool saved = config.exists(key);
        std::string chosen = saved ? config.get<std::string>(key, "") : "";
        const char* sensor = sensor_key(role.id);
        if (!saved) {
            if (sensor && config.exists(prefix + sensor)) {
                // A pre-existing sensor (even None) must not acquire a conflicting heater.
                chosen = config.get<std::string>(prefix + sensor, "");
                if (chosen.empty() ||
                    resolve_role(role, chosen, objects).status != RoleResolutionStatus::Resolved)
                    continue;
            } else {
                chosen = resolve_role(role, "", objects).object;
            }
            if (chosen.empty() ||
                (role.category == HardwareCategory::Fan && used_fans.count(chosen)))
                continue;
            result.push_back({role.config_key, chosen});
            if (role.category == HardwareCategory::Fan)
                used_fans.insert(chosen);
        }
        if (sensor && !chosen.empty() && !config.exists(prefix + sensor) &&
            std::find(objects.begin(), objects.end(), chosen) != objects.end())
            result.push_back({sensor, chosen});
    }
    return result;
}

bool helix::apply_wizard_device_defaults(Config& config,
                                         const std::vector<WizardDeviceDefault>& defaults) {
    bool changed = false;
    for (const auto& item : defaults) {
        const auto path = config.df() + item.key;
        if (!config.exists(path)) {
            bool conflicts = false;
            for (const auto& role : hardware_role_registry()) {
                const char* sensor = sensor_key(role.id);
                const char* counterpart = sensor && item.key == role.config_key ? sensor
                                          : sensor && item.key == sensor        ? role.config_key
                                                                                : nullptr;
                if (counterpart && config.exists(config.df() + counterpart) &&
                    config.get<std::string>(config.df() + counterpart, "") != item.value)
                    conflicts = true;
                if (role.category == HardwareCategory::Fan && item.key.rfind("fans/", 0) == 0 &&
                    config.exists(config.df() + role.config_key) &&
                    config.get<std::string>(config.df() + role.config_key, "") == item.value)
                    conflicts = true;
            }
            if (conflicts)
                continue;
            config.set<std::string>(path, item.value);
            changed = true;
        }
    }
    return changed;
}

WizardDevicesStep* get_wizard_devices_step() {
    return &lazy_global<WizardDevicesStep>("WizardDevicesStep");
}

void WizardDevicesStep::register_callbacks() {
    using wizard::StepId;
    lv_xml_register_event_cb(nullptr, "on_wizard_devices_heaters", [](lv_event_t*) {
        ui_wizard_open_device_editor(StepId::HeaterSelect);
    });
    lv_xml_register_event_cb(nullptr, "on_wizard_devices_fans",
                             [](lv_event_t*) { ui_wizard_open_device_editor(StepId::FanSelect); });
    lv_xml_register_event_cb(nullptr, "on_wizard_devices_ams", [](lv_event_t*) {
        ui_wizard_open_device_editor(StepId::AmsIdentify);
    });
    lv_xml_register_event_cb(nullptr, "on_wizard_devices_leds",
                             [](lv_event_t*) { ui_wizard_open_device_editor(StepId::LedSelect); });
    lv_xml_register_event_cb(nullptr, "on_wizard_devices_sensors", [](lv_event_t*) {
        ui_wizard_open_device_editor(StepId::FilamentSensor);
    });
}

lv_obj_t* WizardDevicesStep::create(lv_obj_t* parent) {
    auto* config = Config::get_instance();
    auto* api = get_moonraker_api();
    const PrinterDiscovery empty;
    const auto& hardware = api ? api->hardware() : empty;
    printer_prefix_ = config->df();
    defaults_ = wizard_device_defaults(*config, hardware);
    std::string heaters, fans;
    for (const auto& role : hardware_role_registry()) {
        const auto key = printer_prefix_ + role.config_key;
        std::string value;
        if (config->exists(key)) {
            const auto* node = config->try_get_json(key);
            value = node && node->is_string() ? node->get<std::string>() : lv_tr("Unknown");
            if (value.empty() || value == "None")
                value = lv_tr("None");
            else {
                const auto& objects =
                    role.category == HardwareCategory::Fan ? hardware.fans() : hardware.heaters();
                if (std::find(objects.begin(), objects.end(), value) == objects.end())
                    value += std::string(" (") + lv_tr("Not detected") + ")";
            }
        } else {
            const auto it = std::find_if(defaults_.begin(), defaults_.end(),
                                         [&](const auto& d) { return d.key == role.config_key; });
            value = it == defaults_.end() ? lv_tr("Not configured")
                                          : it->value + " (" + lv_tr("Suggested") + ")";
        }
        append_line(role.category == HardwareCategory::Fan ? fans : heaters,
                    std::string(lv_tr(role_label(role.id))) + ": " + value);
    }
    std::string ams;
    auto& state = AmsState::instance();
    for (int i = 0; i < state.backend_count(); ++i) {
        if (auto* backend = state.get_backend(i))
            append_line(ams, ams_type_traits(backend->get_type()).product_name);
    }
    if (ams.empty())
        ams = lv_tr("Not detected");
    const auto leds = list_names(hardware.leds());
    const auto sensor_names =
        hardware.objects_of({hw::ObjectKind::FilamentSwitch, hw::ObjectKind::FilamentMotion});
    const auto sensors = list_names(sensor_names);
    const char* status =
        lv_tr(hardware.objects_reported()
                  ? "Existing assignments are kept. Suggested devices are saved when you continue."
                  : "Devices are unavailable until Moonraker discovery completes.");
    const char* attrs[] = {
        "heaters",    heaters.c_str(), "fans",          fans.c_str(), "ams",  ams.c_str(), "leds",
        leds.c_str(), "sensors",       sensors.c_str(), "status",     status, nullptr};
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(parent, component_name(), attrs));
    created_ = root != nullptr;
    if (root) {
        const auto disable_empty = [root](const char* name, bool empty) {
            if (empty) {
                if (auto* row = lv_obj_find_by_name(root, name))
                    lv_obj_add_state(row, LV_STATE_DISABLED);
            }
        };
        disable_empty("wizard_devices_heaters", hardware.heaters().empty());
        disable_empty("wizard_devices_fans", hardware.fans().empty());
        disable_empty("wizard_devices_ams", state.backend_count() == 0);
        disable_empty("wizard_devices_leds", hardware.leds().empty());
        disable_empty("wizard_devices_sensors", sensor_names.empty());
    }
    return root;
}

void WizardDevicesStep::cleanup() {
    auto* config = Config::get_instance();
    if (created_ && config->df() == printer_prefix_ &&
        apply_wizard_device_defaults(*config, defaults_)) {
        if (!config->save())
            NOTIFY_ERROR(lv_tr("Failed to save hardware configuration"));
    }
    defaults_.clear();
    created_ = false;
}
