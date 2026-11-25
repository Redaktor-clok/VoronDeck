// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_test_access.h"
#include "../test_helpers/printer_capture.h"
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "data_root_resolver.h"
#include "klipper_extruder_naming.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::test;
// ============================================================================
// Machine captures (prestonbrown/helixscreen#1603)
// ============================================================================
//
// Real-machine captures live in tests/fixtures/printers/<model>.json, so adding
// a printer is a capture and a file. They are OBSERVATIONS of what a machine
// reports over Moonraker, not copies of the database: the two are not supposed
// to agree, and the point of testing against them is to measure the gap.
// Transcribe them faithfully; deriving them from database entries would make
// the test circular.

namespace {

// Heuristic-class scores stay under this test-chosen bar, below the
// production autosave bar kAutosaveMinConfidence = 85 (#1284).
constexpr int kHeuristicClassCeiling = 70;

/// The hardware -> expected model rows in tests/fixtures/printers/detection_cases.json,
/// read once: each DYNAMIC_SECTION re-enters its test case, and a re-read would
/// repeat the load's assertions once per row.
const nlohmann::json& load_detection_corpus() {
    static const nlohmann::json corpus = load_printer_capture("detection_cases");
    return corpus;
}

} // namespace

// ============================================================================
// Test Fixtures and Helpers
// ============================================================================

/**
 * @brief Test fixture providing common hardware configurations
 */
class PrinterDetectorFixture {
  protected:
    // A real Qidi Q2 as its owner's machine reports itself (debug bundle
    // NPN3LYBJ). The capture file records why its hostname and build volume
    // look wrong for a spec sheet and must not be "fixed".
    PrinterHardwareData qidi_q2_hardware() {
        return printer_capture("qidi_q2");
    }

    // Elegoo Centauri Carbon running OpenCentauri COSMOS, as the bench machine
    // reports itself. Its hostname names neither vendor nor model; the capture
    // file records what actually identifies the machine.
    PrinterHardwareData elegoo_centauri_carbon_hardware() {
        return printer_capture("elegoo_centauri_carbon");
    }
};

// ============================================================================
// Detection corpus: hardware -> expected model
// ============================================================================
//
// One row of tests/fixtures/printers/detection_cases.json per printer shape:
// the hardware a machine reports and what detection must make of it. A new
// printer is a row there, not a test case. Each key a row carries is one
// assertion on the result:
//
//   expect          identified outright as this model (detected, margin >= kDetectMinMargin)
//   detected, none  detected() is true / false
//   type_name       the winner's name, exactly (no detection implied)
//   confidence, confidence_below, margin_above   numeric bounds
//   family, runner_family   winner / runner-up is SOME model with this name prefix
//   runner_up       the runner-up, exactly
//   tie, tied       margin() == 0 / tied_count > 1: nothing separates the leaders
//   autosave        meets_autosave_threshold() is this value
//   not, contains, not_contains, runner_not_contains, contains_any   name checks
//   reason_any      the reason text names at least one of these
//   preset          the result's preset field
//
// Scoring rules whose assertions compare two detections or reach beyond the
// result stay as cases below.

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: corpus rows detect their expected model",
                 "[printer][detector][corpus][1284][ad5m][ad5x][anycubic][artillery][board_match]["
                 "build_volume][case_sensitivity][combined][cpu_arch_match][creality][elegoo]["
                 "flsun][heuristics][hostname_exclude][hostname_match][kinematics][led_match]["
                 "macros][mcu][negative][non_printer][objects][prusa][qidi][ratos][ratrig][real_"
                 "world][regression][sensor_match][sovol][steppers][tool_count]") {
    const nlohmann::json& corpus = load_detection_corpus();
    auto strings = [](const nlohmann::json& row, const char* key) {
        std::vector<std::string> out;
        if (row.contains(key)) {
            for (const auto& item : row.at(key)) {
                out.push_back(item.get<std::string>());
            }
        }
        return out;
    };
    auto starts_with = [](const std::string& s, const std::string& prefix) {
        return s.rfind(prefix, 0) == 0;
    };
    auto has = [](const std::string& s, const std::string& part) {
        return s.find(part) != std::string::npos;
    };
    for (const auto& row : corpus) {
        const std::string name = row.at("name").get<std::string>();
        DYNAMIC_SECTION(name) {
            const PrinterHardwareData hardware = row.contains("capture")
                                                     ? printer_capture(row.at("capture"))
                                                     : hardware_from_json(row.at("hardware"), name);
            auto result = PrinterDetector::detect(hardware);
            CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
                    result.runner_up_confidence, result.margin(), result.tied_count, result.reason);

            // "expect": identified outright as this model.
            if (row.contains("expect")) {
                REQUIRE(result.detected());
                REQUIRE(result.type_name == row.at("expect").get<std::string>());
                REQUIRE(result.margin() >= PrinterDetector::kDetectMinMargin);
            }
            if (row.value("detected", false)) {
                REQUIRE(result.detected());
            }
            if (row.value("none", false)) {
                REQUIRE_FALSE(result.detected());
            }
            if (row.contains("type_name")) {
                CHECK(result.type_name == row.at("type_name").get<std::string>());
            }
            if (row.contains("confidence")) {
                CHECK(result.confidence == row.at("confidence").get<int>());
            }
            if (row.contains("confidence_below")) {
                CHECK(result.confidence < row.at("confidence_below").get<int>());
            }
            // "family" / "runner_family": the winner or runner-up is SOME model
            // of that family; "tie": nothing separates it from the runner-up.
            if (row.contains("family")) {
                CHECK(starts_with(result.type_name, row.at("family").get<std::string>()));
            }
            if (row.contains("runner_family")) {
                CHECK(starts_with(result.runner_up_type_name,
                                  row.at("runner_family").get<std::string>()));
            }
            if (row.contains("runner_up")) {
                CHECK(result.runner_up_type_name == row.at("runner_up").get<std::string>());
            }
            if (row.value("tie", false)) {
                CHECK(result.margin() == 0);
            }
            if (row.value("tied", false)) {
                CHECK(result.tied_count > 1);
            }
            if (row.contains("margin_above")) {
                CHECK(result.margin() > row.at("margin_above").get<int>());
            }
            if (row.contains("autosave")) {
                CHECK(PrinterDetector::meets_autosave_threshold(result) ==
                      row.at("autosave").get<bool>());
            }
            for (const auto& other : strings(row, "not")) {
                CHECK(result.type_name != other);
            }
            for (const auto& part : strings(row, "contains")) {
                CHECK(has(result.type_name, part));
            }
            for (const auto& part : strings(row, "not_contains")) {
                CHECK_FALSE(has(result.type_name, part));
            }
            for (const auto& part : strings(row, "runner_not_contains")) {
                CHECK_FALSE(has(result.runner_up_type_name, part));
            }
            if (row.contains("contains_any")) {
                const auto parts = strings(row, "contains_any");
                CHECK(std::any_of(parts.begin(), parts.end(),
                                  [&](const std::string& p) { return has(result.type_name, p); }));
            }
            if (row.contains("reason_any")) {
                const auto parts = strings(row, "reason_any");
                CHECK(std::any_of(parts.begin(), parts.end(),
                                  [&](const std::string& p) { return has(result.reason, p); }));
            }
            if (row.contains("preset")) {
                CHECK(result.preset == row.at("preset").get<std::string>());
            }
        }
    }
}

TEST_CASE("PrinterDetector: corpus rows are unique and use only known keys",
          "[printer][detector][corpus]") {
    const nlohmann::json& corpus = load_detection_corpus();
    // A misspelled key would be an assertion the corpus case never makes.
    static const std::set<std::string> kRowKeys = {"name",
                                                   "note",
                                                   "capture",
                                                   "hardware",
                                                   "expect",
                                                   "detected",
                                                   "none",
                                                   "type_name",
                                                   "confidence",
                                                   "confidence_below",
                                                   "margin_above",
                                                   "family",
                                                   "runner_family",
                                                   "runner_up",
                                                   "tie",
                                                   "tied",
                                                   "autosave",
                                                   "not",
                                                   "contains",
                                                   "not_contains",
                                                   "runner_not_contains",
                                                   "contains_any",
                                                   "reason_any",
                                                   "preset"};
    std::set<std::string> names;
    for (const auto& row : corpus) {
        names.insert(row.at("name").get<std::string>());
        for (const auto& item : row.items()) {
            INFO("row '" << row.at("name").get<std::string>() << "' key " << item.key());
            CHECK(kRowKeys.count(item.key()) == 1);
        }
    }
    REQUIRE(!corpus.empty());
    REQUIRE(names.size() == corpus.size());
}

// ============================================================================
// Heuristic Type Tests
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: fan_combo missing one pattern fails",
                 "[printer][heuristics]") {
    PrinterHardwareData hardware{
        .heaters = {"extruder"},
        .sensors = {},
        .fans = {"bed_fans"}, // Has bed_fans but missing chamber/exhaust
        .leds = {},
        .hostname = "generic-test", // No hostname match
        .printer_objects = {},
        .steppers = {},

        .kinematics = "corexy" // Add kinematics to boost confidence
    };

    auto result = PrinterDetector::detect(hardware);
    CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
            result.runner_up_confidence, result.margin(), result.tied_count);

    REQUIRE(result.detected());
    // A partial combo scores as a single fan_match: family evidence only.
    REQUIRE(result.type_name.rfind("Voron", 0) == 0);
    REQUIRE(result.margin() == 0);

    // The full combo is what separates the Voron 2.4 from its look-alikes,
    // so the partial rig must score strictly below it.
    PrinterHardwareData with_full_combo = hardware;
    with_full_combo.fans = {"bed_fans", "chamber_fan", "exhaust_fan"};
    const auto full = PrinterDetector::detect(with_full_combo);
    REQUIRE(full.type_name == "Voron 2.4");
    REQUIRE(result.confidence < full.confidence);
}

// ============================================================================
// Database Loading Tests
// ============================================================================

TEST_CASE("PrinterDetector: Database loads successfully", "[printer][database]") {
    // First detection loads database
    PrinterHardwareData hardware;
    auto result = PrinterDetector::detect(hardware);

    // Should not crash or return error reason about database
    REQUIRE(result.reason.find("Failed to load") == std::string::npos);
    REQUIRE(result.reason.find("Invalid") == std::string::npos);
}

TEST_CASE("PrinterDetector: Subsequent calls use cached database", "[printer][database]") {
    PrinterHardwareData hw;
    hw.hostname = "voron-2.4";
    const auto first = PrinterDetector::detect(hw);
    const auto second = PrinterDetector::detect(hw);
    REQUIRE(first.type_name == "Voron 2.4");
    REQUIRE(second.type_name == first.type_name);
    REQUIRE(second.confidence == first.confidence);
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: detection works after database compaction (second printer add)",
                 "[printer][database][regression]") {
    PrinterHardwareData hw;
    hw.hostname = "voron-2.4";
    const auto first = PrinterDetector::detect(hw);
    REQUIRE(first.type_name == "Voron 2.4");
    PrinterDetector::compact_database();
    const auto second = PrinterDetector::detect(hw);
    CHECK(second.type_name == first.type_name);
    CHECK(second.confidence == first.confidence);
    PrinterDetector::reload();
}

// ============================================================================
// Helper Method Tests
// ============================================================================

TEST_CASE("PrinterDetector: detected() helper returns true for valid match", "[printer][helpers]") {
    PrinterDetectionResult result{
        .type_name = "Test Printer", .confidence = 50, .reason = "Test reason"};

    REQUIRE(result.detected());
}

TEST_CASE("PrinterDetector: detected() helper returns false for no match", "[printer][helpers]") {
    PrinterDetectionResult result{.type_name = "", .confidence = 0, .reason = "No match"};

    REQUIRE_FALSE(result.detected());
}

// ============================================================================
// Enhanced Detection Tests - Build Volume
// ============================================================================

// A corexy printer with the Qidi Max 4 footprint detects as Qidi Max 4, but the
// same footprint on a cartesian machine must NOT — kinematics_exclude encodes the
// hard rule that every Qidi is corexy (telemetry-confirmed). Guards against build
// volume alone shadowing another brand (regression: Qidi Max 4 vs Ender 5 Max).

// The physical Max 4 bed is 390x390 (confirmed on-device, #1068). The bed_mesh a
// printer reports covers only the probed area, a few mm inside the physical bed,
// so the Y span commonly lands just under 390. An earlier build_volume window of
// min_y=390 rejected exactly that case; the window now mirrors X at [370, 410].

// ============================================================================
// Enhanced Detection Tests - Combined Heuristics
// ============================================================================

