// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/mock_config_storage.h"
#include "home_screen_preferences.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using DeviceKind = HomeScreenPreferences::DeviceKind;

namespace {

class HomePreferencesFixture : public HelixTestFixture {
  protected:
    Config config;
    HomeScreenPreferences preferences{config};

    HomePreferencesFixture() {
        ConfigTestAccess::data(config) = {{"config_version", kCurrentConfigVersion},
                                          {"active_printer_id", "printer-a"},
                                          {"printers",
                                           {{"printer-a", {{"moonraker_host", "192.0.2.1"}}},
                                            {"printer-b", {{"moonraker_host", "192.0.2.2"}}}}}};
        ConfigTestAccess::active_printer_id(config) = "printer-a";
    }
};

} // namespace

TEST_CASE_METHOD(HomePreferencesFixture,
                 "Home preferences default to visible without writing config",
                 "[home-preferences]") {
    const auto before = ConfigTestAccess::data(config);
    CHECK(preferences.is_visible(DeviceKind::Heater, "extruder"));
    CHECK(preferences.is_visible(DeviceKind::PassiveSensor, "temperature_sensor mcu"));
    CHECK(preferences.is_visible(DeviceKind::Fan, "heater_fan hotend"));
    const auto units = preferences.spool_units();
    CHECK(units.percent);
    CHECK(units.grams);
    CHECK(units.metres);
    CHECK(ConfigTestAccess::data(config) == before);
}

TEST_CASE_METHOD(HomePreferencesFixture,
                 "Explicit Home choices override legacy tile and sensor defaults",
                 "[home-preferences]") {
    const auto before = ConfigTestAccess::data(config);
    CHECK_FALSE(preferences.is_visible(DeviceKind::Heater, "heater_generic extra", false));
    CHECK(preferences.is_visible(DeviceKind::Heater, "extruder", true));
    CHECK_FALSE(preferences.is_visible(DeviceKind::PassiveSensor, "temperature_sensor driver", false));
    CHECK(ConfigTestAccess::data(config) == before);

    preferences.set_visible(DeviceKind::Heater, "extruder", false);
    preferences.set_visible(DeviceKind::Heater, "heater_generic extra", true);
    preferences.set_visible(DeviceKind::PassiveSensor, "temperature_sensor driver", true);
    CHECK_FALSE(preferences.is_visible(DeviceKind::Heater, "extruder", true));
    CHECK(preferences.is_visible(DeviceKind::Heater, "heater_generic extra", false));
    CHECK(preferences.is_visible(DeviceKind::PassiveSensor, "temperature_sensor driver", false));
    preferences.set_visible(DeviceKind::Heater, "heater_generic extra", false);
    CHECK_FALSE(preferences.is_visible(DeviceKind::Heater, "heater_generic extra", true));
    REQUIRE(config.set_active_printer("printer-b"));
    CHECK(preferences.is_visible(DeviceKind::Heater, "extruder", true));
    CHECK_FALSE(preferences.is_visible(DeviceKind::PassiveSensor, "temperature_sensor driver", false));
}

TEST_CASE_METHOD(HomePreferencesFixture,
                 "Home choices use literal IDs and preserve disconnected devices",
                 "[home-preferences]") {
    const std::string full_id = "temperature_sensor board/revision~2";
    preferences.set_visible(DeviceKind::PassiveSensor, full_id, false);
    preferences.set_visible(DeviceKind::PassiveSensor, "temperature_sensor offline", false);
    preferences.set_visible(DeviceKind::Heater, full_id, true);

    CHECK_FALSE(preferences.is_visible(DeviceKind::PassiveSensor, full_id));
    CHECK(preferences.is_visible(DeviceKind::Heater, full_id));
    auto stored = config.get<json>(config.df() + "home_screen/passive_sensors", json());
    REQUIRE(stored.is_object());
    REQUIRE(stored.contains(full_id));
    CHECK(stored[full_id] == false);
    CHECK(stored.size() == 2);

    // A refresh/edit for connected hardware does not prune a disconnected ID.
    preferences.set_all_visible(DeviceKind::PassiveSensor, {full_id}, true);
    CHECK_FALSE(preferences.is_visible(DeviceKind::PassiveSensor, "temperature_sensor offline"));
    CHECK(preferences.is_visible(DeviceKind::PassiveSensor, "temperature_sensor new_board"));
}

