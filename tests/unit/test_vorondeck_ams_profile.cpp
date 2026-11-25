// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using nlohmann::json;

TEST_CASE("Retired AMS objects do not claim a supported system", "[vorondeck][ams-profile]") {
    struct Example {
        const char* object;
        const char* alias;
        AmsType type;
        bool enabled;
    };
    const Example examples[] = {
        {"ace", "ace", AmsType::ACE, false},
        {"filament_hub", "valgace", AmsType::ACE, false},
        {"ace_instance_2", "bunnyace", AmsType::ACE, false},
        {"ifs", "ifs", AmsType::AD5X_IFS, false},
        {"ifs_materials", "ad5x", AmsType::AD5X_IFS, false},
        {"filament_switch_sensor _ifs_port_sensor_1", "ad5x_ifs", AmsType::AD5X_IFS, false},
        {"filament_motion_sensor ifs_motion_sensor", "ifs", AmsType::AD5X_IFS, false},
        {"box", "cfs", AmsType::CFS, false},
        {"box_stepper slot1", "qidi_box", AmsType::QIDI_BOX, false},
        {"filament_detect", "snapmaker", AmsType::SNAPMAKER, false},
    };
    for (const auto& example : examples) {
        DYNAMIC_SECTION(example.object) {
            PrinterDiscovery hw;
            hw.parse_objects(json::array({example.object, "extruder", "heater_bed"}));
            const auto expected = example.enabled ? example.type : AmsType::NONE;
            CHECK(hw.mmu_type() == expected);
            CHECK(ams_type_from_string(example.alias) == expected);
            CHECK(hw.detected_ams_systems().size() == (example.enabled ? 1 : 0));
            CHECK(hw.has_heater_bed());
        }
    }
}

TEST_CASE("Open Klipper filament systems remain discoverable", "[vorondeck][ams-profile]") {
    const std::pair<const char*, AmsType> examples[] = {
        {"mmu", AmsType::HAPPY_HARE},
        {"AFC", AmsType::AFC},
        {"toolchanger", AmsType::TOOL_CHANGER},
        {"indx", AmsType::INDX},
    };
    for (const auto& [object, expected] : examples) {
        DYNAMIC_SECTION(object) {
            PrinterDiscovery hw;
            hw.parse_objects(json::array({object, "tool T0", "extruder", "heater_bed"}));
            CHECK(hw.mmu_type() == expected);
            REQUIRE(hw.detected_ams_systems().size() == 1);
            CHECK(hw.detected_ams_systems().front().type == expected);
        }
    }
}

TEST_CASE("Generic screw calibration remains available", "[vorondeck][ams-profile]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"auto_screws_tilt_adjust"}));
    CHECK(hw.has_screws_tilt() == bool(false));
    hw.parse_objects(json::array());
    hw.parse_config_keys(json{{"auto_screws_tilt_adjust", json::object()}});
    CHECK(hw.has_screws_tilt() == bool(false));
    hw.parse_config_keys(json{{"screws_tilt_adjust", json::object()}});
    CHECK(hw.has_screws_tilt());
}

TEST_CASE("OpenAMS keeps its versioned API discovery", "[vorondeck][ams-profile]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"oams_manager", "extruder"}));
    CHECK(hw.mmu_type() == AmsType::NONE);
    hw.settle_status_claims(
        json{{"oams_manager", {{"api_version", 1}, {"schema", "openams.manager"}}}});
    CHECK(hw.mmu_type() == AmsType::OPENAMS);
    REQUIRE(hw.detected_ams_systems().size() == 1);
    CHECK(hw.detected_ams_systems().front().type == AmsType::OPENAMS);
}

TEST_CASE("Disabled vendor objects cannot override an open AMS", "[vorondeck][ams-profile]") {
    for (const auto* open_system : {"AFC", "mmu"}) {
        DYNAMIC_SECTION(open_system) {
            PrinterDiscovery hw;
            hw.parse_objects(json::array(
                {"ifs", "box_stepper slot1", open_system, "box", "filament_detect", "extruder"}));
            REQUIRE(hw.detected_ams_systems().size() == 1);
            CHECK(hw.mmu_type() == ams_type_from_string(open_system));
        }
    }
}
