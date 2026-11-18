// SPDX-License-Identifier: GPL-3.0-or-later
#include "pid_heater_request.h"
#include "ui_component_keypad.h"
#include "ui_panel_calibration_pid.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../catch_amalgamated.hpp"
#include <array>
#include <algorithm>

using nlohmann::json;

TEST_CASE("PID quick entry requires explicit native PID control and limits", "[pid-keypad]") {
    json config = {{"extruder", {{"control", "pid"}, {"min_temp", "0"}, {"max_temp", "300"}}},
                   {"heater_generic dryer", {{"control", "pid"}, {"min_temp", 5}, {"max_temp", 90}}}};
    const auto nozzle = helix::pid_heater_request(config, "extruder");
    REQUIRE(nozzle);
    CHECK(nozzle->accepts(222));
    CHECK_FALSE(nozzle->accepts(0));
    CHECK_FALSE(nozzle->accepts(301));
    CHECK_FALSE(nozzle->accepts(std::nan("")));
    config["extruder"]["max_temp"] = 450;
    REQUIRE(helix::pid_heater_request(config, "extruder"));
    CHECK(helix::pid_heater_request(config, "extruder")->accepts(350));
    CHECK(helix::pid_heater_request(config, "extruder")->accepts(450));
    config["extruder"]["max_temp"] = 250;
    CHECK_FALSE(helix::pid_heater_request(config, "extruder")->accepts(251));
    const auto dryer = helix::pid_heater_request(config, "heater_generic dryer");
    REQUIRE(dryer);
    CHECK(dryer->object == "heater_generic dryer");
    CHECK(dryer->command_name == "dryer");
    CHECK(dryer->accepts(5));
    CHECK(dryer->accepts(90));
    CHECK_FALSE(dryer->accepts(4));
    for (const auto control : {"mpc", "watermark", ""}) {
        config["heater_generic dryer"]["control"] = control;
        CHECK_FALSE(helix::pid_heater_request(config, "heater_generic dryer"));
    }
    config["extruder"].erase("control");
    CHECK_FALSE(helix::pid_heater_request(config, "extruder"));
    config["temperature_fan fan"] = {{"control", "pid"}, {"min_temp", 0}, {"max_temp", 100}};
    CHECK_FALSE(helix::pid_heater_request(config, "temperature_fan fan"));
    config["heater_bed"] = {{"control", "pid"}, {"min_temp", 0}, {"max_temp", "100junk"}};
    CHECK_FALSE(helix::pid_heater_request(config, "heater_bed"));
    CHECK_FALSE(helix::pid_heater_request(config, "missing"));
}

namespace {
class PidKeypadFixture : public LVGLUITestFixture {
  public:
    MoonrakerClientMock mock{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI mock_api{mock, state()};
    PIDCalibrationPanel panel;
    helix::IMoonrakerAPI* previous_api = get_moonraker_api();

    PidKeypadFixture() {
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
        panel.init_subjects();
        panel.set_api(&mock_api);
        state().set_hardware(helix::test::discovery_of(
            {"extruder", "heater_bed", "heater_generic dryer"}));
        state().network_state().set_printer_connection_state_internal(2, "Ready");
        state().set_klippy_state_sync(helix::KlippyState::READY);
        lv_subject_set_int(state().print_state().get_print_active_subject(), 0);
        set_moonraker_api(&mock_api);
    }
    ~PidKeypadFixture() override {
        NavigationManager::instance().shutdown();
        panel.cleanup();
        set_moonraker_api(previous_api);
        helix::ui::UpdateQueue::instance().drain();
    }
    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(200);
    }
    lv_obj_t* find(const char* name) {
        auto* found = lv_obj_find_by_name(lv_screen_active(), name);
        REQUIRE(found);
        return found;
    }
    void tap(const char* name) {
        lv_obj_send_event(find(name), LV_EVENT_CLICKED, nullptr);
        settle();
    }
    bool sent(const std::string& command) {
        const auto& history = mock.gcode_script_history();
        return std::any_of(history.begin(), history.end(), [&](const auto& line) {
            return line.find(command) != std::string::npos;
        });
    }
};
}

TEST_CASE_METHOD(PidKeypadFixture, "Keypad secondary action uses entered value without Set or Cancel",
                 "[pid-keypad][keypad]") {
    int sets = 0, cancels = 0;
    float action_value = -1;
    ui_keypad_config_t config{};
    config.max_value = 300;
    config.title_label = "extruder";
    config.on_confirm = [&](float) { ++sets; };
    config.on_cancel = [&] { ++cancels; };
    REQUIRE(ui_keypad_show(&config));
    settle(); // Native navigation pushes overlays on the next UI tick.
    const auto generation = ui_keypad_generation();
    REQUIRE(ui_keypad_set_secondary_action(generation, "PID Calibration",
                                          [&](float value) { action_value = value; }));
    tap("btn_2");tap("btn_2");tap("btn_2");tap("keypad_secondary_action");
    CHECK(action_value == 222);
    CHECK(sets == 0);
    CHECK(cancels == 0);
    REQUIRE(ui_keypad_show(&config));
    settle();
    CHECK_FALSE(ui_keypad_set_secondary_action(generation, "Stale", [](float) {}));
    CHECK(lv_obj_has_flag(find("keypad_secondary_action"), LV_OBJ_FLAG_HIDDEN));
    ui_keypad_hide();settle();
    CHECK(cancels == 1);
    CHECK_FALSE(ui_keypad_set_secondary_action(ui_keypad_generation(), "Closed", [](float) {}));
}

