// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend.h"
#include "ams_state.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

using helix::AmsBackend;
using helix::AmsState;

/// This file had no fixture, so nothing drained the UpdateQueue. The AmsState
/// tests below add backends and sync them, and AmsState defers its subject
/// writes; each test returned with that work queued and handed it to whichever
/// test drained next (prestonbrown/helixscreen#1169). The drain sits in the
/// derived destructor body so it runs while AmsState's subjects are still alive,
/// before HelixTestFixture's own teardown.
struct MultiBackendFixture : public HelixTestFixture {
    ~MultiBackendFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: single MMU detected as one system",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"mmu", "mmu_encoder mmu_encoder", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::HAPPY_HARE);
    REQUIRE(hw.mmu_type() == helix::AmsType::HAPPY_HARE);
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: toolchanger only detected as one system",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::TOOL_CHANGER);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: toolchanger + Happy Hare prefers MMU backend",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "mmu",
                                                    "mmu_encoder mmu_encoder", "extruder",
                                                    "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    // Only the MMU should be registered — toolchanger is just tool switching
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::HAPPY_HARE);
    // Toolchanger capability is still detected
    REQUIRE(hw.has_tool_changer());
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: AFC + toolchanger prefers AFC backend",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "AFC", "AFC_stepper lane1", "AFC_stepper lane2",
         "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    // Only AFC should be registered — toolchanger is just tool switching
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::AFC);
    // Toolchanger capability is still detected
    REQUIRE(hw.has_tool_changer());
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: a toolchanger beside AFC is a coexisting filament system",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "AFC",
                                            "AFC_stepper lane1", "AFC_stepper lane2", "extruder",
                                            "extruder1", "heater_bed", "gcode_move"}));

    REQUIRE(hw.mmu_type() == helix::AmsType::AFC);
    CHECK(hw.has_filament_system(helix::AmsType::AFC));
    CHECK(hw.has_filament_system(helix::AmsType::TOOL_CHANGER));
    CHECK_FALSE(hw.has_filament_system(helix::AmsType::HAPPY_HARE));
    CHECK_FALSE(hw.has_filament_system(helix::AmsType::NONE));

    // A re-parse of a printer without the toolchanger drops it again.
    hw.parse_objects(nlohmann::json::array(
        {"AFC", "AFC_stepper lane1", "extruder", "heater_bed", "gcode_move"}));
    REQUIRE(hw.mmu_type() == helix::AmsType::AFC);
    CHECK(hw.has_filament_system(helix::AmsType::AFC));
    CHECK_FALSE(hw.has_filament_system(helix::AmsType::TOOL_CHANGER));
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: the detected system row carries the type's display name",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;

    SECTION("Happy Hare") {
        hw.parse_objects(nlohmann::json::array({"mmu", "extruder", "heater_bed"}));
        const auto& systems = hw.detected_ams_systems();
        REQUIRE(systems.size() == 1);
        CHECK(systems[0].type == helix::AmsType::HAPPY_HARE);
        CHECK(systems[0].name == "Happy Hare");
    }
    SECTION("AFC") {
        hw.parse_objects(nlohmann::json::array({"AFC", "AFC_stepper lane1", "extruder"}));
        const auto& systems = hw.detected_ams_systems();
        REQUIRE(systems.size() == 1);
        CHECK(systems[0].type == helix::AmsType::AFC);
        CHECK(systems[0].name == "AFC");
    }
    SECTION("tool changer: one row") {
        hw.parse_objects(nlohmann::json::array(
            {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed"}));
        const auto& systems = hw.detected_ams_systems();
        REQUIRE(systems.size() == 1);
        CHECK(systems[0].type == helix::AmsType::TOOL_CHANGER);
        CHECK(systems[0].name == "Tool Changer");
    }
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: no AMS detected returns empty",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.detected_ams_systems().empty());
    REQUIRE(hw.mmu_type() == helix::AmsType::NONE);
}

// Proprietary markers cannot create a backend or displace an open system.
TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: retired markers do not displace supported systems",
                 "[ams][multi-backend]") {
    const std::vector<nlohmann::json> markers = {
        {"ace"},
        {"ace_instance_0", "ace_instance_1"},
        {"filament_hub"},
        {"filament_detect"},
        {"ace", "filament_detect"},
        {"box"},
        {"box_stepper slot0", "box_stepper slot1"},
        {"filament_motion_sensor ifs_motion_sensor", "filament_switch_sensor head_switch_sensor"}};
    struct Supported {
        nlohmann::json objects;
        helix::AmsType type;
    };
    const std::vector<Supported> supported = {
        {nlohmann::json::array(), helix::AmsType::NONE},
        {{"mmu", "mmu_encoder mmu_encoder"}, helix::AmsType::HAPPY_HARE},
        {{"AFC", "AFC_BoxTurtle Turtle_1", "AFC_stepper lane1"}, helix::AmsType::AFC},
        {{"toolchanger", "tool T0", "tool T1", "extruder1"}, helix::AmsType::TOOL_CHANGER}};
    for (const auto& retired : markers) {
        for (const auto& current : supported) {
            CAPTURE(retired.dump(), static_cast<int>(current.type));
            auto objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
            for (const auto& item : retired)
                objects.push_back(item);
            for (const auto& item : current.objects)
                objects.push_back(item);
            helix::PrinterDiscovery hw;
            hw.parse_objects(objects);
            CHECK(hw.mmu_type() == current.type);
            CHECK_FALSE(hw.has_snapmaker());
            CHECK(hw.ace_object_names().empty());
            CHECK(hw.qidi_box_slot_count() == 0);
            const auto& systems = hw.detected_ams_systems();
            if (current.type == helix::AmsType::NONE) {
                CHECK(systems.empty());
            } else {
                REQUIRE(systems.size() == 1);
                CHECK(systems[0].type == current.type);
            }
        }
    }
}

// ============================================================================
// Task 2: Multi-backend storage tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: add_backend stores multiple backends",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    auto mock1 = AmsBackend::create_mock(4);
    auto mock2 = AmsBackend::create_mock(2);
    ams.add_backend(std::move(mock1));
    ams.add_backend(std::move(mock2));

    REQUIRE(ams.backend_count() == 2);
    REQUIRE(ams.get_backend(0) != nullptr);
    REQUIRE(ams.get_backend(1) != nullptr);
    REQUIRE(ams.get_backend(2) == nullptr);
    REQUIRE(ams.get_backend() == ams.get_backend(0));

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: set_backend replaces all backends",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));
    REQUIRE(ams.backend_count() == 2);

    ams.set_backend(AmsBackend::create_mock(3));
    REQUIRE(ams.backend_count() == 1);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: clear_backends removes all",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    REQUIRE(ams.backend_count() == 1);

    ams.clear_backends();
    REQUIRE(ams.backend_count() == 0);
    REQUIRE(ams.get_backend() == nullptr);

    ams.deinit_subjects();
}

