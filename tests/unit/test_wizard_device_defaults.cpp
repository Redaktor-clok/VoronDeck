// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "printer_discovery.h"
#include "ui_wizard_devices.h"
#include "wizard_config_paths.h"
#include "../catch_amalgamated.hpp"
#include <set>

using namespace helix;
class WizardDeviceDefaultsFixture : public HelixTestFixture {
  protected:
    Config config;
    PrinterDiscovery hardware;
    WizardDeviceDefaultsFixture() {
        setup_printer_data(config, nlohmann::json::object());
        ConfigTestAccess::path(config) = "";
        hardware.parse_objects(nlohmann::json::array({"extruder", "heater_bed", "fan",
            "heater_fan hotend_fan", "fan_generic chamber_exhaust", "neopixel case_light",
            "filament_switch_sensor runout"}));
    }
};

TEST_CASE_METHOD(WizardDeviceDefaultsFixture, "Device overview proposes without writing and pairs heaters",
                 "[wizard][wizard_devices]") {
    const auto before = ConfigTestAccess::data(config);
    const auto plan = wizard_device_defaults(config, hardware);
    REQUIRE_FALSE(plan.empty());
    CHECK(ConfigTestAccess::data(config) == before);
    REQUIRE(apply_wizard_device_defaults(config, plan));
    CHECK(config.get<std::string>(config.df() + wizard::HOTEND_HEATER) == "extruder");
    CHECK(config.get<std::string>(config.df() + wizard::HOTEND_SENSOR) == "extruder");
    CHECK(config.get<std::string>(config.df() + wizard::BED_HEATER) == "heater_bed");
    CHECK(config.get<std::string>(config.df() + wizard::BED_SENSOR) == "heater_bed");
    std::set<std::string> fans;
    for (const auto& item : plan) {
        if (item.key.rfind("fans/", 0) == 0) CHECK(fans.insert(item.value).second);
        CHECK(item.key.rfind("leds/", 0) != 0);
        CHECK(item.key.rfind("ams/", 0) != 0);
        CHECK(item.key.rfind("filament_sensors/", 0) != 0);
    }
    CHECK_FALSE(apply_wizard_device_defaults(config, plan));
}

TEST_CASE_METHOD(WizardDeviceDefaultsFixture, "Device overview preserves empty stale and malformed selections",
                 "[wizard][wizard_devices]") {
    config.set<std::string>(config.df() + wizard::PART_FAN, "");
    config.set<std::string>(config.df() + wizard::HOTEND_FAN, "fan_generic removed");
    config.set<int>(config.df() + wizard::CHAMBER_FAN, 7);
    const auto plan = wizard_device_defaults(config, hardware);
    apply_wizard_device_defaults(config, plan);
    CHECK(config.get<std::string>(config.df() + wizard::PART_FAN).empty());
    CHECK(config.get<std::string>(config.df() + wizard::HOTEND_FAN) == "fan_generic removed");
    CHECK(config.get<int>(config.df() + wizard::CHAMBER_FAN) == 7);
}

TEST_CASE_METHOD(WizardDeviceDefaultsFixture, "Device overview never invents hardware before discovery",
                 "[wizard][wizard_devices]") {
    PrinterDiscovery undiscovered;
    CHECK(wizard_device_defaults(config, undiscovered).empty());
    undiscovered.parse_objects(nlohmann::json::array());
    CHECK(wizard_device_defaults(config, undiscovered).empty());
}

TEST_CASE_METHOD(WizardDeviceDefaultsFixture, "Device overview respects explicit sensor choices",
                 "[wizard][wizard_devices]") {
    config.set<std::string>(config.df() + wizard::BED_SENSOR, "");
    config.set<std::string>(config.df() + wizard::HOTEND_HEATER, "extruder");
    apply_wizard_device_defaults(config, wizard_device_defaults(config, hardware));
    CHECK_FALSE(config.exists(config.df() + wizard::BED_HEATER));
    CHECK(config.get<std::string>(config.df() + wizard::BED_SENSOR).empty());
    CHECK(config.get<std::string>(config.df() + wizard::HOTEND_SENSOR) == "extruder");
}

TEST_CASE_METHOD(WizardDeviceDefaultsFixture, "Device overview rechecks keys changed after its preview",
                 "[wizard][wizard_devices]") {
    const auto plan = wizard_device_defaults(config, hardware);
    config.set<std::string>(config.df() + wizard::HOTEND_HEATER, "extruder1");
    config.set<std::string>(config.df() + wizard::PART_FAN, "");
    apply_wizard_device_defaults(config, plan);
    CHECK(config.get<std::string>(config.df() + wizard::HOTEND_HEATER) == "extruder1");
    CHECK_FALSE(config.exists(config.df() + wizard::HOTEND_SENSOR));
    CHECK(config.get<std::string>(config.df() + wizard::PART_FAN).empty());
}