// A delta rig's class evidence - delta kinematics, delta_calibrate, the
// stepper_a/b/c naming - is carried by every delta vendor in the database, so
// it pins the family and says nothing about the vendor. A hostname pattern one
// entry carries and its look-alikes lack is corroborating evidence here and
// earns nothing toward separation between candidates both published at 100.
// The vendor decision needs hardware: the GD32F303 MCU, opted in as a
// separator (prestonbrown/helixscreen#1607).
TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: bare delta vendors stay ambiguous without MCU corroboration",
                 "[printer][detector][1607]") {
    auto delta_rig = [](const char* hostname, const char* mcu = nullptr) {
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .hostname = hostname,
                                     .printer_objects = {"delta_calibrate"},
                                     .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                     .kinematics = "delta"};
        if (mcu) {
            hardware.mcu = mcu;
            hardware.mcu_list = {mcu};
        }
        return hardware;
    };

    const std::vector<std::string> delta_family = PrinterDetector::get_list_names("delta");
    const auto in_delta_family = [&delta_family](const std::string& name) {
        return std::find(delta_family.begin(), delta_family.end(), name) != delta_family.end();
    };

    SECTION("a bare delta reports the family, not a vendor") {
        auto result = PrinterDetector::detect(delta_rig("test"));
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
                result.runner_up_confidence, result.margin(), result.tied_count);
        REQUIRE(result.detected());
        REQUIRE(in_delta_family(result.type_name));
        REQUIRE(in_delta_family(result.runner_up_type_name));
        REQUIRE(result.runner_up_type_name != result.type_name);
        REQUIRE(result.margin() == 0);
        REQUIRE(result.tied_count > 1);
        REQUIRE_FALSE(PrinterDetector::meets_autosave_threshold(result));
    }

    SECTION("an flsun hostname without the MCU stays ambiguous") {
        for (const char* host : {"flsun", "flsun-v400"}) {
            auto result = PrinterDetector::detect(delta_rig(host));
            INFO("hostname '" << host << "' -> " << result.type_name << " @ " << result.confidence
                              << ", runner-up " << result.runner_up_type_name << " @ "
                              << result.runner_up_confidence << ", margin " << result.margin()
                              << ", " << result.tied_count << " tied");
            REQUIRE(result.detected());
            REQUIRE(in_delta_family(result.type_name));
            // A hostname string the user can set to anything is not hardware:
            // without the MCU the vendor stays a guess nothing may persist.
            REQUIRE(result.margin() == 0);
            REQUIRE(result.tied_count > 1);
            REQUIRE(result.ambiguous());
            REQUIRE_FALSE(PrinterDetector::meets_autosave_threshold(result));
        }
    }

    SECTION("the MCU corroborated capture keeps the FLSUN V400 vendor verdict") {
        auto result = PrinterDetector::detect(delta_rig("flsun-v400", "GD32F303"));
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
                result.runner_up_confidence, result.margin(), result.tied_count);
        REQUIRE(result.detected());
        REQUIRE(result.type_name == "FLSUN V400");
        REQUIRE(result.margin() >= PrinterDetector::kDetectMinMargin);
        REQUIRE(PrinterDetector::meets_autosave_threshold(result));
    }
}

// The FLSUN S1 and S1 Pro run one FLSUN OS image and one Klipper config, so no
// object tells them apart; one entry names both. What separates them from the
// other deltas is hardware the S1 platform carries: the closed-loop motor boards
// report through gcode_button motor_a/b/c, and the lid holds a filament drying
// box. The hostname patterns all start with "flsun": a bare "s1" also names the
// Ender-3 S1 and the Kobra S1.
TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: FLSUN S1 / S1 Pro",
                 "[printer][detector][flsun]") {
    auto s1_rig = [](const char* hostname) {
        PrinterHardwareData hardware{
            .heaters = {"extruder", "heater_bed", "heater_generic HotBed1"},
            .sensors = {"temperature_sensor mcu_temp", "temperature_sensor Armv7 mcu"},
            .fans = {"fan", "heater_fan heat_sink_fan", "heater_fan motherboard_fan",
                     "fan_generic box_fan", "fan_generic drying_box_fan"},
            .hostname = hostname,
            .printer_objects = {"delta_calibrate", "bed_mesh", "probe", "gcode_button motor_a",
                                "gcode_button motor_b", "gcode_button motor_c",
                                "output_pin motor_cali_a", "output_pin drying_box_heater",
                                "fan_generic drying_box_fan", "heater_generic HotBed1",
                                "filament_motion_sensor my_sensor"},
            .steppers = {"stepper_a", "stepper_b", "stepper_c"},
            .kinematics = "delta",
            .mcu = "stm32f103xe",
            .mcu_list = {"stm32f103xe"}};
        hardware.objects_reported = true;
        return hardware;
    };

    const auto expect_s1 = [](const PrinterHardwareData& hardware) {
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
                result.runner_up_confidence, result.margin(), result.tied_count);
        REQUIRE(result.type_name == "FLSUN S1 / S1 Pro");
        REQUIRE(result.margin() >= PrinterDetector::kDetectMinMargin);
        REQUIRE(PrinterDetector::meets_autosave_threshold(result));
    };

    SECTION("a stock FLSUN OS rig is identified by its hardware, whatever its hostname") {
        expect_s1(s1_rig("printer"));
    }

    SECTION("an Open Source Edition rig: FLSUN-S1 hostname, renamed drying box objects") {
        auto hardware = s1_rig("FLSUN-S1-3C2A");
        hardware.heaters = {"extruder", "heater_bed", "heater_generic heater_bed_2",
                            "heater_generic drying_box"};
        hardware.printer_objects = {"delta_calibrate",
                                    "bed_mesh",
                                    "probe",
                                    "gcode_button motor_a",
                                    "gcode_button motor_b",
                                    "gcode_button motor_c",
                                    "output_pin _motor_cali_a",
                                    "heater_generic drying_box",
                                    "heater_fan drying_box_fan",
                                    "heater_generic heater_bed_2"};
        expect_s1(hardware);
    }

    SECTION("a bare delta with an stm32f103xe board is not an S1") {
        // The MCU is shared with MKS Robin Nano boards on older FLSUN deltas.
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .hostname = "flsun",
                                     .printer_objects = {"delta_calibrate"},
                                     .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                     .kinematics = "delta",
                                     .mcu = "stm32f103xe",
                                     .mcu_list = {"stm32f103xe"}};
        hardware.objects_reported = true;
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
                result.runner_up_confidence, result.margin(), result.tied_count);
        REQUIRE(result.type_name != "FLSUN S1 / S1 Pro");
        REQUIRE(std::find(result.contenders.begin(), result.contenders.end(),
                          "FLSUN S1 / S1 Pro") == result.contenders.end());
    }
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: an S1 in another vendor's hostname is not an FLSUN S1",
                 "[printer][detector][flsun]") {
    SECTION("an ender3-s1 cartesian") {
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .fans = {"fan"},
                                     .hostname = "ender3-s1",
                                     .printer_objects = {"bed_mesh", "probe"},
                                     .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                     .kinematics = "cartesian",
                                     .mcu = "stm32f103xe",
                                     .mcu_list = {"stm32f103xe"}};
        hardware.objects_reported = true;
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name);
        REQUIRE(result.type_name != "FLSUN S1 / S1 Pro");
    }

    SECTION("an ender3-s1 hostname before any hardware is reported") {
        // Discovery can run detection on the hostname alone; only the hostname
        // patterns stand between this rig and an FLSUN verdict.
        PrinterHardwareData hardware{.hostname = "ender3-s1"};
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name);
        REQUIRE(result.type_name != "FLSUN S1 / S1 Pro");
    }

    SECTION("a cartesian with a filament drying box heater") {
        PrinterHardwareData hardware{
            .heaters = {"extruder", "heater_bed", "heater_generic drying_box"},
            .fans = {"fan"},
            .hostname = "printer",
            .printer_objects = {"bed_mesh", "heater_generic drying_box"},
            .steppers = {"stepper_x", "stepper_y", "stepper_z"},
            .kinematics = "cartesian"};
        hardware.objects_reported = true;
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name);
        REQUIRE(result.type_name != "FLSUN S1 / S1 Pro");
    }

    SECTION("a retired kobra_s1 hostname is not mistaken for FLSUN") {
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .fans = {"fan"},
                                     .hostname = "kobra_s1",
                                     .printer_objects = {"bed_mesh", "filament_tracker"},
                                     .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                     .kinematics = "corexy"};
        hardware.objects_reported = true;
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name, result.margin());
        REQUIRE(result.type_name != "FLSUN S1 / S1 Pro");
    }
}

// ============================================================================
// LED-Based Detection Tests (AD5M Pro vs AD5M)
// ============================================================================

// ----------------------------------------------------------------------------
// led_required: absence as evidence (prestonbrown/helixscreen#1498)
//
// The chamber light is the only hardware that separates a 5M Pro from a plain
// 5M. Every other heuristic on the two entries is identical, so on a plain
// machine the Pro entry matches everything the plain entry does and the two
// tie; nothing scores the light's absence. led_required does, but only on a
// list that was reported: an empty list before discovery has run says nothing.
// ----------------------------------------------------------------------------

namespace {
PrinterHardwareData plain_ad5m_reporting(bool objects_reported) {
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {"tvocValue", "weightValue"},
                                 .fans = {"fan", "fan_generic exhaust_fan"},
                                 .leds = {},
                                 .hostname = "flashforge-ad5m",
                                 .printer_objects = {"extruder", "heater_bed"},
                                 .kinematics = "corexy"};
    hardware.objects_reported = objects_reported;
    return hardware;
}
} // namespace

// ----------------------------------------------------------------------------
// Creality CFS family (prestonbrown/helixscreen#1498)
// ----------------------------------------------------------------------------

namespace {
/// A CFS-equipped K1-series machine as its object list reports it; the caller
/// names the model through the hostname.
PrinterHardwareData creality_cfs_hardware(const std::string& hostname) {
    PrinterHardwareData hardware{
        .heaters = {"extruder", "heater_bed"},
        .sensors = {"temperature_sensor chamber_temp", "temperature_fan chamber_fan"},
        .fans = {"temperature_fan chamber_fan", "heater_fan hotend_fan"},
        .hostname = hostname,
        .printer_objects = {"extruder", "heater_bed", "temperature_sensor chamber_temp",
                            "temperature_fan chamber_fan", "fan_feedback", "box",
                            "gcode_macro BOX_UNLOAD"},
        .kinematics = "corexy",
        .mcu = "stm32h723xx"};
    hardware.objects_reported = true;
    return hardware;
}
} // namespace

// ============================================================================
// Regression: the real stock Qidi Q2 fingerprint, transcribed from the
// printer.cfg a Q2 owner attached to prestonbrown/helixscreen#1047. The Q2
// ships `[probe_air]` and `[multi_color_controller]` — objects the database
// credited to the Artillery M1 Pro (95) and the Qidi Max 4 (85/70) as if they
// were model-unique. They are QIDI *firmware* objects shared across the range,
// so both sibling entries outscored the Q2's own top signal (M191, 80) and a
// stock Q2 detected as someone else's printer.
//
// Two firmware generations are covered because they fail differently: 1.1.1
// has no M4029 macro, so nothing vetoes the Artillery M1 Pro; 01.01.02+ added
// M4029, which vetoes the M1 Pro but leaves the Qidi Max 4 winning.
// ============================================================================

namespace {
// Stock Q2 on 1.1.1 firmware (no M4029 macro). The 01.01.02+ generation adds
// M4029, which the caller appends for that firmware.
PrinterHardwareData stock_q2_hardware(bool with_m4029) {
    PrinterHardwareData hardware = printer_capture("qidi_q2_stock");
    if (with_m4029) {
        hardware.printer_objects.emplace_back("gcode_macro M4029");
    }
    return hardware;
}
} // namespace

// On 01.01.02+ the M4029 macro vetoes the Artillery M1 Pro on its own, so the
// demoted probe_air is what keeps the Qidi Max 4 off this profile.
//
// This case does NOT yet land on "Qidi Q2". Every Qidi entry fingerprints the
// same shared stock firmware (chamber heater + M141/M191/CLEAR_NOZZLE) and
// nothing model-specific outranks it, so five Qidi models tie on score and the
// winner is decided by database order. Pinning the exact (wrong) winner here
// would just cement that, so this asserts only the regression that is fixed.

// ============================================================================
// A real Qidi Q2, as its owner's machine reports itself
// (prestonbrown/helixscreen#1431)
// ============================================================================

TEST_CASE("PrinterDetector: every decline reason names itself in a bundle",
          "[detector][mismatch]") {
    // These strings are the only record a debug bundle keeps of why no prompt
    // appeared, so an unnamed value reads as "unknown" and loses the diagnosis.
    using MD = PrinterDetector::MismatchDecision;
    for (auto decision : {MD::Warn, MD::NoDetection, MD::ConfidenceTooLow, MD::Ambiguous,
                          MD::MatchesSavedType, MD::SavedTypeNotSpecific, MD::AlreadyDismissed}) {
        const std::string name = PrinterDetector::mismatch_decision_name(decision);
        INFO("decision name: " << name);
        CHECK_FALSE(name.empty());
        CHECK(name != "unknown");
    }
}

TEST_CASE("PrinterDetector: relate_types names the variant between two entries for one machine",
          "[detector][mismatch][type-mismatch]") {
    using Relation = PrinterDetector::TypeRelation;
    const std::vector<PrinterDetector::VariantEntry> entries = {
        {"k2", "K2 Plus", "", ""},
        {"k2_mod", "K2 Plus (mod)", "k2", "mod"},
        {"k2_cfs", "K2 Plus (CFS)", "k2", "CFS"},
        {"voron", "Voron 2.4", "", ""},
        {"orphan", "Orphan (mod)", "no_such_id", "mod"},
        {"unlabeled", "K2 Plus (unlabeled)", "k2", ""},
    };

    SECTION("detected is a variant of the saved base") {
        const auto r = PrinterDetector::relate_types(entries, "K2 Plus", "K2 Plus (mod)");
        CHECK(r.relation == Relation::DetectedVariant);
        CHECK(r.base == "K2 Plus");
        CHECK(r.detected_label == "mod");
        CHECK(r.saved_label.empty());
    }
    SECTION("saved is a variant and the hardware looks like its base") {
        const auto r = PrinterDetector::relate_types(entries, "K2 Plus (CFS)", "K2 Plus");
        CHECK(r.relation == Relation::SavedVariant);
        CHECK(r.base == "K2 Plus");
        CHECK(r.saved_label == "CFS");
        CHECK(r.detected_label.empty());
    }
    SECTION("both are variants of one base") {
        const auto r = PrinterDetector::relate_types(entries, "K2 Plus (CFS)", "K2 Plus (mod)");
        CHECK(r.relation == Relation::SiblingVariants);
        CHECK(r.base == "K2 Plus");
        CHECK(r.saved_label == "CFS");
        CHECK(r.detected_label == "mod");
    }
    SECTION("unrelated models, either way round") {
        for (const auto& [saved, detected] :
             {std::pair{"Voron 2.4", "K2 Plus (mod)"}, std::pair{"K2 Plus", "Voron 2.4"},
              std::pair{"K2 Plus (mod)", "Voron 2.4"}}) {
            INFO(saved << " vs " << detected);
            const auto r = PrinterDetector::relate_types(entries, saved, detected);
            CHECK(r.relation == Relation::DifferentModel);
            CHECK(r.base.empty());
            CHECK(r.saved_label.empty());
            CHECK(r.detected_label.empty());
        }
    }
    SECTION("a variant_of naming no entry is a model of its own") {
        CHECK(PrinterDetector::relate_types(entries, "K2 Plus", "Orphan (mod)").relation ==
              Relation::DifferentModel);
        CHECK(PrinterDetector::relate_types(entries, "Orphan (mod)", "K2 Plus (mod)").relation ==
              Relation::DifferentModel);
    }
    SECTION("a variant without a label is a model of its own") {
        CHECK(PrinterDetector::relate_types(entries, "K2 Plus", "K2 Plus (unlabeled)").relation ==
              Relation::DifferentModel);
    }
    SECTION("a name outside the entries relates to nothing") {
        CHECK(PrinterDetector::relate_types(entries, "My Custom Rig", "K2 Plus").relation ==
              Relation::DifferentModel);
    }
}