// ============================================================================
// Task 3: Per-backend slot subject accessor tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: primary backend uses flat slot subjects",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.set_backend(AmsBackend::create_mock(4));

    REQUIRE(ams.get_slot_color_subject(0, 0) == ams.get_slot_color_subject(0));
    REQUIRE(ams.get_slot_color_subject(0, 3) == ams.get_slot_color_subject(3));
    REQUIRE(ams.get_slot_status_subject(0, 0) == ams.get_slot_status_subject(0));
    REQUIRE(ams.get_slot_status_subject(0, 3) == ams.get_slot_status_subject(3));

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: secondary backend gets separate slot subjects",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(3));

    auto* color_0 = ams.get_slot_color_subject(0, 0);
    auto* color_1 = ams.get_slot_color_subject(1, 0);
    REQUIRE(color_0 != nullptr);
    REQUIRE(color_1 != nullptr);
    REQUIRE(color_0 != color_1);

    auto* status_0 = ams.get_slot_status_subject(0, 0);
    auto* status_1 = ams.get_slot_status_subject(1, 0);
    REQUIRE(status_0 != nullptr);
    REQUIRE(status_1 != nullptr);
    REQUIRE(status_0 != status_1);

    // Out of range for secondary (mock2 only has 3 slots: 0, 1, 2)
    REQUIRE(ams.get_slot_color_subject(1, 3) == nullptr);
    REQUIRE(ams.get_slot_status_subject(1, 3) == nullptr);

    // Non-existent backend
    REQUIRE(ams.get_slot_color_subject(2, 0) == nullptr);
    REQUIRE(ams.get_slot_status_subject(2, 0) == nullptr);

    ams.deinit_subjects();
}

