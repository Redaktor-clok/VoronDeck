// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pa_calibration.cpp
 * @brief helix::pacal provider table + PACalibrateCollector
 *
 * Two things are worth pinning here.
 *
 * The capability gate: stock Klipper cannot measure pressure advance, so a
 * printer that does not advertise a measuring firmware's command must report
 * the capability ABSENT rather than offer a screen that can only fail. That is
 * the difference between a hidden button and a refusal the user cannot act on.
 *
 * The collector contract: the console result line - not the RPC reply - is the
 * authority for completion, exactly as for PID_CALIBRATE. A run that outlives
 * its RPC timeout is still running.
 */

#include "../../include/moonraker_advanced_api.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/pa_calibration.h"
#include "../../include/printer_discovery.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <regex>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A discovery populated the way Moonraker's object list would populate it.
PrinterDiscovery with_objects(const std::vector<std::string>& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    hw.set_printer_objects(objects);
    return hw;
}

/// A Snapmaker U1 as its object list reports it: the flow calibrator has no
/// status object, filament_parameters (which it depends on) does.

} // namespace

// ============================================================================
// Capability gate
// ============================================================================

TEST_CASE("PA calibration is unsupported on a printer with no measuring firmware",
          "[pa_calibration]") {
    // A perfectly ordinary Klipper machine: heaters, a probe, even the tool
    // offset macro. None of that can measure pressure advance.
    const PrinterDiscovery hw = with_objects(
        {"extruder", "heater_bed", "probe", "toolchanger", "gcode_macro CALIBRATE_TOOL_OFFSETS"});

    REQUIRE_FALSE(pacal::is_supported(hw));
    REQUIRE(pacal::provider_name(hw).empty());
    REQUIRE_FALSE(pacal::procedure_for(hw, 0, 245).has_value());
}

TEST_CASE("Retired firmware objects do not offer automatic PA calibration", "[pa_calibration]") {
    for (const char* object : {"filament_parameters", "ff_pa"}) {
        const auto hw = with_objects({"extruder", object});
        REQUIRE_FALSE(pacal::is_supported(hw));
        REQUIRE_FALSE(pacal::procedure_for(hw, 0, 245));
    }
}

TEST_CASE("PA plausibility band brackets a healthy direct-drive value", "[pa_calibration]") {
    const PrinterDiscovery hw = with_objects({"extruder"});

    const auto range = pacal::sane_range(hw);
    REQUIRE(range.low < range.high);
    REQUIRE(range.extruder_kind != nullptr);

    // A typical direct-drive result sits inside; a Bowden-scale number does
    // not. Flagging that is the one judgement the machine cannot make itself.
    REQUIRE(pacal::is_plausible(hw, 0.0412f));
    REQUIRE_FALSE(pacal::is_plausible(hw, 0.6f));
    REQUIRE_FALSE(pacal::is_plausible(hw, 0.0f));
}

// ============================================================================
// Collector
// ============================================================================

