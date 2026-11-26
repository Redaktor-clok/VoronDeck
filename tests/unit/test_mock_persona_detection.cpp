// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_update_queue.h"

#include "../../src/api/moonraker_client_mock_internal.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "test_helpers/mock_personas.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"

namespace {

using helix::test::PersonaEnv;

/// What the app does under --test: the real discovery sequence over the mock.
helix::PrinterDiscovery discover(const helix::mock::PersonaEntry& p) {
    helix::PrinterDiscovery hw;
    {
        MoonrakerClientMock mock(p.type);
        mock.connect("ws://mock/websocket", [] {}, [] {});
        bool done = false;
        mock.discover_printer([&done] { done = true; });
        REQUIRE(done);
        hw = mock.hardware();
    }
    // Discovery queues UI-thread callbacks; run them before the test ends.
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    return hw;
}

struct Expectation {
    std::string_view id;
    std::string_view type_name; ///< "" = no printer asserted (generic personas)
    std::string_view preset;
    bool autosaves;
};

// clang-format off
constexpr Expectation kExpected[] = {
    {"voron_24",           "Voron 2.4",                 "",             true},
    {"voron_trident",      "Voron Trident",             "",             false},
    {"generic_corexy",     "",                          "",             false},
    {"generic_bedslinger", "",                          "",             false},
    {"multi_extruder",     "",                          "",             false},
    {"delta",              "",                          "",             false},
};
// clang-format on

TEST_CASE("The Voron personas model real centre-origin beds", "[mock][persona]") {
    const auto find_persona = [](std::string_view id) -> const helix::mock::PersonaEntry* {
        for (const auto& p : helix::mock::kPersonas) {
            if (p.id == id) {
                return &p;
            }
        }
        return nullptr;
    };
    const auto* voron = find_persona("voron_24");
    REQUIRE(voron != nullptr);
    const auto& v24 = helix::mock::descriptor(voron->type);
    // A real 2.4 homes to the bed centre: the demo files sliced for
    // centre-origin printers must fit the default persona.
    CHECK(v24.axis_min.x == -175.0);
    CHECK(v24.axis_min.y == -175.0);
    CHECK(v24.axis_max.x == 175.0);
    CHECK(v24.axis_max.y == 175.0);
    const auto* trident = find_persona("voron_trident");
    REQUIRE(trident != nullptr);
    const auto& vt = helix::mock::descriptor(trident->type);
    CHECK(vt.axis_min.x == -125.0);
    CHECK(vt.axis_max.x == 125.0);
    // The generic personas stay corner-origin.
    const auto* corexy = find_persona("generic_corexy");
    REQUIRE(corexy != nullptr);
    const auto& gc = helix::mock::descriptor(corexy->type);
    CHECK(gc.axis_min.x == 0.0);
    CHECK(gc.axis_max.x == 250.0);
    // The steppers the fit check and the volume parser read, through the real
    // discovery path.
    const auto hw = discover(*voron);
    CHECK(hw.build_volume().x_min == -175.0f);
    CHECK(hw.build_volume().x_max == 175.0f);
    CHECK(hw.build_volume().y_min == -175.0f);
    CHECK(hw.build_volume().y_max == 175.0f);
}

} // namespace

TEST_CASE("Every persona has a detection expectation", "[mock][persona][detect]") {
    for (const auto& p : helix::mock::kPersonas) {
        bool listed = false;
        for (const auto& e : kExpected)
            listed = listed || e.id == p.id;
        INFO(p.id);
        CHECK(listed);
    }
    for (const auto& e : kExpected) {
        INFO(e.id);
        CHECK(helix::mock::find_persona(e.id) != nullptr);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "Each mock persona auto-detects as the printer it impersonates",
                 "[mock][persona][detect]") {
    for (const auto& e : kExpected) {
        DYNAMIC_SECTION(e.id) {
            const auto* p = helix::mock::find_persona(e.id);
            REQUIRE(p != nullptr);
            helix::test::PersonaEnv env(e.id);
            const auto r = PrinterDetector::auto_detect(discover(*p));
            INFO(r.type_name << " " << r.confidence << "% margin " << r.margin() << ": "
                             << r.reason);
            CHECK(PrinterDetector::meets_autosave_threshold(r) == e.autosaves);
            if (!e.type_name.empty()) {
                CHECK(r.type_name == e.type_name);
                CHECK(r.preset == e.preset);
            }
            // A persona that declares its type declares the one detection picks.
            if (!p->saved_type.empty() && e.autosaves) {
                CHECK(p->saved_type == r.type_name);
            }
        }
    }
}

TEST_CASE("Retired printer personas are not selectable", "[mock][persona]") {
    for (const auto* id : {"k1", "k1max", "ad5m", "creator5", "creator5_zmod", "snapmaker_u1",
                           "cc1", "ad5x", "k2"}) {
        CAPTURE(id);
        CHECK(helix::mock::find_persona(id) == nullptr);
    }
}
