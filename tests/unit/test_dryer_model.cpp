// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_dryer_model.cpp
 * @brief The dryer model reads the three drying sources (box dryers, the chamber cycle,
 * bed drying), publishes the `home_dryer_available` gate, and bumps its version when the
 * summary changes.
 *
 * Run with: ./build/bin/helix-tests "[dryer][model]"
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/configfile_mock_client.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_state.h"
#include "bed_drying_controller.h"
#include "moonraker_api.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "src/ui/panel_widgets/dryer_model.h"
#include "src/ui/panel_widgets/dryer_widget.h"
#include "temperature_controller.h"

#include <ctime>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

struct DryerModelFixture : public LVGLUITestFixture {
    DryerModelFixture() {
        reset_sources();
        dryer_model::init_subjects();
        drain();
    }
    ~DryerModelFixture() override {
        reset_sources();
        drain();
    }

    void reset_sources() {
        state().capabilities_state().set_has_chamber_dryer(false);
        state().capabilities_state().set_bed_drying(false, false);
        PanelWidgetManager::instance().unregister_shared_resource<BedDryingController>();
        // A restored bed run latches the spools on the shared printer state, and the
        // latch refuses every later test's gcode until something clears it.
        state().print_state().set_spool_latch(false, {});
    }
    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    static bool gate() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "home_dryer_available");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject) != 0;
    }
    static int version() {
        return lv_subject_get_int(dryer_model::version_subject());
    }
};

} // namespace

// Its next init resets the subject's observer list, which would orphan the node.
TEST_CASE_METHOD(DryerModelFixture, "dryer model: a torn-down filament system is not observed",
                 "[dryer][model]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    lv_subject_t* humidity = ams.get_unit_humidity_subject(0);
    dryer_model::refresh();
    REQUIRE(lv_ll_get_len(&humidity->subs_ll) == 1);

    ams.deinit_subjects();
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&humidity->subs_ll) == 0);

    ams.init_subjects(false);
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&humidity->subs_ll) == 1);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a torn-down printer state is not observed",
                 "[dryer][model]") {
    auto& ps = state();
    lv_subject_t* active = ps.temperature_state().get_chamber_dryer_active_subject();
    dryer_model::refresh();
    REQUIRE(lv_ll_get_len(&active->subs_ll) == 1);

    ps.deinit_subjects();
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&active->subs_ll) == 0);

    ps.init_subjects(true);
    dryer_model::refresh();
    CHECK(lv_ll_get_len(&active->subs_ll) == 1);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: no source, no gate", "[dryer][model]") {
    CHECK_FALSE(gate());
    CHECK_FALSE(dryer_model::summary().available);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a chamber dryer opens the gate",
                 "[dryer][model]") {
    state().capabilities_state().set_has_chamber_dryer(true);
    drain();
    CHECK(gate());
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].kind == DryerSourceKind::Chamber);
    CHECK(dryer_model::summary().drying_count == 0);

    state().capabilities_state().set_has_chamber_dryer(false);
    drain();
    CHECK_FALSE(gate());
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: bed drying opens the gate", "[dryer][model]") {
    state().capabilities_state().set_bed_drying(true, true);
    drain();
    CHECK(gate());
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].kind == DryerSourceKind::Bed);

    // Enclosed but unable to dry on the bed: no source.
    state().capabilities_state().set_bed_drying(true, false);
    drain();
    CHECK_FALSE(gate());
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a running bed cycle reports its progress",
                 "[dryer][model][bed]") {
    constexpr long long kNow = 2'000'000;
    ConfigfileMockClient client(MoonrakerClientMock::PrinterType::VORON_24);
    MoonrakerAPI api(client, state());
    TemperatureController tc(state(), &api);

    bed_drying::RunRecord record;
    record.latched = true;
    record.material = 0;
    record.start_s = kNow - 3600;
    record.end_s = kNow + 3 * 3600;
    record.bed_c = 70;
    (void)SettingsManager::instance().set_bed_drying_record(record);

    BedDryingController ctrl(state(), &api, &tc, [] { return kNow; });
    ctrl.init_subjects();
    PanelWidgetManager::instance().register_shared_resource<BedDryingController>(&ctrl);
    state().capabilities_state().set_bed_drying(true, true);
    ctrl.restore();
    dryer_model::init_subjects(); // attach to the controller that came up
    drain();

    const DryerSummary& s = dryer_model::summary();
    REQUIRE(s.sources.size() == 1);
    CHECK(s.sources[0].kind == DryerSourceKind::Bed);
    CHECK(s.sources[0].drying);
    CHECK(s.sources[0].remaining_min == 180);
    CHECK(s.sources[0].progress_pct == 25);
    CHECK(s.sources[0].target_c == 70);

    PanelWidgetManager::instance().unregister_shared_resource<BedDryingController>();
    SettingsManager::instance().clear_bed_drying_record();
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: the version moves only when the summary does",
                 "[dryer][model]") {
    state().capabilities_state().set_has_chamber_dryer(true);
    drain();
    const int with_chamber = version();
    dryer_model::refresh();
    CHECK(version() == with_chamber);

    state().capabilities_state().set_bed_drying(true, true);
    drain();
    CHECK(version() > with_chamber);
}