// ============================================================================
// A real Elegoo Centauri Carbon, as the bench machine reports itself
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: a Centauri Carbon on COSMOS identifies as itself",
                 "[printer][real_world][elegoo][cc1]") {
    // Its hostname names neither the vendor nor the model, so the identity has
    // to come from the firmware's own macros.
    auto result = PrinterDetector::detect(elegoo_centauri_carbon_hardware());

    REQUIRE(result.detected());
    CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
            result.runner_up_confidence, result.margin());
    CHECK(result.type_name == "Elegoo Centauri Carbon");
    CHECK(result.margin() >= PrinterDetector::kDetectMinMargin);
    CHECK(PrinterDetector::meets_autosave_threshold(result));
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: the COSMOS macros are what identify a Centauri Carbon",
                 "[printer][real_world][elegoo][cc1]") {
    // The evidence this entry leads on must be evidence only this machine
    // carries. Strip the firmware's own macros and what is left - corexy, a
    // chamber sensor, a load cell, M191 - describes a class of enclosed
    // printer, so the entry must stop leading rather than win on a coincidence.
    auto stripped = elegoo_centauri_carbon_hardware();
    auto& objects = stripped.printer_objects;
    const size_t before = objects.size();
    objects.erase(std::remove_if(objects.begin(), objects.end(),
                                 [](const std::string& obj) {
                                     return obj.find("COSMOS") != std::string::npos ||
                                            obj.find("ELEGOO") != std::string::npos;
                                 }),
                  objects.end());
    REQUIRE(objects.size() == before - 3); // the fixture really carries all three

    const auto full = PrinterDetector::detect(elegoo_centauri_carbon_hardware());
    const auto without = PrinterDetector::detect(stripped);

    CAPTURE(full.confidence, without.confidence, without.type_name);
    CHECK(full.confidence > without.confidence);
}

// ============================================================================
// A real FLSUN T1 Pro on stock FLSUN OS
// ============================================================================

namespace {
/// The shipped database entry with this id. The suite runs from the repo root.
nlohmann::json shipped_printer_entry(const std::string& printer_id) {
    namespace fs = std::filesystem;
    const fs::path db_path = fs::current_path() / "assets" / "config" / "printer_database.json";
    REQUIRE(fs::exists(db_path));

    std::ifstream in(db_path);
    REQUIRE(in.is_open());
    nlohmann::json db;
    in >> db;

    for (const auto& printer : db.at("printers")) {
        if (printer.value("id", "") == printer_id) {
            return printer;
        }
    }
    FAIL("no printer database entry with id " << printer_id);
    return {};
}
} // namespace

// ============================================================================
// A score several models share is not an identification
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: an exact tie between models is not a confident detection",
                 "[printer][detection][autosave][tie]") {
    PrinterHardwareData hw;
    hw.fans = {"bed_fans"};
    hw.kinematics = "corexy";
    const auto result = PrinterDetector::detect(hw);
    REQUIRE(result.detected());
    REQUIRE(result.confidence == result.runner_up_confidence);
    REQUIRE(result.type_name != result.runner_up_type_name);
    CHECK_FALSE(PrinterDetector::meets_autosave_threshold(result));
}

TEST_CASE("PrinterDetector: a separated winner is still auto-saved", "[detection][autosave][tie]") {
    // The counterpart to the tie rule: declining a tie must not become
    // declining everything with a runner-up.
    PrinterDetectionResult separated;
    separated.type_name = "Qidi Q2";
    separated.confidence = 92;
    separated.runner_up_type_name = "Qidi Plus 4";
    separated.runner_up_confidence = 91;

    CHECK(PrinterDetector::meets_autosave_threshold(separated));
}

// ============================================================================
// Contradicting the user costs more evidence than filling a blank
// ============================================================================

TEST_CASE("PrinterDetector: telling a user their saved type is wrong needs at least as much "
          "evidence as filling an unset one",
          "[detector][autosave][mismatch]") {
    using MD = PrinterDetector::MismatchDecision;

    // A saved printer type is a statement the user made. Overriding it is a
    // stronger claim than filling in a blank, so it cannot be the cheaper one.
    CHECK(PrinterDetector::kMismatchMinConfidence >= PrinterDetector::kAutosaveMinConfidence);

    for (int confidence = PrinterDetector::kMismatchMinConfidence;
         confidence < PrinterDetector::kAutosaveMinConfidence; ++confidence) {
        INFO("confidence " << confidence);
        // Too weak to be written into an empty config, so too weak to tell
        // someone the type they chose is wrong.
        CHECK(PrinterDetector::classify_type_mismatch("Voron 2.4", "Qidi Plus 4", confidence, "",
                                                      PrinterDetector::kDetectMinMargin) ==
              MD::ConfidenceTooLow);
    }

    // The bar moving up must not silence the warning altogether: a detection
    // strong enough to be persisted is strong enough to be raised.
    CHECK(PrinterDetector::classify_type_mismatch("Voron 2.4", "Qidi Plus 4",
                                                  PrinterDetector::kAutosaveMinConfidence, "",
                                                  PrinterDetector::kDetectMinMargin) == MD::Warn);
    CHECK(PrinterDetector::classify_type_mismatch("Voron 2.4", "Qidi Plus 4", 100, "",
                                                  PrinterDetector::kDetectMinMargin) == MD::Warn);
}

// ============================================================================
// Combined Heuristics - MCU + Other Evidence
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: detect() exposes the runner-up candidate",
                 "[printer][detector][runner_up]") {
    PrinterHardwareData hw;
    hw.hostname = "voron-2.4";
    hw.fans = {"bed_fans", "exhaust_fan", "chamber_fan"};
    hw.kinematics = "corexy";
    const auto result = PrinterDetector::detect(hw);
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.runner_up_type_name != result.type_name);
    REQUIRE(result.runner_up_confidence > 0);
    REQUIRE(result.runner_up_confidence <= result.confidence);
}

// ============================================================================
// Negative Tests - MCU Should Not Cause False Positives
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: Common MCU should not cause false positive",
                 "[printer][mcu][negative]") {
    // STM32F103 is very common, should not trigger high-confidence detection alone
    PrinterHardwareData hardware{.heaters = {"extruder"},
                                 .sensors = {},
                                 .fans = {"fan"},
                                 .leds = {},
                                 .hostname = "test-printer-123",
                                 .printer_objects = {},
                                 .steppers = {"stepper_x", "stepper_y", "stepper_z"},

                                 .kinematics =
                                     "unknown_kinematics", // Use unknown to avoid kinematics match
                                 .mcu = "STM32F103",       // Very common, low confidence
                                 .mcu_list = {"STM32F103"}};

    auto result = PrinterDetector::detect(hardware);

    // A common MCU alone must never reach a score the product would act on
    if (result.detected()) {
        REQUIRE_FALSE(PrinterDetector::meets_autosave_threshold(result));
        REQUIRE(PrinterDetector::classify_type_mismatch("Voron 2.4", result, "") !=
                PrinterDetector::MismatchDecision::Warn);
    }
}

// ============================================================================
// Pre-Print Option Set Database Tests
// ============================================================================

TEST_CASE("PrinterDetector: Pre-print option set lookup", "[printer][capabilities]") {
    SECTION("Unknown printer returns empty option set") {
        auto set = PrinterDetector::get_pre_print_option_set("Nonexistent Printer Model XYZ");

        REQUIRE(set.empty());
        REQUIRE(set.macro_name.empty());
        REQUIRE(set.options.empty());
    }

    SECTION("Printer without pre_print_options section returns empty") {
        // Voron 2.4 exists in database but has no pre_print_options
        auto set = PrinterDetector::get_pre_print_option_set("Voron 2.4");

        // This should return empty since Voron macros are user-customized
        REQUIRE(set.empty());
    }
}

TEST_CASE("PrePrintOptionSet: Helper methods work correctly", "[printer][capabilities]") {
    SECTION("empty() reflects state") {
        PrePrintOptionSet empty_set;
        REQUIRE(empty_set.empty());

        PrePrintOptionSet filled_set;
        filled_set.macro_name = "PRINT_START";
        REQUIRE_FALSE(filled_set.empty());
    }

    SECTION("find() returns expected option pointers") {
        PrePrintOptionSet set;
        PrePrintOption a;
        a.id = "bed_mesh";
        a.strategy_kind = PrePrintStrategyKind::MacroParam;
        a.strategy = PrePrintStrategyMacroParam{"SKIP_BED_MESH", "0", "1", ""};
        set.options.push_back(a);

        PrePrintOption b;
        b.id = "purge_line";
        b.strategy_kind = PrePrintStrategyKind::MacroParam;
        b.strategy = PrePrintStrategyMacroParam{"DISABLE_PRIMING", "false", "true", ""};
        set.options.push_back(b);

        REQUIRE(set.find("bed_mesh") != nullptr);
        REQUIRE(set.find("purge_line") != nullptr);
        REQUIRE(set.find("qgl") == nullptr);
        REQUIRE(set.find("unknown_key") == nullptr);

        const PrePrintOption* bed = set.find("bed_mesh");
        const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&bed->strategy);
        REQUIRE(macro != nullptr);
        REQUIRE(macro->param_name == "SKIP_BED_MESH");
    }
}

// ============================================================================
// User Extensions and Load Status Tests
// ============================================================================

TEST_CASE("PrinterDetector: get_load_status returns valid data", "[printer][extensions]") {
    // Force reload to ensure clean state
    PrinterDetector::reload();

    auto status = PrinterDetector::get_load_status();

    // Should have loaded successfully
    REQUIRE(status.loaded);

    // Should have loaded the bundled database
    REQUIRE(status.total_printers > 50); // Bundled has ~59 printers

    // Should have at least one loaded file (bundled database)
    REQUIRE_FALSE(status.loaded_files.empty());
    REQUIRE(status.loaded_files[0].find("printer_database.json") != std::string::npos);
}

TEST_CASE("PrinterDetector: reload clears and reloads data", "[printer][extensions]") {
    // Get initial status
    auto status1 = PrinterDetector::get_load_status();
    REQUIRE(status1.loaded);

    // Reload
    PrinterDetector::reload();

    // Get status again
    auto status2 = PrinterDetector::get_load_status();
    REQUIRE(status2.loaded);

    // Should have same number of printers (no extensions in test environment)
    REQUIRE(status1.total_printers == status2.total_printers);
}

TEST_CASE("PrinterDetector: list includes Custom/Other and Unknown", "[printer][extensions]") {
    PrinterDetector::reload();

    const auto& names = PrinterDetector::get_list_names();

    REQUIRE_FALSE(names.empty());

    // Custom/Other should be second to last
    REQUIRE(names[names.size() - 2] == "Custom/Other");

    // Unknown should be last
    REQUIRE(names.back() == "Unknown");
}

TEST_CASE("PrinterDetector: get_unknown_list_index returns last index", "[printer][extensions]") {
    PrinterDetector::reload();

    int unknown_idx = PrinterDetector::get_unknown_list_index();
    const auto& names = PrinterDetector::get_list_names();

    REQUIRE(unknown_idx == static_cast<int>(names.size() - 1));
    REQUIRE(names[static_cast<size_t>(unknown_idx)] == "Unknown");
}

TEST_CASE("PrinterDetector: find_list_index is case insensitive", "[printer][extensions]") {
    PrinterDetector::reload();

    // Find a known printer with different cases
    int idx1 = PrinterDetector::find_list_index("Voron 2.4");
    int idx2 = PrinterDetector::find_list_index("voron 2.4");
    int idx3 = PrinterDetector::find_list_index("VORON 2.4");

    // All should find the same index (not Unknown)
    REQUIRE(idx1 == idx2);
    REQUIRE(idx2 == idx3);
    REQUIRE(idx1 != PrinterDetector::get_unknown_list_index());
}

TEST_CASE("PrinterDetector: find_list_index returns Unknown for missing printer",
          "[printer][extensions]") {
    PrinterDetector::reload();

    int idx = PrinterDetector::find_list_index("Nonexistent Printer XYZ123");

    REQUIRE(idx == PrinterDetector::get_unknown_list_index());
}