TEST_CASE_METHOD(PidKeypadFixture, "PID quick run sends exact generic heater and never auto saves",
                 "[pid-keypad]") {
    const helix::PidHeaterRequest request{"heater_generic dryer", "dryer", 5, 90};
    REQUIRE(panel.start_keypad_calibration(request, 55, lv_screen_active()));
    settle(); // Activate the queued overlay before delivering calibration completion.
    CHECK(sent("PID_CALIBRATE HEATER=dryer TARGET=55"));
    CHECK_FALSE(sent("HEATER=heater_bed"));
    panel.on_calibration_result(true, 10, 2, 40);
    CHECK(panel.get_state() == PIDCalibrationPanel::State::COMPLETE);
    CHECK_FALSE(sent("SAVE_CONFIG"));
    auto* unsaved = lv_xml_get_subject(nullptr, "pid_quick_unsaved");
    REQUIRE(unsaved);
    CHECK(lv_subject_get_int(unsaved) == 1);
    panel.handle_quick_save();
    CHECK_FALSE(sent("SAVE_CONFIG")); // Merely opening confirmation never saves.
    tap("btn_secondary");
    CHECK_FALSE(sent("SAVE_CONFIG"));
    CHECK(lv_subject_get_int(unsaved) == 1);
}

TEST_CASE_METHOD(PidKeypadFixture, "PID capability offer survives the keypad's queued navigation push",
                 "[pid-keypad][keypad]") {
    ui_keypad_config_t config{};
    config.max_value = 300;
    config.title_label = "extruder";
    REQUIRE(ui_keypad_show(&config));
    // This is the production open_target ordering: no tick between show/offer.
    ui_keypad_offer_pid("extruder", lv_screen_active());
    settle();
    auto* has_action = lv_xml_get_subject(nullptr, "keypad_has_secondary");
    REQUIRE(has_action);
    CHECK(lv_subject_get_int(has_action) == 1);
    CHECK_FALSE(lv_obj_has_flag(find("keypad_secondary_action"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(sent("PID_CALIBRATE"));
    ui_keypad_hide();
    settle();
}

TEST_CASE_METHOD(PidKeypadFixture, "PID quick run rejects unsafe printer state and zero target",
                 "[pid-keypad]") {
    const helix::PidHeaterRequest request{"extruder", "extruder", 0, 300};
    SECTION("zero") { CHECK_FALSE(panel.start_keypad_calibration(request, 0, lv_screen_active())); }
    SECTION("above native maximum") { CHECK_FALSE(panel.start_keypad_calibration(request, 301, lv_screen_active())); }
    SECTION("printing or paused") {
        lv_subject_set_int(state().print_state().get_print_active_subject(), 1);
        CHECK_FALSE(panel.start_keypad_calibration(request, 222, lv_screen_active()));
    }
    SECTION("disconnected") {
        state().network_state().set_printer_connection_state_internal(0, "Offline");
        CHECK_FALSE(panel.start_keypad_calibration(request, 222, lv_screen_active()));
    }
    SECTION("shutdown") {
        state().set_klippy_state_sync(helix::KlippyState::SHUTDOWN);
        CHECK_FALSE(panel.start_keypad_calibration(request, 222, lv_screen_active()));
    }
    CHECK_FALSE(sent("PID_CALIBRATE"));
    CHECK_FALSE(sent("SAVE_CONFIG"));
}

TEST_CASE_METHOD(PidKeypadFixture, "PID Save confirmation belongs to its original run and connection",
                 "[pid-keypad]") {
    const helix::PidHeaterRequest request{"heater_generic dryer", "dryer", 5, 90};
    REQUIRE(panel.start_keypad_calibration(request, 55, lv_screen_active()));
    settle();
    panel.on_calibration_result(true, 10, 2, 40);
    panel.handle_quick_save();
    REQUIRE_FALSE(sent("SAVE_CONFIG"));
    SECTION("reset request before confirmation") {
        panel.on_activate(); // Same native instance, fresh context; old request is now absent.
        tap("btn_primary");
        CHECK_FALSE(sent("SAVE_CONFIG"));
    }
    SECTION("connection epoch changed before confirmation") {
        state().network_state().reset_klippy_state_freshness();
        tap("btn_primary");
        CHECK_FALSE(sent("SAVE_CONFIG"));
    }
    SECTION("unchanged confirmed run may explicitly save") {
        tap("btn_primary");
        CHECK(sent("SAVE_CONFIG"));
    }
}
