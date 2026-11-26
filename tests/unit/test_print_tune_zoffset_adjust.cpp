// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_manager.h"
#include "ui_print_tune_overlay.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "tool_state.h"

#include <array>
#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using nlohmann::json;

namespace {

class PrintTuneZOffsetFixture : public LVGLUITestFixture {
  public:
    PrintTuneZOffsetFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        for (auto& p : root_panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(root_panels_.data());

        state().set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        capture_api = std::make_unique<MoonrakerAPI>(mock_client, state());

        state().set_hardware(helix::PrinterDiscovery{});
    }

    ~PrintTuneZOffsetFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        capture_api.reset();
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();
        helix::ToolState::instance().deinit_subjects();
    }

    PrintTuneOverlay& show_overlay() {
        auto& overlay = get_print_tune_overlay();
        overlay.show(lv_screen_active(), capture_api.get(), state());
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        overlay.on_activate(); // seeds the adjustment base from PrinterState
        helix::ui::UpdateQueue::instance().drain();
        return overlay;
    }

    void set_live_offset(double mm) {
        state().update_from_status(
            json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, mm, 0.0}}}}});
    }
    void set_persisted_offset(double mm) {
        state().update_from_status(
            json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", mm}}}}}}}});
    }
    void set_printing(bool printing) {
        state().update_from_status(
            json{{"print_stats", json{{"state", printing ? "printing" : "standby"}}}});
    }
    void set_homed(bool homed) {
        state().update_from_status(json{{"toolhead", json{{"homed_axes", homed ? "xyz" : ""}}}});
    }
    void set_persistence_wrapper() {
        helix::PrinterDiscovery hw;
        json objects = json::array({"gcode_macro SET_GCODE_OFFSET"});
        hw.parse_objects(objects);
        state().set_hardware(std::move(hw));
    }

    const std::string& last_gcode() const {
        return mock_client.last_send_script();
    }

    MoonrakerClientMock mock_client;
    std::unique_ptr<MoonrakerAPI> capture_api;
    std::array<lv_obj_t*, kUiPanelCount> root_panels_{};
};

} // namespace