TEST_CASE_METHOD(DryerModelFixture,
                 "dryer model: a chamber cycle running at the first reading draws no ring",
                 "[dryer][model][chamber]") {
    auto& temps = state().temperature_state();
    state().capabilities_state().set_has_chamber_dryer(true);
    // Before any frame the default 0 says nothing; the first real frame reports a cycle
    // that was already under way.
    lv_subject_set_int(temps.get_chamber_dryer_active_subject(), 1);
    lv_subject_set_int(temps.get_chamber_dryer_reported_subject(), 1);
    drain();
    const DryerSummary& s = dryer_model::summary();
    REQUIRE(s.sources.size() == 1);
    CHECK(s.sources[0].drying);
    CHECK(s.sources[0].progress_pct == -1);

    lv_subject_set_int(temps.get_chamber_dryer_active_subject(), 0);
    drain();
    CHECK_FALSE(dryer_model::summary().sources[0].drying);
    lv_subject_set_int(temps.get_chamber_dryer_reported_subject(), 0);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: idle rows follow the bed and chamber temperature",
                 "[dryer][model][temps]") {
    auto& temps = state().temperature_state();
    state().capabilities_state().set_bed_drying(true, true);
    lv_subject_set_int(temps.get_bed_temp_subject(), 250);
    drain();
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].current_c == 25);

    lv_subject_set_int(temps.get_bed_temp_subject(), 612);
    drain();
    CHECK(dryer_model::summary().sources[0].current_c == 61);

    // Less than a whole degree is not worth a refresh.
    const int before = version();
    lv_subject_set_int(temps.get_bed_temp_subject(), 614);
    drain();
    CHECK(version() == before);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: a burst of source changes queues one refresh",
                 "[dryer][model][queue]") {
    using Access = helix::ui::UpdateQueueTestAccess;
    auto& queue = helix::ui::UpdateQueue::instance();
    auto& temps = state().temperature_state();

    state().capabilities_state().set_has_chamber_dryer(true);
    const size_t one_change = Access::pending_count(queue);
    REQUIRE(one_change > 0);
    drain();

    state().capabilities_state().set_has_chamber_dryer(false);
    state().capabilities_state().set_bed_drying(true, true);
    lv_subject_set_int(temps.get_bed_temp_subject(), 300);
    CHECK(Access::pending_count(queue) == one_change);
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: attaching to the sources queues no refresh",
                 "[dryer][model][queue]") {
    state().capabilities_state().set_has_chamber_dryer(true);
    drain();

    dryer_model::refresh();

    CHECK(helix::ui::UpdateQueueTestAccess::queue_empty(helix::ui::UpdateQueue::instance()));
}

TEST_CASE_METHOD(DryerModelFixture, "dryer model: the chamber row follows whole degrees",
                 "[dryer][model][temps]") {
    auto& temps = state().temperature_state();
    state().capabilities_state().set_has_chamber_dryer(true);
    lv_subject_set_int(temps.get_chamber_temp_subject(), 250);
    drain();
    REQUIRE(dryer_model::summary().sources.size() == 1);
    CHECK(dryer_model::summary().sources[0].current_c == 25);

    lv_subject_set_int(temps.get_chamber_temp_subject(), 612);
    drain();
    CHECK(dryer_model::summary().sources[0].current_c == 61);

    // Less than a whole degree queues nothing.
    lv_subject_set_int(temps.get_chamber_temp_subject(), 614);
    CHECK(helix::ui::UpdateQueueTestAccess::queue_empty(helix::ui::UpdateQueue::instance()));
}