TEST_CASE("PrinterDetector: get_list_entries aligns with get_list_names", "[printer][selector]") {
    PrinterDetector::reload();

    for (const char* kinematics : {"", "delta", "corexy"}) {
        INFO("kinematics=" << kinematics);
        const auto& names = PrinterDetector::get_list_names(kinematics);
        const auto& entries = PrinterDetector::get_list_entries(kinematics);

        REQUIRE(entries.size() == names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            INFO("index " << i);
            CHECK(entries[i].name == names[i]);
        }
    }
}

TEST_CASE("PrinterDetector: visible machines carry manufacturers, pseudo-entries do not",
          "[printer][selector]") {
    PrinterDetector::reload();

    const auto& entries = PrinterDetector::get_list_entries("");
    REQUIRE(entries.size() > 2);

    // Everything except the two appended pseudo-entries has a vendor to
    // drill into.
    for (size_t i = 0; i + 2 < entries.size(); ++i) {
        INFO("entry=" << entries[i].name);
        CHECK_FALSE(entries[i].manufacturer.empty());
    }
    CHECK(entries[entries.size() - 2].name == "Custom/Other");
    CHECK(entries[entries.size() - 2].manufacturer.empty());
    CHECK(entries[entries.size() - 1].name == "Unknown");
    CHECK(entries[entries.size() - 1].manufacturer.empty());

    // Spot check the database's manufacturer field arrives unmodified.
    const auto voron = std::find_if(entries.begin(), entries.end(), [](const PrinterListEntry& e) {
        return e.name == "Voron 2.4";
    });
    REQUIRE(voron != entries.end());
    CHECK(voron->manufacturer == "Voron");
}

// ============================================================================
// Combined Scoring Tests
// ============================================================================

TEST_CASE("PrinterDetector: Combined scoring rewards multiple matches",
          "[printer][combined_scoring]") {
    PrinterDetector::reload();

    // Doron Velta fingerprint with hostname match - should trigger multiple heuristics
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {},
                                 .leds = {},
                                 .hostname = "doron-velta",
                                 .printer_objects = {"delta_calibrate", "stepper_enable"},
                                 .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                 .kinematics = "delta",
                                 .mcu = "rp2040"};

    auto result = PrinterDetector::detect(hardware);
    CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
            result.runner_up_confidence, result.margin(), result.tied_count);

    REQUIRE(result.detected());
    REQUIRE(result.type_name == "Doron Velta");
    REQUIRE(result.margin() >= PrinterDetector::kDetectMinMargin);
    // Should have multiple matches: kinematics, delta_calibrate, stepper_a, hostname doron,
    // hostname velta
    REQUIRE(result.match_count >= 4);
    // The combined score with bonus exceeds the best single heuristic alone
    REQUIRE(result.confidence > result.best_single_confidence);
}

TEST_CASE("PrinterDetector: Specific printer wins over generic with same confidence",
          "[printer][combined_scoring]") {
    PrinterDetector::reload();

    // Generic delta printer without Doron-specific hostname
    PrinterHardwareData generic_delta{.heaters = {"extruder", "heater_bed"},
                                      .sensors = {},
                                      .fans = {},
                                      .leds = {},
                                      .hostname = "my-delta-printer",
                                      .printer_objects = {"delta_calibrate"},
                                      .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                      .kinematics = "delta"};

    // Specific Doron Velta with hostname
    PrinterHardwareData doron_velta{.heaters = {"extruder", "heater_bed"},
                                    .sensors = {},
                                    .fans = {},
                                    .leds = {},
                                    .hostname = "doron-velta-001",
                                    .printer_objects = {"delta_calibrate"},
                                    .steppers = {"stepper_a", "stepper_b", "stepper_c"},
                                    .kinematics = "delta"};

    auto generic_result = PrinterDetector::detect(generic_delta);
    auto doron_result = PrinterDetector::detect(doron_velta);

    REQUIRE(generic_result.detected());
    REQUIRE(doron_result.detected());

    // Doron Velta should match itself with hostname bonus
    REQUIRE(doron_result.type_name == "Doron Velta");
    REQUIRE(doron_result.margin() >= PrinterDetector::kDetectMinMargin);

    // Doron Velta has more matching heuristics (hostname matches)
    REQUIRE(doron_result.match_count > generic_result.match_count);

    // When confidence ties at 100%, higher match_count wins (tiebreaker)
    // Both may cap at 100%, but Doron Velta wins due to more matches
    REQUIRE(doron_result.confidence >= generic_result.confidence);
}

TEST_CASE("PrinterDetector: Single heuristic match works without bonus",
          "[printer][combined_scoring]") {
    PrinterDetector::reload();

    // Printer with only exhaust_fan - single distinctive match for Voron
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"exhaust_fan"},
                                 .leds = {},
                                 .hostname = "random-hostname-xyz"};

    auto result = PrinterDetector::detect(hardware);
    CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
            result.runner_up_confidence, result.margin(), result.tied_count);

    REQUIRE(result.detected());
    // exhaust_fan is Voron signature
    REQUIRE(result.type_name.find("Voron") != std::string::npos);
    // Single match should have match_count of 1
    REQUIRE(result.match_count == 1);
    // No bonus: the published score is exactly the single heuristic's own
    REQUIRE(result.confidence == result.best_single_confidence);
}

TEST_CASE("PrinterDetector: match_count in result reflects actual matches",
          "[printer][combined_scoring]") {
    PrinterHardwareData hw;
    hw.hostname = "voron-2.4";
    hw.fans = {"bed_fans", "exhaust_fan", "chamber_fan"};
    hw.kinematics = "corexy";
    const auto result = PrinterDetector::detect(hw);
    REQUIRE(result.type_name == "Voron 2.4");
    REQUIRE(result.match_count >= 3);
    REQUIRE(result.reason.find("+") != std::string::npos);
}

// ============================================================================
// Kinematics Filtering Tests
// ============================================================================

TEST_CASE("PrinterDetector: a kinematics filter keeps its own class and drops the others",
          "[printer][kinematics_filter]") {
    PrinterDetector::reload();

    // Kalico's limited_* and hybrid_* kinematics filter as the class they name.
    struct Row {
        const char* filter;
        std::vector<std::string> includes;
        std::vector<std::string> excludes;
    };
    const Row rows[] = {
        {"delta", {"FLSUN V400", "Doron Velta"}, {"Voron 2.4"}},
        {"corexy", {"Voron 2.4"}, {"FLSUN V400"}},
        {"limited_corexy", {"Voron 2.4", "Voron Trident"}, {"FLSUN V400"}},
        {"hybrid_corexy", {"Voron 2.4"}, {"FLSUN V400"}},
        {"limited_cartesian", {"Sovol SV06"}, {"Voron 2.4"}},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.filter) {
            const auto& names = PrinterDetector::get_list_names(row.filter);
            auto listed = [&names](const std::string& name) {
                return std::find(names.begin(), names.end(), name) != names.end();
            };
            for (const auto& name : row.includes) {
                CHECK(listed(name));
            }
            for (const auto& name : row.excludes) {
                CHECK_FALSE(listed(name));
            }
            // Custom/Other and Unknown close every filtered list.
            REQUIRE(names.size() >= 2);
            CHECK(names[names.size() - 2] == "Custom/Other");
            CHECK(names.back() == "Unknown");
        }
    }
}

TEST_CASE("PrinterDetector: Empty filter returns same as unfiltered",
          "[printer][kinematics_filter]") {
    PrinterDetector::reload();

    const auto& unfiltered = PrinterDetector::get_list_names();
    const auto& empty_filter = PrinterDetector::get_list_names("");

    REQUIRE(unfiltered.size() == empty_filter.size());
}

TEST_CASE("PrinterDetector: find_list_index with kinematics filter",
          "[printer][kinematics_filter]") {
    PrinterDetector::reload();

    // Doron Velta should be findable in delta-filtered list
    int doron_idx = PrinterDetector::find_list_index("Doron Velta", "delta");
    REQUIRE(doron_idx != PrinterDetector::get_unknown_list_index("delta"));

    // Voron 2.4 should NOT be findable in delta-filtered list (it's corexy)
    int voron_idx = PrinterDetector::find_list_index("Voron 2.4", "delta");
    REQUIRE(voron_idx == PrinterDetector::get_unknown_list_index("delta"));
}

TEST_CASE("PrinterDetector: Filtered list is smaller than unfiltered",
          "[printer][kinematics_filter]") {
    PrinterDetector::reload();

    const auto& all = PrinterDetector::get_list_names();
    const auto& delta = PrinterDetector::get_list_names("delta");
    const auto& corexy = PrinterDetector::get_list_names("corexy");

    // Filtered lists should be smaller than unfiltered
    REQUIRE(delta.size() < all.size());
    REQUIRE(corexy.size() < all.size());
}

// ============================================================================
// tool_count Heuristic Tests
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: tool_count counts only strict extruder names",
                 "[printer][heuristics][tool_count][1502]") {
    // extruder_mixing passes the loose "extruder"-prefix test discovery uses
    // when building the heater list (printer_discovery.h keeps it deliberately:
    // sensor enumeration), so a name like this reaches hardware.heaters — but
    // it is not a numbered extruder, and a tool count is exactly what the
    // strict grammar exists for. Counting it would inflate 3 real extruders to
    // tool_count_4 and select the AD5X profile.
    REQUIRE_FALSE(helix::is_extruder_name("extruder_mixing"));

    PrinterHardwareData hardware{
        .heaters = {"extruder", "extruder1", "extruder2", "extruder_mixing", "heater_bed"},
        .sensors = {},
        .fans = {},
        .leds = {},
        .hostname = "shop-printer-7",
        .printer_objects = {},
        .steppers = {},
        .kinematics = "corexy"};

    auto result = PrinterDetector::detect(hardware);
    CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
            result.runner_up_confidence, result.margin(), result.tied_count);

    // Three extruders must not satisfy tool_count_4 through a loose-prefix
    // heater name. With tool_count cold, scoring rests on kinematics alone:
    // a corexy tie at 40 broken by database order, so the tie is pinned by
    // margin and the winner by name — under loose counting AD5X's
    // tool_count_4 fires and it wins outright at 88.
    REQUIRE(result.detected());
    REQUIRE(result.margin() == 0);
    REQUIRE(result.type_name == "Voron 2.4");
}

// ============================================================================
// Creator 5 Pro Detection Tests (two firmwares, one machine)
// ============================================================================
// The same hardware runs either the FlashForge fork (K4C5: ff_* printer
// objects) or Z-Mod (ghzserg/z_c5pro: no ff_* objects, but head-dock grab
// buttons). zmod_color alone is NOT a C5 fingerprint - AD5X Z-Mod carries it
// too - so the directional pairs below pin all three ways the entries can
// steal each other's machine.

// ============================================================================
// Creality Nebula Pad host
// ============================================================================

namespace {
bool names_flashforge(const std::string& type_name) {
    return type_name.rfind("FlashForge", 0) == 0;
}
} // namespace

// ============================================================================
// Z-Offset Calibration Strategy Lookup
// ============================================================================

TEST_CASE("Z-offset calibration strategy lookup", "[printer_detector]") {
    SECTION("FlashForge AD5M stock has no firmware_managed strategy") {
        std::string strategy =
            PrinterDetector::get_z_offset_calibration_strategy("FlashForge Adventurer 5M");
        REQUIRE(strategy.empty());
    }

    SECTION("FlashForge AD5M Pro stock has no firmware_managed strategy") {
        std::string strategy =
            PrinterDetector::get_z_offset_calibration_strategy("FlashForge Adventurer 5M Pro");
        REQUIRE(strategy.empty());
    }

    SECTION("Unknown printer returns empty string") {
        std::string strategy =
            PrinterDetector::get_z_offset_calibration_strategy("Some Random Printer");
        REQUIRE(strategy.empty());
    }
}

TEST_CASE("Z-offset persistence flags lookup", "[printer_detector][zoffset]") {
    SECTION("an entry that says nothing persists and keeps its offset across tool changes") {
        CHECK(PrinterDetector::z_offset_persists("FlashForge Adventurer 5X"));
        CHECK_FALSE(PrinterDetector::tool_change_resets_z_offset("FlashForge Adventurer 5X"));
    }

    SECTION("an unknown printer takes the same defaults") {
        CHECK(PrinterDetector::z_offset_persists("Some Random Printer"));
        CHECK_FALSE(PrinterDetector::tool_change_resets_z_offset("Some Random Printer"));
    }
}

// ============================================================================
// Bed Mesh Calibration Gcode Override
// ============================================================================

TEST_CASE("Bed mesh calibration gcode override", "[printer_detector]") {
    PrinterDetector::reload();

    SECTION("Elegoo Centauri Carbon delegates to the firmware's own mesh macro") {
        std::string gcode = PrinterDetector::get_bed_mesh_calibrate_gcode("Elegoo Centauri Carbon");
        REQUIRE_FALSE(gcode.empty());
        // COSMOS's BED_MESH_CALIBRATE heats, homes, cleans, tares and probes by
        // itself. Its M729 and M8213 are emergency stops, the wipe and tare
        // wrappers do not exist, and its M190/M109 also wait for a hot bed to
        // cool, so none of them belongs in front of it.
        for (const char* absent :
             {"M729", "M8213", "M190", "M109", "TEMPERATURE_WAIT", "WITH_WIPE", "SAVE_TARE"}) {
            INFO("template contains " << absent);
            CHECK(gcode.find(absent) == std::string::npos);
        }
        CHECK(gcode.rfind("BED_MESH_CALIBRATE", 0) == 0);
        CHECK(gcode.find("BED_TEMP={bed_temp}") != std::string::npos);
        // The chosen profile rides on the calibration itself: its Z-offset step
        // clears the active mesh, so nothing could be saved from it afterwards.
        CHECK(gcode.find("BED_MESH_CALIBRATE{profile_arg} ") == 0);
    }

    SECTION("Printer without override returns empty") {
        std::string gcode =
            PrinterDetector::get_bed_mesh_calibrate_gcode("FlashForge Adventurer 5M");
        REQUIRE(gcode.empty());
    }

    SECTION("Unknown printer returns empty") {
        std::string gcode = PrinterDetector::get_bed_mesh_calibrate_gcode("Some Random Printer");
        REQUIRE(gcode.empty());
    }

    SECTION("Case insensitive lookup") {
        std::string gcode = PrinterDetector::get_bed_mesh_calibrate_gcode("elegoo centauri carbon");
        REQUIRE(gcode.find("BED_TEMP={bed_temp}") != std::string::npos);
    }
}