TEST_CASE_METHOD(
    PrintTuneZOffsetFixture,
    "PrintTune: idle persistent-offset baby-step sends absolute Z= from the stored offset",
    "[ui_integration][zoffset][persistence][regression]") {
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2); // 0.010mm
    overlay.handle_z_adjust(-1);     // one step closer to the bed
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z=-0.160 MOVE=1");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: mid-print persistent-offset baby-step stays relative",
                 "[ui_integration][zoffset][persistence][regression]") {
    set_persisted_offset(-0.15);
    set_live_offset(-0.15);
    set_printing(true);
    set_homed(true);
    set_persistence_wrapper();

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: idle persistent-offset baby-step moves the persisted subject with it",
                 "[ui_integration][zoffset][persistence]") {
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_subject_get_int(state().motion_state().get_persisted_z_offset_subject()) == -160);
    CHECK(lv_subject_get_int(state().motion_state().get_gcode_z_offset_subject()) == -160);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: mid-print baby-step still sends the relative Z_ADJUST form",
                 "[ui_integration][zoffset][persistence]") {
    set_persisted_offset(-0.15);
    set_live_offset(-0.15);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: a non-persistent-offset printer keeps the legacy relative behavior",
                 "[ui_integration][zoffset][regression]") {
    set_live_offset(-0.15);
    set_printing(false);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(1); // farther from the bed
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=0.010 MOVE=1");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture, "PrintTune: MOVE=1 is omitted when the axes are unhomed",
                 "[ui_integration][zoffset]") {
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    set_homed(false);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z=-0.160");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: an offset already past the travel guard still steps normally",
                 "[ui_integration][zoffset][1280][regression]") {
    set_live_offset(2.5);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2); // 0.010mm
    overlay.handle_z_adjust(1);      // farther from the bed
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=0.010 MOVE=1");
    CHECK(lv_subject_get_int(state().motion_state().get_gcode_z_offset_subject()) == 2510);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: stepping toward the bed from a large offset moves one step",
                 "[ui_integration][zoffset][1280][regression]") {
    set_live_offset(2.5);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
    CHECK(lv_subject_get_int(state().motion_state().get_gcode_z_offset_subject()) == 2490);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture, "PrintTune: session travel is still bounded at 2mm",
                 "[ui_integration][zoffset][1280]") {
    set_live_offset(2.5);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(0); // 0.050mm
    for (int i = 0; i < 60; ++i) {
        overlay.handle_z_adjust(-1);
    }
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_subject_get_int(state().motion_state().get_gcode_z_offset_subject()) == 500);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: the displayed offset seeds from the stored value while idle",
                 "[ui_integration][zoffset][persistence][regression]") {
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);

    show_overlay();

    lv_subject_t* subj = lv_xml_get_subject(nullptr, "tune_z_offset_display");
    REQUIRE(subj != nullptr);
    const char* shown = lv_subject_get_string(subj);
    REQUIRE(shown != nullptr);
    CHECK(std::string(shown).find("0.150") != std::string::npos);
    CHECK(std::string(shown).find("0.000") == std::string::npos);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: a status frame moving the active tool's Z updates the tool readout",
                 "[ui_integration][zoffset][tool-offsets]") {
    helix::PrinterDiscovery hw;
    json objects = json::array({"toolchanger", "gcode_move", "tool T0", "extruder"});
    hw.parse_objects(objects);
    helix::ToolState& ts = helix::ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    ts.init_tools(hw);

    auto& overlay = show_overlay();
    overlay.handle_z_target_select(1); // the heading follows the tool's own Z

    ts.update_from_status(json{{"tool T0", json{{"gcode_z_offset", 0.10}}}});
    helix::ui::UpdateQueue::instance().drain();

    lv_subject_t* display = lv_xml_get_subject(nullptr, "tune_z_offset_display");
    REQUIRE(display != nullptr);
    REQUIRE(std::string(lv_subject_get_string(display)) == "0.100 mm");

    ts.update_from_status(json{{"tool T0", json{{"gcode_z_offset", 0.25}}}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(std::string(lv_subject_get_string(display)) == "0.250 mm");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: the pressure advance row shows and sends with beta features off",
                 "[ui_integration][pa_tune]") {
    struct BetaOff {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "show_beta_features");
        bool prev_config = helix::Config::get_instance()->is_beta_features_enabled();
        int prev_subject = subject ? lv_subject_get_int(subject) : 0;
        BetaOff() {
            helix::Config::get_instance()->set<bool>("/beta_features", false);
            if (subject) {
                lv_subject_set_int(subject, 0);
            }
        }
        ~BetaOff() {
            helix::Config::get_instance()->set<bool>("/beta_features", prev_config);
            if (subject) {
                lv_subject_set_int(subject, prev_subject);
            }
        }
    } beta_off;
    REQUIRE(beta_off.subject != nullptr);

    state().update_from_status(json{{"extruder", json{{"pressure_advance", 0.04}}}});
    helix::ui::UpdateQueue::instance().drain();
    auto& overlay = show_overlay();
    lv_obj_t* row = lv_obj_find_by_name(overlay.get_panel(), "tune_pa_row");
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    overlay.handle_pa_adjust(1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(last_gcode() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0410");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: flow buttons step 5 and 1 percent, speed buttons 10 and 5",
                 "[ui_integration][print_tune][speed_flow_steps]") {
    auto& overlay = show_overlay();
    overlay.update_speed_flow_display(100, 100);
    auto tap = [&](const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(overlay.get_panel(), name);
        REQUIRE(btn != nullptr);
        helix::ui::send_tap(btn);
        helix::ui::UpdateQueue::instance().drain();
        return last_gcode();
    };

    CHECK(tap("btn_flow_minus_5") == "M221 S95");
    CHECK(tap("btn_flow_minus_1") == "M221 S94");
    CHECK(tap("btn_flow_plus_1") == "M221 S95");
    CHECK(tap("btn_flow_plus_5") == "M221 S100");

    CHECK(tap("btn_speed_minus_10") == "M220 S90");
    CHECK(tap("btn_speed_minus_5") == "M220 S85");
    CHECK(tap("btn_speed_plus_5") == "M220 S90");
    CHECK(tap("btn_speed_plus_10") == "M220 S100");
}
