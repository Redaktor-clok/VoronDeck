// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/scoped_env.h"
#include "moonraker_client_mock.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {
using json = nlohmann::json;

struct TuneFactorsFixture : HelixTestFixture {
    helix::ScopedEnv ams{"HELIX_MOCK_AMS", "none"};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<json> reports;
    MoonrakerClientMock mock{MoonrakerClientMock::PrinterType::VORON_24};

    TuneFactorsFixture() {
        mock.register_notify_update([this](const json& frame) {
            const auto& status = frame.at("params").at(0);
            if (status.contains("gcode_move")) {
                std::lock_guard<std::mutex> lock(mutex);
                reports.push_back(status.at("gcode_move"));
                changed.notify_all();
            }
        });
    }
    ~TuneFactorsFixture() override {
        mock.stop_temperature_simulation();
    }

    void factors(double speed, double flow) {
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE_FALSE(reports.empty());
        CHECK(reports.back().at("speed_factor").get<double>() == Catch::Approx(speed));
        CHECK(reports.back().at("extrude_factor").get<double>() == Catch::Approx(flow));
    }
};
} // namespace

TEST_CASE_METHOD(TuneFactorsFixture, "Mock M220 and M221 report independent tuning factors",
                 "[mock][tune][gcode]") {
    REQUIRE(mock.gcode_script("M220 S125") == 0);
    factors(1.25, 1.0);
    REQUIRE(mock.gcode_script("M221 S93") == 0);
    factors(1.25, .93);
    REQUIRE(mock.gcode_script("m220 S90") == 0);
    factors(.90, .93);
    REQUIRE(mock.gcode_script("M221 S100") == 0);
    factors(.90, 1.0);
    REQUIRE(mock.gcode_script("M220 S100") == 0);
    factors(1.0, 1.0);
}

TEST_CASE_METHOD(TuneFactorsFixture, "Mock invalid or unrelated factor commands retain tuning",
                 "[mock][tune][gcode]") {
    REQUIRE(mock.gcode_script("M220 S125") == 0);
    REQUIRE(mock.gcode_script("M221 S93") == 0);
    for (const char* command : {"M220 S0", "M221 S-1", "M220 Snan", "M221 Sinf",
                                "M220 Sbad"}) {
        INFO(command);
        CHECK(mock.gcode_script(command) != 0);
        CHECK_FALSE(mock.get_last_gcode_error().empty());
        // Force a fresh status report, not merely the last successful command's frame.
        REQUIRE(mock.gcode_script("SET_GCODE_OFFSET Z=0") == 0);
        factors(1.25, .93);
    }
    REQUIRE(mock.gcode_script("M2200 S50") == 0);
    REQUIRE(mock.gcode_script("M2210 S50") == 0);
    REQUIRE(mock.gcode_script("SET_GCODE_OFFSET Z=0") == 0);
    factors(1.25, .93);
}

TEST_CASE_METHOD(TuneFactorsFixture, "Mock simulation ticks retain requested speed and flow",
                 "[mock][tune][simulation]") {
    SECTION("idle") {}
    SECTION("printing") {
        // Public native transitions reach PRINTING without a private phase override.
        REQUIRE(mock.start_print_internal("3DBenchy.gcode"));
        REQUIRE(mock.pause_print_internal());
        REQUIRE(mock.resume_print_internal());
        REQUIRE(mock.get_print_phase() == MoonrakerClientMock::MockPrintPhase::PRINTING);
    }
    REQUIRE(mock.gcode_script("M220 S125") == 0);
    REQUIRE(mock.gcode_script("M221 S93") == 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        reports.clear();
    }
    mock.start_temperature_simulation();
    bool received = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        received = changed.wait_for(lock, std::chrono::seconds(5),
                                    [this] { return reports.size() >= 3; });
    }
    mock.stop_temperature_simulation();
    REQUIRE(received);
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& report : reports) {
        CHECK(report.at("speed_factor").get<double>() == Catch::Approx(1.25));
        CHECK(report.at("extrude_factor").get<double>() == Catch::Approx(.93));
    }
}