TEST_CASE("Screws tilt shipped sequence", "[printer_detector][screws_tilt]") {
    PrinterDetector::reload();

    SECTION("the Centauri Carbon bypasses COSMOS's wrapper and prepares itself") {
        const std::string gcode = PrinterDetector::get_screws_tilt_gcode("elegoo centauri carbon");
        CHECK(gcode.find("{screws_tilt_builtin}") != std::string::npos);
        CHECK(gcode.find("LOAD_CELL_CALIBRATE TARE=TRUE") != std::string::npos);
        CHECK(PrinterDetector::get_screws_tilt_self_prepares("Elegoo Centauri Carbon"));
    }

    SECTION("an entry without a sequence, or no entry, ships nothing") {
        CHECK(PrinterDetector::get_screws_tilt_gcode("FlashForge Adventurer 5M").empty());
        CHECK(PrinterDetector::get_screws_tilt_gcode("Some Random Printer").empty());
        CHECK_FALSE(PrinterDetector::get_screws_tilt_self_prepares("FlashForge Adventurer 5M"));
        CHECK_FALSE(PrinterDetector::get_screws_tilt_self_prepares(""));
    }
}

TEST_CASE("Bed mesh sequence self-preparation flag", "[printer_detector]") {
    PrinterDetector::reload();

    SECTION("the Centauri Carbon's sequence prepares the printer itself") {
        CHECK(PrinterDetector::get_bed_mesh_self_prepares("Elegoo Centauri Carbon"));
        CHECK(PrinterDetector::get_bed_mesh_self_prepares("elegoo centauri carbon"));
    }

    SECTION("an entry without the flag, or no entry, prepares nothing itself") {
        CHECK_FALSE(PrinterDetector::get_bed_mesh_self_prepares("FlashForge Adventurer 5M"));
        CHECK_FALSE(PrinterDetector::get_bed_mesh_self_prepares("Some Random Printer"));
        CHECK_FALSE(PrinterDetector::get_bed_mesh_self_prepares(""));
    }
}

TEST_CASE("Centauri Carbon preset buttons name only commands COSMOS defines",
          "[printer_detector][cc1]") {
    // What COSMOS 26.08 lacks, or defines as an emergency stop (M729, M8213).
    constexpr std::array<const char*, 6> ABSENT = {"WIPE_NOZZLE",
                                                   "BED_MESH_CALIBRATE_WITH_WIPE",
                                                   "FULL_CALIBRATION",
                                                   "LOAD_CELL_SAVE_TARE",
                                                   "M729",
                                                   "M8213"};

    const std::string path = "assets/config/presets/cc1.json";
    INFO("reading " << path << " (tests must run from the repo root)");
    std::ifstream in(path);
    REQUIRE(in.good());
    const auto preset = nlohmann::json::parse(in);
    const auto& macros = preset.at("printer").at("default_macros");

    int buttons = 0;
    for (const auto& [slot, value] : macros.items()) {
        const std::string gcode =
            value.is_string() ? value.get<std::string>() : value.value("gcode", std::string{});
        if (gcode.empty()) {
            continue;
        }
        ++buttons;
        for (const char* absent : ABSENT) {
            INFO(slot << " sends " << gcode);
            CHECK(gcode.find(absent) == std::string::npos);
        }
    }
    // The loop must have looked at the macro buttons, not an empty object.
    REQUIRE(buttons >= 3);
}

// ============================================================================
// Database Compact Tests
// ============================================================================

TEST_CASE("PrinterDetector: compact_database strips heuristics, preserves lookups",
          "[printer_detector][memory]") {
    // Ensure database is loaded, then compact
    PrinterDetector::reload();
    PrinterHardwareData hw;
    hw.hostname = "some-random-printer-xyz";
    PrinterDetector::detect(hw);
    PrinterDetector::compact_database();

    // After compact, image lookup and list building still work
    SECTION("Image lookup works after compact") {
        std::string image = PrinterDetector::get_image_for_printer("Voron 2.4");
        REQUIRE(!image.empty());
    }

    SECTION("List building works after compact") {
        const auto& names = PrinterDetector::get_list_names();
        REQUIRE(names.size() > 2); // More than just Custom/Other + Unknown
    }

    SECTION("Filtered list (kinematics) works after compact") {
        const auto& delta_names = PrinterDetector::get_list_names("delta");
        const auto& all_names = PrinterDetector::get_list_names();
        // Delta-filtered list should be smaller than full list
        REQUIRE(delta_names.size() < all_names.size());
        REQUIRE(delta_names.size() >= 2); // At least Custom/Other + Unknown
    }
}

TEST_CASE("PrinterDetector: get_name_for_preset resolves DB preset field",
          "[printer_detector][preset]") {
    PrinterDetector::reload();
    REQUIRE(PrinterDetector::get_name_for_preset("cc1") == "Elegoo Centauri Carbon");
    REQUIRE(PrinterDetector::get_name_for_preset("CC1") == "Elegoo Centauri Carbon");
    REQUIRE(PrinterDetector::get_name_for_preset("not_a_real_preset_xyz").empty());
    REQUIRE(PrinterDetector::get_name_for_preset("").empty());
}

TEST_CASE("PrinterDetector: get_preset_for_name resolves DB name field",
          "[printer_detector][preset]") {
    PrinterDetector::reload();
    REQUIRE(PrinterDetector::get_preset_for_name("Elegoo Centauri Carbon") == "cc1");
    const auto name = PrinterDetector::get_name_for_preset("cc1");
    REQUIRE(PrinterDetector::get_preset_for_name(name) == "cc1");
    REQUIRE(PrinterDetector::get_preset_for_name("Not A Real Printer").empty());
    REQUIRE(PrinterDetector::get_preset_for_name("").empty());
}

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: No unknown heuristic warning for Venture Delta",
                 "[printer_detector]") {
    // Venture Delta had a config_match heuristic that was removed (unimplemented type).
    // Verify detection still works for delta printers and no config_match entries remain.
    PrinterDetector::reload(); // Ensure fresh database with heuristics
    PrinterHardwareData hw;
    hw.kinematics = "delta";
    hw.hostname = "venture-delta";
    hw.printer_objects = {"delta_calibrate", "gcode_macro START_PRINT"};
    hw.steppers = {"stepper_a", "stepper_b", "stepper_c"};
    hw.build_volume = {.x_min = 0, .x_max = 300, .y_min = 0, .y_max = 300};

    auto result = PrinterDetector::detect(hw);
    CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
            result.runner_up_confidence, result.margin(), result.tied_count);
    // A class-only delta fingerprint ties the delta vendors by design (the
    // known_collisions ratchet in test_printer_database_distinguishability);
    // the contract is strong detection with the tiebreak picking Venture
    // Delta, not a detection margin.
    REQUIRE(result.confidence >= 90);
    REQUIRE(result.ambiguous());
    REQUIRE(result.type_name == "Venture Delta");
}

// ============================================================================
// Seed-config resolution: HELIX_DATA_DIR fallback
// ============================================================================

namespace {

/// Restores cwd on destruction so failed/early-exiting tests don't pollute.
struct CwdGuard {
    std::string original;
    CwdGuard() {
        char buf[4096];
        original = getcwd(buf, sizeof(buf)) != nullptr ? buf : "";
    }
    ~CwdGuard() {
        if (!original.empty()) {
            int r = chdir(original.c_str());
            (void)r;
        }
    }
};

} // namespace

TEST_CASE("PrinterDetector: loads printer_database.json from HELIX_DATA_DIR/assets/config/",
          "[printer][seed_resolution]") {
    namespace fs = std::filesystem;
    auto temp_root =
        fs::temp_directory_path() / ("test_printer_detector_seed_" + std::to_string(getpid()));

    // Use a nested scope so guards (cwd + env) restore BEFORE the final
    // reload that puts the singleton back to its real-database state.
    {
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir");

        std::ofstream(temp_root / "assets" / "config" / "printer_database.json") << R"({
                "version": "test-seed-1.0",
                "printers": [
                    {
                        "id": "test_printer",
                        "name": "Seed Test Printer",
                        "manufacturer": "TestCorp",
                        "kinematics": "cartesian",
                        "heuristics": [
                            {
                                "type": "hostname_match",
                                "field": "hostname",
                                "pattern": "test-seed-host",
                                "confidence": 100,
                                "reason": "Test seed host match"
                            }
                        ]
                    }
                ]
            })";

        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);

        // chdir away from project root so a stray "config/printer_database.json"
        // can't shadow the seed lookup.
        REQUIRE(chdir(temp_root.c_str()) == 0);

        PrinterDetector::reload();
        auto status = PrinterDetector::get_load_status();

        REQUIRE(status.loaded);
        REQUIRE(status.total_printers == 1);
        REQUIRE(!status.loaded_files.empty());
        REQUIRE(status.loaded_files[0].find(temp_root.string()) == 0);

        PrinterHardwareData hw;
        hw.heaters = {"extruder", "heater_bed"};
        hw.hostname = "test-seed-host";
        auto result = PrinterDetector::detect(hw);
        CAPTURE(result.type_name, result.confidence, result.runner_up_type_name,
                result.runner_up_confidence, result.margin(), result.tied_count);
        REQUIRE(result.detected());
        REQUIRE(result.type_name == "Seed Test Printer");
        REQUIRE(result.margin() >= PrinterDetector::kDetectMinMargin);
    }

    // Guards restored: env unset, cwd back at project root. Reload puts
    // PrinterDetector back to the real bundled database for any test that
    // runs after this one in the same binary.
    PrinterDetector::reload();

    fs::remove_all(temp_root);
}

TEST_CASE("PrinterDetector: preset_default survives database array order, not just position",
          "[printer][seed_resolution][preset]") {
    namespace fs = std::filesystem;
    auto temp_root =
        fs::temp_directory_path() / ("test_printer_detector_order_" + std::to_string(getpid()));

    {
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir");

        // Each family's real entries, reversed from printer_database.json's own
        // order so the base model is last, not first. Only "preset" and
        // "preset_default" matter to this lookup.
        std::ofstream(temp_root / "assets" / "config" / "printer_database.json") << R"JSON({
                "version": "test-order-1.0",
                "printers": [
                    {"id": "creality_k1_max_cfs", "name": "Creality K1 Max CFS", "preset": "k1"},
                    {"id": "creality_k1_max", "name": "Creality K1 Max", "preset": "k1"},
                    {"id": "creality_k1_cfs", "name": "Creality K1 CFS", "preset": "k1"},
                    {"id": "creality_k1", "name": "Creality K1", "preset": "k1", "preset_default": true},

                    {"id": "creality_k1c_cfs", "name": "Creality K1C CFS", "preset": "k1c"},
                    {"id": "creality_k1c", "name": "Creality K1C", "preset": "k1c", "preset_default": true},

                    {"id": "anycubic_kobra_3_v2", "name": "Anycubic Kobra 3 V2", "preset": "anycubic_kobra_3"},
                    {"id": "anycubic_kobra_3", "name": "Anycubic Kobra 3", "preset": "anycubic_kobra_3", "preset_default": true}
                ]
            })JSON";

        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);
        REQUIRE(chdir(temp_root.c_str()) == 0);

        PrinterDetector::reload();

        CHECK(PrinterDetector::get_name_for_preset("k1") == "Creality K1");
        CHECK(PrinterDetector::get_name_for_preset("k1c") == "Creality K1C");
        CHECK(PrinterDetector::get_name_for_preset("anycubic_kobra_3") == "Anycubic Kobra 3");
    }

    PrinterDetector::reload();
    fs::remove_all(temp_root);
}

// ============================================================================
// hide_manual_z_calibration — opt-in per printer
// ============================================================================

TEST_CASE("PrinterDetector: hide_manual_z_calibration defaults to false",
          "[printer_detector][tool_offset_cal]") {
    // No bundled printer opts in: a stock klipper-toolchanger's example macro
    // writes nothing for T0, so the paper test must stay unless a database
    // entry says the routine covers the reference tool.
    REQUIRE_FALSE(PrinterDetector::hide_manual_z_calibration("Snapmaker U1"));
    REQUIRE_FALSE(PrinterDetector::hide_manual_z_calibration("Some Random Printer"));
}

TEST_CASE("PrinterDetector: hide_manual_z_calibration reads the database field",
          "[printer_detector][tool_offset_cal][seed_resolution]") {
    namespace fs = std::filesystem;
    auto temp_root =
        fs::temp_directory_path() / ("test_printer_detector_toolcal_" + std::to_string(getpid()));

    {
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir");

        std::ofstream(temp_root / "assets" / "config" / "printer_database.json") << R"({
                "version": "test-toolcal-1.0",
                "printers": [
                    {
                        "id": "covers_reference",
                        "name": "Covers Reference Tool",
                        "manufacturer": "TestCorp",
                        "hide_manual_z_calibration": true
                    },
                    {
                        "id": "stock_toolchanger",
                        "name": "Stock Toolchanger",
                        "manufacturer": "TestCorp"
                    }
                ]
            })";

        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);
        REQUIRE(chdir(temp_root.c_str()) == 0);

        PrinterDetector::reload();
        REQUIRE(PrinterDetector::get_load_status().total_printers == 2);

        REQUIRE(PrinterDetector::hide_manual_z_calibration("Covers Reference Tool"));
        REQUIRE(PrinterDetector::hide_manual_z_calibration("covers reference tool"));
        REQUIRE_FALSE(PrinterDetector::hide_manual_z_calibration("Stock Toolchanger"));
    }

    PrinterDetector::reload();
    fs::remove_all(temp_root);
}

