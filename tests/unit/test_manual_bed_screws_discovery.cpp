// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "../lvgl_test_fixture.h"
#include "capability_overrides.h"
#include "moonraker_discovery_sequence.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "z_offset_calibration_approach.h"
#include "hv/json.hpp"

using json = nlohmann::json;
using helix::Capability;
using helix::MoonrakerDiscoverySequence;

TEST_CASE("Manual bed screws are distinct from probe-assisted screw tilt",
          "[bed_screws][calibration-discovery]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json{"bed_screws"});
    CHECK(hw.has_bed_screws());
    CHECK_FALSE(hw.has_screws_tilt());
    CHECK_FALSE(hw.has_bed_mesh());

    hw.parse_objects(json{"screws_tilt_adjust", "probe"});
    CHECK_FALSE(hw.has_bed_screws());
    CHECK(hw.has_screws_tilt());
    CHECK_FALSE(hw.has_bed_mesh());

    hw.parse_objects(json{"gcode_macro BED_SCREWS_ADJUST", "gcode_macro BED_MESH_CALIBRATE"});
    CHECK_FALSE(hw.has_bed_screws());
    CHECK_FALSE(hw.has_bed_mesh());
}

TEST_CASE("Manual bed screws config discovery removes a stale config-only capability",
          "[bed_screws][calibration-discovery]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array());
    hw.parse_config_keys(json{{"bed_screws", { {"screw1", "10,10"} }}});
    CHECK(hw.has_bed_screws());
    hw.parse_config_keys(json::object());
    CHECK_FALSE(hw.has_bed_screws());
    hw.parse_objects(json{"bed_screws"});
    hw.parse_config_keys(json::object());
    CHECK(hw.has_bed_screws());
    hw.parse_objects(json::array());
    CHECK_FALSE(hw.has_bed_screws());
}

TEST_CASE("Manual adjustment status subscription requires an actual queryable object",
          "[bed_screws][calibration-discovery]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array());
    hw.parse_config_keys(json{{"bed_screws", json::object()}});
    auto subs = MoonrakerDiscoverySequence::build_subscription_objects(hw);
    CHECK_FALSE(subs.contains("bed_screws"));
    hw.parse_objects(json{"bed_screws"});
    subs = MoonrakerDiscoverySequence::build_subscription_objects(hw);
    CHECK(subs.at("bed_screws") == json{"is_active", "state", "current_screw", "accepted_screws"});
    hw.parse_objects(json::array());
    CHECK_FALSE(MoonrakerDiscoverySequence::build_subscription_objects(hw).contains("bed_screws"));
}

TEST_CASE("Calibration capability subjects never invent a bed mesh from a macro override",
          "[bed_screws][calibration-discovery][printer_capabilities]") {
    LVGLTestFixture fixture;
    helix::PrinterCapabilitiesState caps;
    caps.init_subjects(false);
    helix::PrinterDiscovery hw;
    hw.parse_objects(json{"bed_screws"});
    CapabilityOverrides overrides;
    overrides.set_hardware(hw);
    overrides.set_override(capability::BED_MESH, OverrideState::ENABLE);
    caps.set_hardware(hw, overrides);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasBedScrews)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasScrewsTilt)) == 0);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasBedMesh)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasBedMeshCalibration)) == 0);

    hw.parse_objects(json{"bed_mesh", "screws_tilt_adjust"});
    overrides.set_hardware(hw);
    caps.set_hardware(hw, overrides);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasBedScrews)) == 0);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasBedMeshCalibration)) == 1);
    overrides.set_override(capability::BED_MESH, OverrideState::DISABLE);
    caps.set_hardware(hw, overrides);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasBedMeshCalibration)) == 0);
    hw.parse_objects(json::array());
    overrides.set_hardware(hw);
    caps.set_hardware(hw, overrides);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasScrewsTilt)) == 0);
    caps.deinit_subjects();
}

TEST_CASE("Z calibration requires a probe or a configured physical Z endstop",
          "[calibration-discovery]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json::array());
    CHECK_FALSE(hw.has_z_offset_calibration());
    for (const auto& pin : json::array({"probe:z_virtual_endstop", "tmc2209_stepper_z:virtual_endstop",
                                       "", " ^! ", nullptr, 1})) {
        hw.parse_config_keys(json{{"stepper_z", {{"endstop_pin", pin}}}});
        CHECK_FALSE(hw.has_physical_z_endstop());
        CHECK_FALSE(hw.has_z_offset_calibration());
    }
    hw.parse_config_keys(json{{"stepper_z", {{"endstop_pin", "^!mcu:PA0"}}}});
    CHECK(hw.has_physical_z_endstop());
    CHECK(hw.has_z_offset_calibration());
    hw.parse_config_keys(json::object());
    CHECK_FALSE(hw.has_z_offset_calibration());
    hw.parse_objects(json{"probe"});
    CHECK(hw.has_z_offset_calibration());
    CHECK_FALSE(hw.has_physical_z_endstop());
}

TEST_CASE("Calibration device caps clear on missing hardware despite user macro overrides",
          "[calibration-discovery][printer_capabilities]") {
    LVGLTestFixture fixture;
    helix::PrinterCapabilitiesState caps;
    caps.init_subjects(false);
    helix::PrinterDiscovery hw;
    hw.parse_objects(json{"heater_bed", "extruder", "quad_gantry_level", "z_tilt", "probe"});
    CapabilityOverrides overrides;
    overrides.set_override(capability::QGL, OverrideState::ENABLE);
    overrides.set_override(capability::Z_TILT, OverrideState::ENABLE);
    overrides.set_hardware(hw);
    caps.set_hardware(hw, overrides);
    for (auto capability : {Capability::HasHeaters, Capability::HasExtruder, Capability::HasQglCalibration,
                            Capability::HasZTiltCalibration, Capability::HasZOffsetCalibration}) {
        CHECK(lv_subject_get_int(caps.subject(capability)) == 1);
    }
    hw.parse_objects(json{"gcode_macro QUAD_GANTRY_LEVEL", "temperature_sensor heater_bed"});
    overrides.set_hardware(hw);
    caps.set_hardware(hw, overrides);
    for (auto capability : {Capability::HasHeaters, Capability::HasExtruder, Capability::HasQglCalibration,
                            Capability::HasZTiltCalibration, Capability::HasZOffsetCalibration}) {
        CHECK(lv_subject_get_int(caps.subject(capability)) == 0);
    }
    caps.deinit_subjects();
}

TEST_CASE("Z calibration strategy follows the available physical mechanism",
          "[calibration-discovery][zoffset_center]") {
    using S = helix::ZOffsetCalibrationStrategy;
    using helix::zoffset_cal::effective_strategy;
    CHECK(effective_strategy(S::PROBE_CALIBRATE, false, true) == S::ENDSTOP);
    CHECK(effective_strategy(S::ENDSTOP, true, false) == S::PROBE_CALIBRATE);
    CHECK(effective_strategy(S::ENDSTOP, true, true) == S::ENDSTOP);
    CHECK(effective_strategy(S::FIRMWARE_MANAGED, true, true) == S::FIRMWARE_MANAGED);
}