TEST_CASE_METHOD(HomePreferencesFixture,
                 "Home hide-all is explicit and independent of device settings",
                 "[home-preferences]") {
    config.set<json>(config.df() + "sensors", {{"enabled", true}});
    config.set<int>(config.df() + "extruder_target", 215);
    config.set<int>(config.df() + "fan_speed", 60);
    const auto before = ConfigTestAccess::data(config);
    const std::vector<std::string> heaters{"extruder", "heater_bed", "heater_generic chamber"};
    preferences.set_all_visible(DeviceKind::Heater, heaters, false);
    for (const auto& id : heaters) {
        CHECK_FALSE(preferences.is_visible(DeviceKind::Heater, id));
    }
    CHECK(preferences.is_visible(DeviceKind::Heater, "heater_generic dryer"));

    auto after = ConfigTestAccess::data(config);
    after["printers"]["printer-a"].erase("home_screen");
    CHECK(after == before);
    const auto hidden = config.get<json>(config.df() + "home_screen/heaters", json());
    preferences.set_all_visible(DeviceKind::Heater, {}, true);
    CHECK(config.get<json>(config.df() + "home_screen/heaters", json()) == hidden);
}

TEST_CASE_METHOD(HomePreferencesFixture,
                 "Home preferences follow the active printer without reloading",
                 "[home-preferences][multi-printer]") {
    preferences.set_visible(DeviceKind::Fan, "fan_generic auxiliary", false);
    preferences.set_spool_units({false, true, false});
    REQUIRE(config.set_active_printer("printer-b"));
    CHECK(preferences.is_visible(DeviceKind::Fan, "fan_generic auxiliary"));
    CHECK(preferences.spool_units().percent);
    CHECK(preferences.spool_units().metres);
    preferences.set_visible(DeviceKind::Heater, "heater_bed", false);
    preferences.set_spool_units({true, false, true});

    REQUIRE(config.set_active_printer("printer-a"));
    CHECK_FALSE(preferences.is_visible(DeviceKind::Fan, "fan_generic auxiliary"));
    CHECK(preferences.is_visible(DeviceKind::Heater, "heater_bed"));
    CHECK_FALSE(preferences.spool_units().percent);
    CHECK(preferences.spool_units().grams);
    CHECK_FALSE(preferences.spool_units().metres);
}

TEST_CASE_METHOD(HomePreferencesFixture, "Home spool unit checkboxes are independent",
                 "[home-preferences]") {
    for (int mask = 0; mask < 8; ++mask) {
        CAPTURE(mask);
        const HomeScreenPreferences::SpoolUnits chosen{(mask & 1) != 0, (mask & 2) != 0,
                                                       (mask & 4) != 0};
        preferences.set_spool_units(chosen);
        const auto actual = preferences.spool_units();
        CHECK(actual.percent == chosen.percent);
        CHECK(actual.grams == chosen.grams);
        CHECK(actual.metres == chosen.metres);
    }
}

TEST_CASE_METHOD(HomePreferencesFixture,
                 "Malformed Home preferences fall back without discarding other choices",
                 "[home-preferences]") {
    config.set<json>(config.df() + "home_screen",
                     {{"heaters", {{"extruder", "false"}, {"heater_bed", false}}},
                      {"fans", json::array({false})},
                      {"spool_units", {{"percent", 0}, {"grams", false}, {"future_unit", true}}},
                      {"future_option", 42}});
    CHECK(preferences.is_visible(DeviceKind::Heater, "extruder"));
    CHECK_FALSE(preferences.is_visible(DeviceKind::Heater, "heater_bed"));
    CHECK(preferences.is_visible(DeviceKind::Fan, "fan"));
    CHECK(preferences.spool_units().percent);
    CHECK_FALSE(preferences.spool_units().grams);
    CHECK(preferences.spool_units().metres);

    preferences.set_visible(DeviceKind::Fan, "fan", false);
    preferences.set_spool_units({false, false, true});
    CHECK(config.get<bool>(config.df() + "home_screen/spool_units/future_unit", false));
    CHECK(config.get<int>(config.df() + "home_screen/future_option", 0) == 42);
    CHECK_FALSE(preferences.is_visible(DeviceKind::Heater, "heater_bed"));
}

TEST_CASE_METHOD(HomePreferencesFixture,
                 "Home changes can be batched and save failures are reported",
                 "[home-preferences][config]") {
    auto storage = std::make_unique<helix::test::MockConfigStorage>();
    auto* backend = storage.get();
    backend->small = true; // No file backup side effects in this in-memory test.
    config.set_storage(std::move(storage));
    ConfigTestAccess::path(config) = "memory-home-preferences.json";

    preferences.set_visible(DeviceKind::Fan, "fan", false);
    preferences.set_spool_units({true, false, false});
    CHECK(backend->store_calls == 0);
    REQUIRE(preferences.save());
    REQUIRE(backend->doc.has_value());
    const auto saved = json::parse(*backend->doc);
    CHECK(saved["printers"]["printer-a"]["home_screen"]["fans"]["fan"] == false);
    CHECK(backend->store_calls == 1);

    backend->ro = true;
    CHECK_FALSE(preferences.save());
}