// ============================================================================
// print_start_default_phases — per-printer first-print ETA defaults
// ============================================================================

TEST_CASE("PrinterDetector: print_start_default_phases returns each printer's measured durations",
          "[printer][preprint]") {
    using Phase = helix::PrintStartPhase;
    auto at = [](Phase p) { return static_cast<int>(p); };
    // Heating phases stay absent everywhere: the thermal model owns them.
    struct Row {
        const char* printer;
        std::map<int, int> phases;
    };
    const Row rows[] = {
        // COSMOS PRINT_START: G28 ~23s, the default 1-minute heat soak, stored
        // mesh load under 1s, KAMP purge ~13s. No QGL, Z tilt or wipe.
        {"Elegoo Centauri Carbon",
         {{at(Phase::HOMING), 25},
          {at(Phase::SOAKING), 60},
          {at(Phase::BED_MESH), 2},
          {at(Phase::PURGING), 15}}},
        {"Creality K1C", {}},
        {"Creality K2 Plus", {}},
        {"Creality K2 Pro", {}},
        // No print_start_default_phases field: the generic defaults apply.
        {"Creality Ender 3", {}},
        {"Not A Real Printer", {}},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.printer) {
            CHECK(PrinterDetector::get_print_start_default_phases(row.printer) == row.phases);
        }
    }
}

TEST_CASE("PrinterDetector: Centauri Carbon uses the COSMOS pre-print profile",
          "[printer][preprint]") {
    REQUIRE(PrinterDetector::get_print_start_profile("Elegoo Centauri Carbon") == "cosmos_cc1");
}

TEST_CASE("PrinterDetector: pre-print lookups share one entry, and rates name known heaters",
          "[printer][preprint]") {
    namespace fs = std::filesystem;
    auto temp_root =
        fs::temp_directory_path() / ("test_printer_detector_preprint_" + std::to_string(getpid()));
    // Runs after the env and cwd guards below restore, even when a REQUIRE
    // throws, so later tests get the bundled database back.
    struct RestoreDatabase {
        fs::path root;
        ~RestoreDatabase() {
            PrinterDetector::reload();
            fs::remove_all(root);
        }
    } restore{temp_root};
    {
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir");
        std::ofstream(temp_root / "assets" / "config" / "printer_database.json") << R"({
                "version": "test-preprint-1.0",
                "printers": [
                    {
                        "id": "preprint_printer",
                        "name": "Preprint Test Printer",
                        "manufacturer": "TestCorp",
                        "kinematics": "cartesian",
                        "print_start_profile": "preprint_profile",
                        "print_start_default_phases": { "HOMING": 12, "SOAKING": 30, "HEATING_BED": 99 },
                        "thermal_rates": { "heater_bed": 3.5, "bed": 9.0, "extruder": -1 },
                        "heuristics": [
                            {
                                "type": "hostname_match",
                                "field": "hostname",
                                "pattern": "test-preprint-host",
                                "confidence": 100,
                                "reason": "Test preprint host match"
                            }
                        ]
                    }
                ]
            })";
        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);
        REQUIRE(chdir(temp_root.c_str()) == 0);
        PrinterDetector::reload();
        REQUIRE(PrinterDetector::get_load_status().total_printers == 1);

        // Names match case-insensitively, the same way for every field.
        REQUIRE(PrinterDetector::get_print_start_profile("PREPRINT TEST PRINTER") ==
                "preprint_profile");
        // Heating has no stored duration, so its default is dropped.
        const auto phases =
            PrinterDetector::get_print_start_default_phases("preprint test printer");
        REQUIRE(phases.size() == 2);
        REQUIRE(phases.at(static_cast<int>(helix::PrintStartPhase::HOMING)) == 12);
        REQUIRE(phases.at(static_cast<int>(helix::PrintStartPhase::SOAKING)) == 30);

        // "bed" is no heater the rate model reads back, and -1 is no rate.
        const auto rates = PrinterDetector::get_thermal_rates("Preprint Test Printer");
        REQUIRE(rates.size() == 1);
        REQUIRE(rates.at("heater_bed") == Catch::Approx(3.5f));
    }
}

// printer_database.json is user-editable on the device and the firmware build
// has no exceptions, so a malformed file must degrade to "no printers", never throw.
TEST_CASE("PrinterDetector: a malformed database degrades without throwing",
          "[printer][db_shape]") {
    namespace fs = std::filesystem;
    auto temp_root =
        fs::temp_directory_path() / ("test_printer_detector_shape_" + std::to_string(getpid()));
    struct RestoreDatabase {
        fs::path root;
        ~RestoreDatabase() {
            PrinterDetector::reload();
            fs::remove_all(root);
        }
    } restore{temp_root};

    // An extension that both adds a printer and defines a filter set exercises
    // every write merge_user_extensions makes into the bundled document.
    const char* extension = R"({
            "console_filter_sets": { "noise": { "patterns": ["x"] } },
            "printers": [ { "id": "ext", "name": "Extension Printer", "probe_type": "eddy" } ]
        })";

    std::string bundled;
    SECTION("top level is an array") {
        bundled = "[]";
    }
    SECTION("printers is an object") {
        bundled = R"({ "printers": { "a": 1 } })";
    }

    {
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir" / "printer_database.d");
        std::ofstream(temp_root / "assets" / "config" / "printer_database.json") << bundled;
        std::ofstream(temp_root / "config_dir" / "printer_database.d" / "ext.json") << extension;
        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);
        REQUIRE(chdir(temp_root.c_str()) == 0);

        REQUIRE_NOTHROW(PrinterDetector::reload());
        REQUIRE_FALSE(PrinterDetector::get_load_status().loaded);
        REQUIRE(PrinterDetector::get_probe_type("Extension Printer").empty());
        REQUIRE(PrinterDetector::get_list_names() ==
                std::vector<std::string>{"Custom/Other", "Unknown"});
    }
}

TEST_CASE("PrinterDetector: an unchanged broken database is read once, not once per lookup",
          "[printer][db_shape]") {
    namespace fs = std::filesystem;
    auto temp_root =
        fs::temp_directory_path() / ("test_printer_detector_retry_" + std::to_string(getpid()));
    struct RestoreDatabase {
        fs::path root;
        ~RestoreDatabase() {
            PrinterDetector::reload();
            fs::remove_all(root);
        }
    } restore{temp_root};
    {
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir");
        const fs::path db = temp_root / "assets" / "config" / "printer_database.json";
        std::ofstream(db) << "[]";
        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);
        REQUIRE(chdir(temp_root.c_str()) == 0);

        PrinterDetector::reload();
        REQUIRE_FALSE(PrinterDetector::get_load_status().loaded);

        // A valid file under the broken one's mtime is not re-read: only the
        // first lookup paid for the read.
        const auto broken_mtime = fs::last_write_time(db);
        std::ofstream(db, std::ios::trunc)
            << R"({ "printers": [ { "id": "r", "name": "Retry Printer", "probe_type": "eddy" } ] })";
        fs::last_write_time(db, broken_mtime);
        REQUIRE(PrinterDetector::get_probe_type("Retry Printer").empty());

        // Editing the file is what earns another attempt.
        fs::last_write_time(db, broken_mtime + std::chrono::seconds(5));
        REQUIRE(PrinterDetector::get_probe_type("Retry Printer") == "eddy");
    }
}

TEST_CASE("PrinterDetector: wrong-typed entry fields read as absent", "[printer][db_shape]") {
    namespace fs = std::filesystem;
    auto temp_root =
        fs::temp_directory_path() / ("test_printer_detector_types_" + std::to_string(getpid()));
    struct RestoreDatabase {
        fs::path root;
        ~RestoreDatabase() {
            PrinterDetector::reload();
            fs::remove_all(root);
        }
    } restore{temp_root};
    {
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir");
        std::ofstream(temp_root / "assets" / "config" / "printer_database.json") << R"({
                "printers": [
                    7,
                    {
                        "id": "typed", "name": "Typed Printer", "preset": 5,
                        "print_start_profile": 42, "probe_type": ["eddy"],
                        "image": {}, "toolhead_style": null, "enclosed": [true],
                        "calibration": "none", "thermal_rates": [1],
                        "print_start_default_phases": { "HOMING": "slow" },
                        "console_filters": "noise", "console_filter_patterns": [3],
                        "aliases": 9, "belt_tensioner": 3,
                        "variant_of": 4, "variant_label": ["x"]
                    },
                    { "id": "4", "name": "Base Printer" }
                ]
            })";
        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);
        REQUIRE(chdir(temp_root.c_str()) == 0);
        REQUIRE_NOTHROW(PrinterDetector::reload());
        REQUIRE_NOTHROW(PrinterDetector::compact_database());

        const std::string name = "Typed Printer";
        REQUIRE(PrinterDetector::get_print_start_profile(name).empty());
        REQUIRE(PrinterDetector::get_probe_type(name).empty());
        REQUIRE(PrinterDetector::get_image_for_printer(name).empty());
        REQUIRE(PrinterDetector::get_toolhead_style(name).empty());
        REQUIRE(PrinterDetector::get_preset_for_name(name).empty());
        REQUIRE_FALSE(PrinterDetector::is_enclosed(name));
        REQUIRE(PrinterDetector::get_belt_tensioner(name) ==
                helix::calibration::BeltTensionerKind::UNKNOWN);
        REQUIRE(PrinterDetector::get_bed_mesh_calibrate_gcode(name).empty());
        REQUIRE_FALSE(PrinterDetector::get_bed_mesh_self_prepares(name));
        REQUIRE(PrinterDetector::get_thermal_rates(name).empty());
        REQUIRE(PrinterDetector::get_print_start_default_phases(name).empty());
        REQUIRE(PrinterDetector::get_console_filter_patterns(name).empty());
        REQUIRE(PrinterDetector::canonical_type_name("Old Name") == "Old Name");
        REQUIRE(PrinterDetector::get_name_for_preset("5").empty());
        REQUIRE(PrinterDetector::relate_types(name, "Base Printer").relation ==
                PrinterDetector::TypeRelation::DifferentModel);
    }
}

TEST_CASE("PrinterDetector: belt_tensioner maps the database string to a kind",
          "[printer][belt_tension]") {
    using helix::calibration::BeltTensionerKind;

    SECTION("Voron 2.4 and Trident are screw-tensioned") {
        REQUIRE(PrinterDetector::get_belt_tensioner("Voron 2.4") == BeltTensionerKind::SCREW);
        REQUIRE(PrinterDetector::get_belt_tensioner("Voron Trident") == BeltTensionerKind::SCREW);
    }

    SECTION("a printer with no belt_tensioner entry is UNKNOWN") {
        REQUIRE(PrinterDetector::get_belt_tensioner("Creality K1C") == BeltTensionerKind::UNKNOWN);
    }

    SECTION("an unknown printer name is UNKNOWN") {
        REQUIRE(PrinterDetector::get_belt_tensioner("Not A Real Printer") ==
                BeltTensionerKind::UNKNOWN);
    }

    SECTION("screw, spring, empty and unrecognised strings") {
        namespace fs = std::filesystem;
        auto temp_root =
            fs::temp_directory_path() / ("test_printer_detector_belt_" + std::to_string(getpid()));
        struct RestoreDatabase {
            fs::path root;
            ~RestoreDatabase() {
                PrinterDetector::reload();
                fs::remove_all(root);
            }
        } restore{temp_root};
        helix::ScopedEnv data_g("HELIX_DATA_DIR", nullptr);
        helix::ScopedEnv config_g("HELIX_CONFIG_DIR", nullptr);
        CwdGuard cwd_g;

        fs::remove_all(temp_root);
        fs::create_directories(temp_root / "assets" / "config");
        fs::create_directories(temp_root / "config_dir");
        std::ofstream(temp_root / "assets" / "config" / "printer_database.json") << R"({
                "printers": [
                    { "id": "a", "name": "Screw Printer", "belt_tensioner": "screw" },
                    { "id": "b", "name": "Spring Printer", "belt_tensioner": "spring" },
                    { "id": "c", "name": "Empty Printer", "belt_tensioner": "" },
                    { "id": "d", "name": "Odd Printer", "belt_tensioner": "cam" }
                ]
            })";
        setenv("HELIX_DATA_DIR", temp_root.c_str(), 1);
        setenv("HELIX_CONFIG_DIR", (temp_root / "config_dir").c_str(), 1);
        REQUIRE(chdir(temp_root.c_str()) == 0);
        PrinterDetector::reload();

        REQUIRE(PrinterDetector::get_belt_tensioner("Screw Printer") == BeltTensionerKind::SCREW);
        REQUIRE(PrinterDetector::get_belt_tensioner("Spring Printer") == BeltTensionerKind::SPRING);
        REQUIRE(PrinterDetector::get_belt_tensioner("Empty Printer") == BeltTensionerKind::UNKNOWN);
        REQUIRE(PrinterDetector::get_belt_tensioner("Odd Printer") == BeltTensionerKind::UNKNOWN);
    }
}