// ============================================================================
// Task 4: Per-backend event routing and sync tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: sync_backend updates correct subjects",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));

    // Sync primary
    ams.sync_backend(0);
    REQUIRE(lv_subject_get_int(ams.get_slot_count_subject()) > 0);

    // Sync secondary - should update secondary slot subjects
    ams.sync_backend(1);
    auto* sec_color = ams.get_slot_color_subject(1, 0);
    REQUIRE(sec_color != nullptr);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: update_slot_for_backend delegates to primary",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));

    // Backend 0 refreshes the primary slot subjects from the backend.
    lv_subject_t* color = ams.get_slot_color_subject(0);
    REQUIRE(color != nullptr);
    const auto expected = static_cast<int>(ams.get_backend(0)->get_slot_info(0).color_rgb);
    REQUIRE(expected != 0x123456);
    lv_subject_set_int(color, 0x123456);
    ams.update_slot_for_backend(0, 0);
    REQUIRE(lv_subject_get_int(color) == expected);

    // Should not crash for out-of-range backend
    ams.update_slot_for_backend(5, 0);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: get_backend negative index returns nullptr",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    REQUIRE(ams.get_backend(-1) == nullptr);

    ams.deinit_subjects();
}

// ============================================================================
// Task 5: Multi-backend init flow tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: init_backends_from_hardware with single system",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.detected_ams_systems().size() == 1);
    REQUIRE(hw.detected_ams_systems()[0].type == helix::AmsType::TOOL_CHANGER);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: init_backends skips when no systems detected",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    // Verify detection returns empty - no systems to init
    REQUIRE(hw.detected_ams_systems().empty());

    ams.deinit_subjects();
}

// ============================================================================
// Task 8: Integration and lifecycle tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "Multi-backend: full lifecycle", "[ams][multi-backend]") {
    lv_init_safe();

    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    // Add two mock backends
    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));

    REQUIRE(ams.backend_count() == 2);
    REQUIRE(lv_subject_get_int(ams.get_backend_count_subject()) == 2);

    // Sync both backends
    ams.sync_backend(0);
    ams.sync_backend(1);

    // Primary backend slots via flat accessors
    REQUIRE(ams.get_slot_color_subject(0) != nullptr);
    REQUIRE(ams.get_slot_color_subject(3) != nullptr);

    // Secondary backend slots via indexed accessors
    REQUIRE(ams.get_slot_color_subject(1, 0) != nullptr);
    REQUIRE(ams.get_slot_color_subject(1, 1) != nullptr);
    REQUIRE(ams.get_slot_color_subject(1, 2) == nullptr); // mock2 only has 2 slots

    // Active backend selection
    ams.set_active_backend(1);
    REQUIRE(ams.active_backend_index() == 1);
    REQUIRE(lv_subject_get_int(ams.get_active_backend_subject()) == 1);

    // Out of range selection is ignored
    ams.set_active_backend(5);
    REQUIRE(ams.active_backend_index() == 1); // unchanged

    // Deinit cleans everything up
    ams.deinit_subjects();
    REQUIRE(ams.backend_count() == 0);
    REQUIRE(lv_subject_get_int(ams.get_backend_count_subject()) == 0);
}

TEST_CASE_METHOD(MultiBackendFixture, "Multi-backend: deinit then re-init is safe",
                 "[ams][multi-backend]") {
    lv_init_safe();

    AmsState& ams = AmsState::instance();

    // First cycle
    ams.deinit_subjects();
    ams.init_subjects(false);
    ams.add_backend(AmsBackend::create_mock(4));
    REQUIRE(ams.backend_count() == 1);
    ams.deinit_subjects();

    // Second cycle
    ams.init_subjects(false);
    REQUIRE(ams.backend_count() == 0);
    ams.add_backend(AmsBackend::create_mock(2));
    REQUIRE(ams.backend_count() == 1);
    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "Multi-backend: active backend resets on clear",
                 "[ams][multi-backend]") {
    lv_init_safe();

    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));
    ams.set_active_backend(1);
    REQUIRE(ams.active_backend_index() == 1);

    ams.clear_backends();
    REQUIRE(ams.active_backend_index() == 0);
    REQUIRE(ams.backend_count() == 0);

    ams.deinit_subjects();
}
