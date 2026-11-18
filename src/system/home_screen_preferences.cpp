// SPDX-License-Identifier: GPL-3.0-or-later
#include "home_screen_preferences.h"

#include "config.h"

namespace helix {
namespace {

const char* device_key(HomeScreenPreferences::DeviceKind kind) {
    switch (kind) {
    case HomeScreenPreferences::DeviceKind::Heater:
        return "heaters";
    case HomeScreenPreferences::DeviceKind::PassiveSensor:
        return "passive_sensors";
    case HomeScreenPreferences::DeviceKind::Fan:
        return "fans";
    }
    return nullptr;
}

bool boolean_or_visible(const json& object, const std::string& key) {
    if (!object.is_object()) {
        return true;
    }
    const auto it = object.find(key);
    return it == object.end() || !it->is_boolean() || it->get<bool>();
}

std::string preferences_path(const Config& config) {
    return config.df() + "home_screen/";
}

} // namespace

bool HomeScreenPreferences::is_visible(DeviceKind kind, const std::string& object_id,
                                       bool default_visible) const {
    const char* key = device_key(kind);
    if (!key) {
        return default_visible;
    }
    const json* choices = config_.try_get_json(preferences_path(config_) + key);
    if (!choices || !choices->is_object()) {
        return default_visible;
    }
    const auto it = choices->find(object_id);
    return it != choices->end() && it->is_boolean() ? it->get<bool>() : default_visible;
}

void HomeScreenPreferences::set_visible(DeviceKind kind, const std::string& object_id,
                                        bool visible) {
    set_all_visible(kind, {object_id}, visible);
}

void HomeScreenPreferences::set_all_visible(DeviceKind kind,
                                            const std::vector<std::string>& object_ids,
                                            bool visible) {
    const char* key = device_key(kind);
    if (!key || object_ids.empty()) {
        return;
    }
    const std::string path = preferences_path(config_) + key;
    json choices = config_.get<json>(path, json::object());
    if (!choices.is_object()) {
        choices = json::object();
    }
    for (const auto& id : object_ids) {
        choices[id] = visible;
    }
    config_.set<json>(path, choices);
}

HomeScreenPreferences::SpoolUnits HomeScreenPreferences::spool_units() const {
    const json* units = config_.try_get_json(preferences_path(config_) + "spool_units");
    if (!units) {
        return {};
    }
    return {boolean_or_visible(*units, "percent"), boolean_or_visible(*units, "grams"),
            boolean_or_visible(*units, "metres")};
}

void HomeScreenPreferences::set_spool_units(const SpoolUnits& units) {
    const std::string path = preferences_path(config_) + "spool_units";
    json choices = config_.get<json>(path, json::object());
    if (!choices.is_object()) {
        choices = json::object();
    }
    choices["percent"] = units.percent;
    choices["grams"] = units.grams;
    choices["metres"] = units.metres;
    config_.set<json>(path, choices);
}

bool HomeScreenPreferences::save() {
    return config_.save();
}

} // namespace helix