TEST_CASE("PrinterDetector: print_start_default_phases give each Voron only its own leveling",
          "[printer][preprint]") {
    // The generic table charges every printer for both QGL and Z_TILT. A
    // Voron runs at most one of them, and the two small machines run neither,
    // so an unqualified table is the difference between a 260s first-print
    // estimate and the truth. Heating stays absent: the thermal model owns it.
    const int HOMING = static_cast<int>(helix::PrintStartPhase::HOMING);
    const int QGL = static_cast<int>(helix::PrintStartPhase::QGL);
    const int Z_TILT = static_cast<int>(helix::PrintStartPhase::Z_TILT);
    const int BED_MESH = static_cast<int>(helix::PrintStartPhase::BED_MESH);
    const int CLEANING = static_cast<int>(helix::PrintStartPhase::CLEANING);
    const int PURGING = static_cast<int>(helix::PrintStartPhase::PURGING);
    const int HEATING_BED = static_cast<int>(helix::PrintStartPhase::HEATING_BED);

    SECTION("Trident tilts its bed and never levels a gantry") {
        auto phases = PrinterDetector::get_print_start_default_phases("Voron Trident");
        REQUIRE(phases.size() == 5);
        REQUIRE(phases[Z_TILT] == 45);
        REQUIRE(phases.count(QGL) == 0);
        REQUIRE(phases[HOMING] == 30);
        REQUIRE(phases[BED_MESH] == 90);
        REQUIRE(phases[CLEANING] == 20);
        REQUIRE(phases[PURGING] == 15);
        REQUIRE(phases.count(HEATING_BED) == 0);
    }

    SECTION("2.4 levels its gantry and never tilts the bed") {
        auto phases = PrinterDetector::get_print_start_default_phases("Voron 2.4");
        REQUIRE(phases.size() == 5);
        REQUIRE(phases[QGL] == 60);
        REQUIRE(phases.count(Z_TILT) == 0);
        REQUIRE(phases[HOMING] == 30);
        REQUIRE(phases[BED_MESH] == 90);
    }

    SECTION("V0 and Switchwire run neither") {
        for (const char* name : {"Voron 0.2", "Voron Switchwire"}) {
            CAPTURE(name);
            auto phases = PrinterDetector::get_print_start_default_phases(name);
            REQUIRE(phases.size() == 4);
            REQUIRE(phases.count(QGL) == 0);
            REQUIRE(phases.count(Z_TILT) == 0);
            REQUIRE(phases.count(HOMING) == 1);
            REQUIRE(phases.count(BED_MESH) == 1);
        }
    }
}

// ============================================================================
// apply_preset_with_variants — ZMOD vs ForgeX firmware variants
// ============================================================================

namespace helix {

/// Set up the env vars + temp dir + symlink to the shipped printer DB so
/// apply_preset_with_variants finds the seed-bundle preset files via
/// helix::find_readable. Mirrors PresetConfigFixture in test_config_preset.cpp
/// but local to this file to avoid cross-file fixture coupling.
class VariantPresetFixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string saved_config_dir_;
    std::string saved_data_dir_;
    bool had_config_dir_ = false;
    bool had_data_dir_ = false;

    void SetUp() {
        namespace fs = std::filesystem;
        // Per-process, like the seed fixture above. A fixed name is shared by
        // every shard that runs a test on this fixture, and remove_all() in one
        // shard races create_directories() in another: "cannot remove:
        // Directory not empty". Which shard gets which test moves whenever a
        // test is added anywhere in the suite, so the collision surfaces at
        // random.
        temp_dir = (fs::temp_directory_path() / ("helix_variant_test_" + std::to_string(getpid())))
                       .string();
        fs::create_directories(temp_dir + "/presets");
        fs::create_directories(temp_dir + "/assets/config/presets");

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        if (const char* prev = std::getenv("HELIX_DATA_DIR")) {
            saved_data_dir_ = prev;
            had_data_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);
        setenv("HELIX_DATA_DIR", temp_dir.c_str(), 1);

        std::error_code ec;
        fs::path real_db = fs::current_path() / "assets" / "config" / "printer_database.json";
        if (fs::exists(real_db)) {
            fs::path linked = fs::path(temp_dir) / "assets" / "config" / "printer_database.json";
            fs::create_symlink(real_db, linked, ec);
            if (ec) {
                fs::copy_file(real_db, linked, fs::copy_options::overwrite_existing, ec);
            }
        }

        ConfigTestAccess::path(config) = temp_dir + "/settings.json";
        ConfigTestAccess::active_printer_id(config) = "default";
        ConfigTestAccess::data(config) = {
            {"active_printer_id", "default"},
            {"printers",
             {{"default", {{"moonraker_host", "127.0.0.1"}, {"wizard_completed", false}}}}}};
    }

    void TearDown() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        std::filesystem::remove_all(temp_dir);
        if (had_config_dir_)
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        else
            unsetenv("HELIX_CONFIG_DIR");
        if (had_data_dir_)
            setenv("HELIX_DATA_DIR", saved_data_dir_.c_str(), 1);
        else
            unsetenv("HELIX_DATA_DIR");
    }

    void write_seed_preset(const std::string& name, const std::string& part_fan_role) {
        std::string path = temp_dir + "/assets/config/presets/" + name + ".json";
        std::ofstream f(path);
        f << R"({"preset": ")" << name
          << R"(", "wizard_completed": false, "printer": {"fans": {"part": ")" << part_fan_role
          << R"("}, "heaters": {"bed": "heater_bed"}}})";
    }
};

} // namespace helix

// ============================================================================
// Type Mismatch Warning Decider
// ============================================================================

namespace {
/// The scalar decision for a detection that separated its rivals: what a
/// caller holding a bare confidence and no candidate field passes.
bool warns_when_separated(const std::string& saved, const std::string& detected, int confidence,
                          const std::string& flag) {
    return PrinterDetector::classify_type_mismatch(saved, detected, confidence, flag,
                                                   PrinterDetector::kDetectMinMargin) ==
           PrinterDetector::MismatchDecision::Warn;
}
} // namespace

TEST_CASE("classify_type_mismatch warn table", "[detector][mismatch]") {
    using PD = PrinterDetector;
    const std::string none; // "" — flag never shown
    const std::string ad5m = "FlashForge Adventurer 5M Pro";
    const std::string trident = "Voron Trident";

    SECTION("high-confidence different type warns") {
        REQUIRE(warns_when_separated(ad5m, trident, 85, none));
    }
    SECTION("boundary: the bar warns, one below it does not") {
        // Read from the constant, not spelled out: the warning bar is derived
        // from the auto-save bar so the two cannot land in an order where
        // contradicting a user costs less evidence than filling a blank.
        REQUIRE(warns_when_separated(ad5m, trident, PD::kMismatchMinConfidence, none));
        REQUIRE_FALSE(warns_when_separated(ad5m, trident, PD::kMismatchMinConfidence - 1, none));
    }
    SECTION("same type never warns") {
        REQUIRE_FALSE(warns_when_separated(ad5m, ad5m, 95, none));
    }
    SECTION("deliberate picks and undetected saves are exempt") {
        REQUIRE_FALSE(warns_when_separated("Custom/Other", trident, 95, none));
        REQUIRE_FALSE(warns_when_separated("Unknown", trident, 95, none));
        REQUIRE_FALSE(warns_when_separated("", trident, 95, none));
    }
    SECTION("flag suppresses until the saved type changes") {
        REQUIRE_FALSE(warns_when_separated(ad5m, trident, 85, ad5m));
        // User re-ran wizard and picked ANOTHER wrong type: re-arm once.
        const std::string k1max = "Creality K1 Max (with CFS)";
        REQUIRE(warns_when_separated(k1max, trident, 85, ad5m));
    }
}

// ============================================================================
// Renamed Database Entries
// ============================================================================

TEST_CASE("canonical_type_name resolves former database names", "[detector][mismatch]") {
    using PD = PrinterDetector;

    SECTION("a name the database still publishes is returned untouched") {
        REQUIRE(PD::canonical_type_name("Voron Trident") == "Voron Trident");
        REQUIRE(PD::canonical_type_name("Voron 0.2") == "Voron 0.2");
    }
    SECTION("every recorded rename resolves to the current name") {
        REQUIRE(PD::canonical_type_name("Voron 0.1") == "Voron 0.2");

        REQUIRE(PD::canonical_type_name("Zero G Nebula 255") == "Zero G Mercury One.1 Nebula");
        REQUIRE(PD::canonical_type_name("Zero G Nebula 370") == "Zero G Mercury One.1 Plus Nebula");
    }
    SECTION("unknown and empty names pass through unchanged") {
        REQUIRE(PD::canonical_type_name("") == "");
        REQUIRE(PD::canonical_type_name("Custom/Other") == "Custom/Other");
        REQUIRE(PD::canonical_type_name("Some Rig That Is Not In The Database") ==
                "Some Rig That Is Not In The Database");
    }
}

// The bug this closes: a config written before the entry was renamed detects
// correctly and is still told it is set up as the wrong printer.
TEST_CASE("a renamed entry does not warn once the saved name is canonicalised",
          "[detector][mismatch]") {
    using PD = PrinterDetector;
    const std::string none;
    const std::string legacy = "Voron 0.1";
    const std::string current = "Voron 0.2";

    // Raw comparison — what shipped before the alias lookup, and why the
    // prompt fired at 100% confidence on a printer that was never mis-set.
    REQUIRE(warns_when_separated(legacy, current, 100, none));

    // Through the resolver, the same pair is the same printer.
    REQUIRE_FALSE(warns_when_separated(PD::canonical_type_name(legacy), current, 100, none));

    // A genuine mismatch still warns after canonicalisation — the resolver
    // must not swallow real disagreement.
    REQUIRE(warns_when_separated(PD::canonical_type_name(legacy), "Voron Trident", 100, none));
}

// ============================================================================
// LED Heuristic Exclusivity (#1284)
// ============================================================================

TEST_CASE_METHOD(PrinterDetectorFixture,
                 "PrinterDetector: chamber_light LED alone stays below high-confidence threshold",
                 "[printer][1284]") {
    // Minimal reproduction: the ONLY distinctive signal is an LED whose name
    // contains 'chamber_l'. No FlashForge sensors, no FlashForge hostname.
    PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                 .sensors = {},
                                 .fans = {"fan", "heater_fan hotend_fan"},
                                 .leds = {"led chamber_light"},
                                 .hostname = "mainsailos"};

    auto result = PrinterDetector::detect(hardware);

    // May still be suggested (corroborating signal), but never >=70 where it
    // would override the saved type or arm the mismatch warning.
    REQUIRE(result.confidence < kHeuristicClassCeiling);
    REQUIRE(PrinterDetector::classify_type_mismatch("Voron 2.4", result, "") !=
            PrinterDetector::MismatchDecision::Warn);
}

// ============================================================================
// Auto-Save Confidence Gate
// ============================================================================
//
// The installer (scripts/lib/installer/printer_seed.sh) has always
// gated the install-time preset seed on HELIX_DETECT_MIN_CONFIDENCE=85 AND
// HELIX_DETECT_MIN_MARGIN=10. The runtime path had no equivalent bar: it wrote
// whatever scored above zero, and PRINTER_TYPE being non-empty then short-
// circuits detection permanently. These pin the runtime path to the same
// standard the installer already used.

TEST_CASE("meets_autosave_threshold table", "[detector][autosave]") {
    using PD = PrinterDetector;
    auto res = [](int conf, int runner_up) {
        PrinterDetectionResult r;
        r.type_name = "FlashForge Adventurer 5M Pro";
        r.confidence = conf;
        r.runner_up_confidence = runner_up;
        return r;
    };

    SECTION("unambiguous high-confidence match saves") {
        REQUIRE(PD::meets_autosave_threshold(res(100, 0)));
    }
    SECTION("boundary: 85 saves, 84 does not") {
        REQUIRE(PD::meets_autosave_threshold(res(85, 0)));
        REQUIRE_FALSE(PD::meets_autosave_threshold(res(84, 0)));
    }
    SECTION("a clear lead over the nearest rival outcome saves") {
        // The runner-up is the best candidate offering a DIFFERENT outcome, so
        // a gap here is a gap between two answers the user would tell apart.
        REQUIRE(PD::meets_autosave_threshold(res(100, 91)));
        REQUIRE(PD::meets_autosave_threshold(res(100, 100 - PD::kDetectMinMargin)));
    }
    SECTION("a rival outcome inside the margin blocks the save") {
        // Writing either name into the config makes a coin flip permanent: a
        // non-empty printer type short-circuits every later detection attempt.
        REQUIRE_FALSE(PD::meets_autosave_threshold(res(100, 100)));
        REQUIRE_FALSE(PD::meets_autosave_threshold(res(100, 101 - PD::kDetectMinMargin)));
    }
    SECTION("undetected never saves") {
        REQUIRE_FALSE(PD::meets_autosave_threshold(res(0, 0)));
    }
    SECTION("an empty type name never saves regardless of score") {
        PrinterDetectionResult r;
        r.type_name = "";
        r.confidence = 100;
        r.runner_up_confidence = 0;
        REQUIRE_FALSE(PD::meets_autosave_threshold(r));
    }
}

// ============================================================================
// Reported Voron 2.4 misdetection (debug bundle QS846GMM)
// ============================================================================

// Build volume reaches detection from configfile.settings
// ============================================================================

// The creality_k2_plus and creality_k2_pro database entries are identical apart
// from their k2plus/k2pro hostname heuristic and their build_volume_range
// window. A host named plainly "creality-k2" matches neither hostname pattern,
// so the bed is the ONLY thing that can tell the two apart. Both windows opt
// in as separators: a bed inside one entry's window gives that entry a real
// margin over the sibling, enough to auto-save; a bed in neither window leaves
// the family tie, which stays a guess nothing may persist. The windows read
// the bed size the firmware config declares (Creality's gcode_macro
// product_param): on both real machines stepper travel overshoots the bed to
// reach the nozzle-clean position. A config that declares no bed leaves the
// tie. The discovery-side parse fills the reading before detection runs; the
// safety-limits fetch, the other writer of the volume, runs after it.