class PACalibrateTestFixture : public LVGLTestFixture {
  public:
    PACalibrateTestFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        // execute_gcode()'s halted gate would otherwise reject every command.
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);

        proc_.provider = "Test measuring macro";
        proc_.start_gcode = "TEST_PA_MEASURE";
        proc_.command_word = "TEST_PA_MEASURE";
        proc_.result_pattern = R"(Got pressure advance:\s*([0-9]*\.?[0-9]+))";
        proc_.attempt_pattern = R"(measure k:\s*([0-9]*\.?[0-9]+))";
        proc_.failure_pattern = "flow k is out of range|abort calibration:";
        proc_.expected_attempts = 5;
        proc_.applies_result = true;
        proc_.timeout_ms = 600000;
    }
    ~PACalibrateTestFixture() {
        api_.reset();
    }

    void start() {
        cancel_ = api_->advanced().start_pa_calibrate(
            proc_,
            [this](float k) {
                captured_k_ = k;
                result_received_.store(true);
            },
            [this](const MoonrakerError& err) {
                captured_error_ = err.message;
                error_received_.store(true);
            },
            [this](int attempt, int expected, float k_so_far) {
                attempts_.push_back(attempt);
                expected_ = expected;
                last_k_so_far_ = k_so_far;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void say(const std::string& line) {
        mock_client_.dispatch_gcode_response(line);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
    PrinterDiscovery hw_;
    pacal::Procedure proc_;
    std::function<void()> cancel_;

    std::atomic<bool> result_received_{false};
    std::atomic<bool> error_received_{false};
    float captured_k_ = 0.0f;
    std::string captured_error_;
    std::vector<int> attempts_;
    int expected_ = 0;
    float last_k_so_far_ = 0.0f;
};

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector reads the measured value off the console",
                 "[pa_collector]") {
    start();
    say("// Got pressure advance: 0.0412");

    REQUIRE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_k_ == Catch::Approx(0.0412f).margin(0.00001f));
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector counts candidate probes as progress",
                 "[pa_collector]") {
    start();
    say("// measure k: 0.02000");
    say("// measure area: 0.01810");
    say("// measure k: 0.06000");
    say("// measure k: 0.04000");

    REQUIRE(attempts_.size() == 3);
    REQUIRE(attempts_.back() == 3);
    REQUIRE(last_k_so_far_ == Catch::Approx(0.04f));
    REQUIRE(expected_ > 0);
    // Progress is not completion: the run is still open until the value lands.
    REQUIRE_FALSE(result_received_.load());

    say("// Got pressure advance: 0.0412");
    REQUIRE(result_received_.load());
}

TEST_CASE_METHOD(PACalibrateTestFixture,
                 "PA collector reports a missing command as a capability problem",
                 "[pa_collector]") {
    start();
    say("!! Unknown command:\"TEST_PA_MEASURE\"");

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    // The message has to say the printer cannot do this, not that the run went
    // wrong: they call for completely different things from the user.
    REQUIRE(captured_error_.find("TEST_PA_MEASURE") != std::string::npos);
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector ends on the firmware's own failure line",
                 "[pa_collector]") {
    start();
    say("// flow k is out of range, use default value:0.02");

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    REQUIRE(captured_error_.find("out of range") != std::string::npos);
}

TEST_CASE_METHOD(PACalibrateTestFixture,
                 "PA collector does not end on a line that merely says error", "[pa_collector]") {
    start();
    // Only a line Klipper marks as an error ends the run.
    say("// measure area: fitting error 0.00120");

    REQUIRE_FALSE(error_received_.load());
    say("// Got pressure advance: 0.0412");
    REQUIRE(result_received_.load());
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector is silent after it is cancelled",
                 "[pa_collector]") {
    start();
    REQUIRE(cancel_);
    cancel_();

    // The firmware finishes the run it is in; its result is no longer news.
    say("// Got pressure advance: 0.0412");
    say("!! late failure");

    REQUIRE_FALSE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector surfaces a firmware refusal verbatim",
                 "[pa_collector]") {
    start();
    const std::string refusal =
        "!! Extruder reported no filament at the sensor after 40 mm of priming. The calibration"
        " was cancelled before any extrusion and the nozzle is cooling down.";
    say(refusal);

    REQUIRE(error_received_.load());
    REQUIRE_FALSE(result_received_.load());
    // The machine's own sentence is the most valuable text on the screen when
    // it appears, so nothing may trim it on the way through.
    REQUIRE(captured_error_.find("no filament at the sensor") != std::string::npos);
    REQUIRE(captured_error_.find("nozzle is cooling down") != std::string::npos);
}

TEST_CASE_METHOD(PACalibrateTestFixture, "PA collector completes exactly once", "[pa_collector]") {
    start();
    say("// Got pressure advance: 0.0412");
    REQUIRE(result_received_.load());

    // A second result line, or a late error, must not re-fire the callback and
    // overwrite a finished run.
    result_received_.store(false);
    say("// Got pressure advance: 0.0999");
    say("!! Something went wrong afterwards");

    REQUIRE_FALSE(result_received_.load());
    REQUIRE_FALSE(error_received_.load());
    REQUIRE(captured_k_ == Catch::Approx(0.0412f).margin(0.00001f));
}