// The two real K2s on record: the K2 Plus capture and the community K2 Pro
// report. A hostname carrying the model ("K2Plus-50C1", "K2Pro") names the
// machine on its own. Under a plain "k2" hostname the declared bed is the only
// fact separating the siblings. The Plus capture declares its own; the Pro
// report records none, so one section supplies Creality's 300mm spec.

// A build volume is shared evidence: at 215-235mm up to 15 database windows
// cover the same point, and about 10 do at 300mm. It can corroborate an
// identification made on other grounds, but it must never make one by itself -
// otherwise any rig that reports a bed size gets a name it did not earn. The
// scorer enforces that by refusing to let build_volume_range supply the base
// score, so a printer whose only match is its volume scores nothing at all.
TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: a build volume alone cannot identify",
                 "[printer][build_volume][detector]") {
    SECTION("a bare rig reporting only a 200mm bed is not a Doron Velta") {
        // doron_velta carries the database's highest surviving volume
        // confidence (80). None of its other heuristics - delta kinematics,
        // delta_calibrate, stepper_a, the Fysetc board, its hostnames - is
        // present here, so the volume is the entry's ONLY possible match.
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .hostname = "printer",
                                     .printer_objects = {"bed_mesh"},
                                     .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                     .build_volume = BuildVolume{0, 200, 0, 200, 0}};
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence, result.reason);
        REQUIRE(result.type_name != "Doron Velta");
    }

    SECTION("a featureless rig stays below the mismatch-warning bar at every common bed size") {
        // Generic hostname, one Z, bed_mesh only. Such a rig scored 43-45 and
        // stayed silent before the volume reached detection; it must not start
        // crossing the 70-point bar that triggers a "this is not your printer"
        // warning just because it now reports a bed size.
        for (float bed :
             {180.0f, 200.0f, 215.0f, 220.0f, 235.0f, 250.0f, 256.0f, 300.0f, 350.0f, 400.0f}) {
            PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                         .hostname = "printer",
                                         .printer_objects = {"bed_mesh"},
                                         .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                         .build_volume = BuildVolume{0, bed, 0, bed, 0}};
            auto result = PrinterDetector::detect(hardware);
            INFO("bed " << bed << "mm -> " << result.type_name << " @" << result.confidence);
            REQUIRE(result.confidence < kHeuristicClassCeiling);
        }
    }

    SECTION("a 500mm cartesian rig never reaches a corexy-only identity") {
        // sovol_sv08_max is corexy; on a cartesian machine its kinematics
        // heuristic cannot fire, leaving the 500mm window as its only match.
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .hostname = "printer",
                                     .printer_objects = {"bed_mesh"},
                                     .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                     .kinematics = "cartesian",
                                     .build_volume = BuildVolume{0, 500, 0, 500, 0}};
        auto result = PrinterDetector::detect(hardware);
        CAPTURE(result.type_name, result.confidence);
        REQUIRE(result.type_name != "Sovol SV08 Max");
    }
}

// ============================================================================
// Class evidence vs model evidence
// ============================================================================
//
// A chamber thermistor and an MCU part number describe a CLASS of printer, not a
// model. Scored as identifying they outrank every signal a simpler machine has:
// a Voron 0.2 with a chamber probe and an SKR Pico was reported as a Qidi X-Max 3
// at 78%, because "chamber sensor" (70) and "rp2040" (45) both beat the Voron's
// best evidence, plain CoreXY at 40.

TEST_CASE_METHOD(PrinterDetectorFixture, "PrinterDetector: class evidence alone identifies nothing",
                 "[printer][detector]") {
    SECTION("a chamber sensor on a nameless corexy rig names no printer") {
        // Every enclosed machine has these two. If they can identify, they pick
        // whichever vendor happens to score the chamber sensor highest.
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .sensors = {"chamber_temp"},
                                     .hostname = "printer",
                                     .printer_objects = {"bed_mesh"},
                                     .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                     .kinematics = "corexy",
                                     .build_volume = BuildVolume{0, 250, 0, 250, 0}};
        auto result = PrinterDetector::detect(hardware);
        INFO("got " << result.type_name << " @" << result.confidence);
        // CoreXY (40) may still lead somewhere, but the chamber sensor must not
        // lift anyone to a confident answer.
        REQUIRE(result.confidence < kHeuristicClassCeiling);
    }

    SECTION("an MCU part number on a nameless rig names no printer") {
        PrinterHardwareData hardware{.heaters = {"extruder", "heater_bed"},
                                     .hostname = "printer",
                                     .printer_objects = {"bed_mesh"},
                                     .steppers = {"stepper_x", "stepper_y", "stepper_z"},
                                     .kinematics = "cartesian",
                                     .mcu = "stm32f103",
                                     .build_volume = BuildVolume{0, 235, 0, 235, 0}};
        auto result = PrinterDetector::detect(hardware);
        INFO("got " << result.type_name << " @" << result.confidence);
        REQUIRE(result.confidence < kHeuristicClassCeiling);
    }
}

// ============================================================================
// Saved-vs-detected type mismatch: the decline reason
// ============================================================================
//
// Every non-Warn outcome declines silently in the shipped flow. A debug bundle
// is the only view we get of a reporter's run, so the reason has to be a value
// the caller can log, not an anonymous `false`.

TEST_CASE("PrinterDetector: type mismatch decline reasons are distinguishable", "[detector]") {
    using MD = PrinterDetector::MismatchDecision;

    SECTION("A high-confidence contradiction warns") {
        REQUIRE(PrinterDetector::classify_type_mismatch(
                    "Voron Trident", "FlashForge Adventurer 5M Pro", 90, "",
                    PrinterDetector::kDetectMinMargin) == MD::Warn);
    }

    SECTION("No detected type reports NoDetection, not a low score") {
        REQUIRE(PrinterDetector::classify_type_mismatch("Voron Trident", "", 0, "",
                                                        PrinterDetector::kDetectMinMargin) ==
                MD::NoDetection);
    }

    SECTION("A near-miss reports ConfidenceTooLow and names the gap it missed") {
        REQUIRE(PrinterDetector::classify_type_mismatch("Voron Trident", "Creality K2 Plus",
                                                        PrinterDetector::kMismatchMinConfidence - 1,
                                                        "", PrinterDetector::kDetectMinMargin) ==
                MD::ConfidenceTooLow);
    }

    SECTION("A model that only tied its rivals reports Ambiguous, not a low score") {
        // Scoring well and scoring uniquely are different claims, and a bundle
        // has to say which one failed.
        REQUIRE(PrinterDetector::classify_type_mismatch("Voron Trident", "Qidi Plus 4", 92, "",
                                                        /*detected_margin=*/0) == MD::Ambiguous);
    }

    SECTION("Agreement reports MatchesSavedType") {
        REQUIRE(PrinterDetector::classify_type_mismatch("Voron Trident", "Voron Trident", 95, "",
                                                        PrinterDetector::kDetectMinMargin) ==
                MD::MatchesSavedType);
    }

    SECTION("An unset or deliberately generic saved type reports SavedTypeNotSpecific") {
        REQUIRE(PrinterDetector::classify_type_mismatch("", "Creality K2 Plus", 95, "",
                                                        PrinterDetector::kDetectMinMargin) ==
                MD::SavedTypeNotSpecific);
        REQUIRE(PrinterDetector::classify_type_mismatch("Custom/Other", "Creality K2 Plus", 95, "",
                                                        PrinterDetector::kDetectMinMargin) ==
                MD::SavedTypeNotSpecific);
        REQUIRE(PrinterDetector::classify_type_mismatch("Unknown", "Creality K2 Plus", 95, "",
                                                        PrinterDetector::kDetectMinMargin) ==
                MD::SavedTypeNotSpecific);
    }

    SECTION("A previously answered prompt reports AlreadyDismissed") {
        REQUIRE(PrinterDetector::classify_type_mismatch(
                    "Voron Trident", "Creality K2 Plus", 95, "Voron Trident",
                    PrinterDetector::kDetectMinMargin) == MD::AlreadyDismissed);
    }

    SECTION("Every decision has its own log tag") {
        const MD all[] = {MD::Warn,
                          MD::NoDetection,
                          MD::ConfidenceTooLow,
                          MD::MatchesSavedType,
                          MD::SavedTypeNotSpecific,
                          MD::AlreadyDismissed};
        std::set<std::string> names;
        for (MD d : all) {
            std::string name = PrinterDetector::mismatch_decision_name(d);
            REQUIRE_FALSE(name.empty());
            REQUIRE(name != "unknown");
            names.insert(name);
        }
        // A shared or missing tag would make two different declines read
        // identically in a bundle, which is the whole failure being fixed.
        REQUIRE(names.size() == 6);
    }
}

// ============================================================================
// Reported Voron Trident misdetection (debug bundle TZT85MQ3)
// ============================================================================

// A saved printer type must still reach PrinterState. The type resolves the
// pre-print option set, the z-offset calibration strategy, the purge-line
// capability and the probe-type override, and the only other startup caller of
// set_printer_type_sync() is PrinterImageWidget — so without this, removing that
// tile from the home screen changes the calibration surface.
TEST_CASE_METHOD(helix::VariantPresetFixture,
                 "auto_detect_and_save applies an already-saved type to PrinterState",
                 "[printer_detector][regression]") {
    SetUp();

    get_printer_state().set_printer_type_sync("");
    REQUIRE(get_printer_state().profile_state().printer_type().empty());

    config.set<std::string>(config.df() + helix::wizard::PRINTER_TYPE, "Voron 2.4");

    helix::PrinterDiscovery hw;
    hw.set_printer_objects({"extruder", "heater_bed", "quad_gantry_level"});

    // Returns false: it short-circuited rather than running detection. That is
    // the branch under test, not a failure.
    const bool detected = PrinterDetector::auto_detect_and_save(hw, &config);
    CHECK_FALSE(detected);

    CHECK(get_printer_state().profile_state().printer_type() == "Voron 2.4");

    get_printer_state().set_printer_type_sync("");
    TearDown();
}

// ============================================================================
// Preset installs: nobody is asked, so detection has to decide
// ============================================================================

namespace {

// Elegoo Centauri Carbon on COSMOS firmware, from its bare objects.list. The
// capture file records why this snapshot lands below the auto-save bar.
helix::PrinterDiscovery cc1_discovery() {
    const nlohmann::json j = load_printer_capture("elegoo_centauri_carbon_objects_list");
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(j.at("objects"));
    discovery.set_hostname(j.value("hostname", std::string{}));
    discovery.set_kinematics(j.value("kinematics", std::string{}));
    return discovery;
}

} // namespace

// A platform package seeds its preset into settings.json, and the wizard skips
// every hardware step on such an install, so a printer left without a type
// there keeps the generic image for good. A win too weak to auto-save is still
// the answer when the package already named the machine and nothing will ask.
TEST_CASE_METHOD(helix::VariantPresetFixture,
                 "auto_detect_and_save names the Centauri Carbon from its cc1 package",
                 "[printer_detector][preset]") {
    SetUp();
    get_printer_state().set_printer_type_sync("");

    auto discovery = cc1_discovery();

    auto probe = PrinterDetector::auto_detect(discovery);
    CAPTURE(probe.type_name, probe.confidence, probe.margin());
    REQUIRE_FALSE(PrinterDetector::meets_autosave_threshold(probe));

    config.set_preset("cc1");
    REQUIRE(PrinterDetector::auto_detect_and_save(discovery, &config));
    CHECK(config.get<std::string>(config.df() + helix::wizard::PRINTER_TYPE, "") ==
          "Elegoo Centauri Carbon");
    CHECK(get_printer_state().profile_state().printer_type() == "Elegoo Centauri Carbon");

    get_printer_state().set_printer_type_sync("");
    TearDown();
}

// Without a preset the wizard still asks, so a weak win stays open for the user
// to answer rather than being settled behind their back.
TEST_CASE_METHOD(helix::VariantPresetFixture,
                 "auto_detect_and_save leaves a weak win unset with no preset",
                 "[printer_detector][preset]") {
    SetUp();
    get_printer_state().set_printer_type_sync("");

    auto discovery = cc1_discovery();
    REQUIRE(config.get_preset().empty());

    CHECK_FALSE(PrinterDetector::auto_detect_and_save(discovery, &config));
    CHECK(config.get<std::string>(config.df() + helix::wizard::PRINTER_TYPE, "").empty());

    get_printer_state().set_printer_type_sync("");
    TearDown();
}

// On a preset install nobody is asked, and when the family's entries tie
// between machines that look different there is no honest answer: the
// family's name would itself be one of those machines, picked by database
// order. The type stays unset for the Printer Manager's model row
// (prestonbrown/helixscreen#1606).

// With no evidence at all, a K2 preset install names the family's declared
// default, the base K2, not whichever K2 entry the database lists first
// (prestonbrown/helixscreen#1606).

// A tie between entries picturing the same machine is not a choice between
// machines: the preset install still names it, taking the variant that
// carries the most corroboration.

// The installed family breaks a tie against a machine from outside it: a K2
// preset install whose K2 Plus reading ties only with a Sovol SV06 ACE still
// names the K2 Plus. Only a tie between two of the family's own machines is
// left for the user (prestonbrown/helixscreen#1606).

TEST_CASE("Retired printer presets are not offered", "[printer_detector][catalog]") {
    for (const char* preset : {"k1", "k2", "ad5x", "qidi_q2", "snapmaker_u1", "anycubic_kobra_3"}) {
        CAPTURE(preset);
        CHECK(PrinterDetector::get_name_for_preset(preset).empty());
    }
}
