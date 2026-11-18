// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../catch_amalgamated.hpp"
#include "calibration_receipt.h"
#include "config.h"
#include "ui_wizard_vorondeck.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_input_shaper.h"
#include "ui/ui_widget_helpers.h"

using helix::CalibrationReceipt;
using Tool = WizardChecksStep::Tool;

struct WizardChecksTestAccess {
    static void session(WizardChecksStep& step, lv_obj_t* root, const std::string& printer) {
        step.begin_session(root, printer);
    }
    static int done(WizardChecksStep& step, Tool tool) {
        return lv_subject_get_int(&step.completed_[static_cast<size_t>(tool)]);
    }
    static void receive(WizardChecksStep& step, Tool tool, const CalibrationReceipt::Event& event,
                        const std::string& printer) {
        step.receive(tool, event, printer);
    }
};

class WizardCalibrationFixture : public LVGLUITestFixture {
  protected:
    WizardChecksStep step;
    CalibrationReceipt receipt;
    lv_obj_t* session_root = lv_obj_create(test_screen());
    const std::string printer = helix::Config::get_instance()->df();
    WizardCalibrationFixture() {
        WizardChecksTestAccess::session(step, session_root, printer);
    }
};

TEST_CASE_METHOD(WizardCalibrationFixture, "Calibration receipts reject cancelled superseded and foreign attempts",
                 "[wizard][wizard_calibration_receipts]") {
    for (auto tool : {Tool::Pid, Tool::Mesh, Tool::ZOffset, Tool::Shaper}) {
        step.watch(tool, receipt);
        CHECK(WizardChecksTestAccess::done(step, tool) == 0);
        CHECK_FALSE(step.is_completed(tool));
        const auto cancelled = receipt.begin(printer);
        receipt.cancel();
        CHECK_FALSE(receipt.succeed(cancelled, printer));
        CHECK(WizardChecksTestAccess::done(step, tool) == 0);
        CHECK_FALSE(step.is_completed(tool));
        const auto superseded = receipt.begin(printer);
        const auto active = receipt.begin(printer);
        CHECK_FALSE(receipt.succeed(superseded, printer));
        CHECK_FALSE(receipt.succeed(active, printer + "other"));
        CHECK_FALSE(receipt.succeed(active, printer)); // Returning to the old printer cannot revive it.
        CHECK(WizardChecksTestAccess::done(step, tool) == 0);
        CHECK_FALSE(step.is_completed(tool));
        const auto fresh = receipt.begin(printer);
        REQUIRE(receipt.succeed(fresh, printer));
        CHECK(WizardChecksTestAccess::done(step, tool) == 1);
        CHECK(step.is_completed(tool));
        CHECK_FALSE(receipt.succeed(fresh, printer)); // A receipt is consumed once.
        receipt.begin(printer);
        CHECK(WizardChecksTestAccess::done(step, tool) == 0);
        CHECK_FALSE(step.is_completed(tool)); // Retrying clears the old check.
        receipt.cancel();
        CHECK(WizardChecksTestAccess::done(step, tool) == 0);
        CHECK_FALSE(step.is_completed(tool));
        step.cleanup();
    }
}

TEST_CASE_METHOD(WizardCalibrationFixture, "Opening cached results and skipping checks cannot grant success",
                 "[wizard][wizard_calibration_receipts]") {
    auto cached = receipt.begin(printer);
    REQUIRE(receipt.succeed(cached, printer));
    step.watch(Tool::Pid, receipt);
    CHECK(WizardChecksTestAccess::done(step, Tool::Pid) == 0);
    step.cleanup(); // Skip/back: no state inference from panel visibility.
    WizardChecksTestAccess::session(step, session_root, printer);
    step.watch(Tool::Pid, receipt);
    CHECK(WizardChecksTestAccess::done(step, Tool::Pid) == 0);

    step.cleanup();
    const auto already_running = receipt.begin(printer);
    step.watch(Tool::Pid, receipt);
    REQUIRE(receipt.succeed(already_running, printer));
    CHECK(WizardChecksTestAccess::done(step, Tool::Pid) == 0);
}

TEST_CASE_METHOD(WizardCalibrationFixture, "Wizard receipt scope ends on printer change new wizard and widget deletion",
                 "[wizard][wizard_calibration_receipts]") {
    step.watch(Tool::Mesh, receipt);
    const auto first = receipt.begin(printer);
    REQUIRE(receipt.succeed(first, printer));
    REQUIRE(WizardChecksTestAccess::done(step, Tool::Mesh) == 1);
    step.cleanup();
    WizardChecksTestAccess::session(step, session_root, printer);
    CHECK(WizardChecksTestAccess::done(step, Tool::Mesh) == 1); // Back/Next within this wizard.

    step.watch(Tool::Mesh, receipt);
    const auto old_printer = receipt.begin(printer);
    WizardChecksTestAccess::receive(step, Tool::Mesh,
        {old_printer, CalibrationReceipt::Phase::Succeeded}, printer + "changed");
    CHECK(WizardChecksTestAccess::done(step, Tool::Mesh) == 0);
    WizardChecksTestAccess::session(step, session_root, printer);
    REQUIRE(receipt.succeed(old_printer, printer));
    CHECK(WizardChecksTestAccess::done(step, Tool::Mesh) == 0);

    step.watch(Tool::Mesh, receipt);
    const auto old_wizard = receipt.begin(printer);
    auto* replacement = lv_obj_create(test_screen());
    WizardChecksTestAccess::session(step, replacement, printer);
    REQUIRE(receipt.succeed(old_wizard, printer));
    CHECK(WizardChecksTestAccess::done(step, Tool::Mesh) == 0);
    step.watch(Tool::Mesh, receipt);
    const auto dying = receipt.begin(printer);
    lv_obj_delete(replacement);
    REQUIRE(receipt.succeed(dying, printer));
    CHECK(WizardChecksTestAccess::done(step, Tool::Mesh) == 0);
}

TEST_CASE_METHOD(WizardCalibrationFixture, "Receipt observers tolerate provider teardown before wizard cleanup",
                 "[wizard][wizard_calibration_receipts]") {
    step.watch(Tool::Shaper, receipt);
    const auto old = receipt.begin(printer);
    receipt.deinit();
    step.cleanup();
    CHECK_FALSE(receipt.succeed(old, printer));
    step.watch(Tool::Shaper, receipt);
    const auto fresh = receipt.begin(printer);
    REQUIRE(receipt.succeed(fresh, printer));
    CHECK(WizardChecksTestAccess::done(step, Tool::Shaper) == 1);
}

TEST_CASE_METHOD(WizardCalibrationFixture, "Checks XML binds a checkmark only to confirmed receipts",
                 "[wizard][wizard_calibration_receipts]") {
    const char* attrs[] = {"has_heaters", "1", "has_mesh", "1", "has_probe", "1",
                          "has_shaper", "1", nullptr};
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(session_root, "wizard_checks", attrs));
    REQUIRE(root);
    const char* ticks[] = {"wizard_check_pid_tick", "wizard_check_mesh_tick",
                           "wizard_check_zoffset_tick", "wizard_check_shaper_tick"};
    size_t index = 0;
    for (auto tool : {Tool::Pid, Tool::Mesh, Tool::ZOffset, Tool::Shaper}) {
        auto* tick = helix::ui::find_required(root, ticks[index++], "test");
        REQUIRE(tick);
        CHECK(lv_obj_has_flag(tick, LV_OBJ_FLAG_HIDDEN));
        step.watch(tool, receipt);
        const auto run = receipt.begin(printer);
        CHECK(lv_obj_has_flag(tick, LV_OBJ_FLAG_HIDDEN));
        REQUIRE(receipt.succeed(run, printer));
        CHECK_FALSE(lv_obj_has_flag(tick, LV_OBJ_FLAG_HIDDEN));
        step.cleanup();
    }
    lv_obj_delete(root); // Delete bindings before the local managed subjects.
}

TEST_CASE_METHOD(WizardCalibrationFixture, "Optimistic Z offset and demo PID results do not emit calibration receipts",
                 "[wizard][wizard_calibration_receipts]") {
    SECTION("Optimistic Z offset COMPLETE is only presentation") {
        ZOffsetCalibrationPanel panel;
        panel.init_subjects();
        step.watch(Tool::ZOffset, panel.calibration_receipt());
        panel.on_calibration_result(true, "");
        CHECK(panel.calibration_receipt().event().phase == CalibrationReceipt::Phase::Idle);
        CHECK(WizardChecksTestAccess::done(step, Tool::ZOffset) == 0);
        step.cleanup();
    }
    SECTION("PID screenshot injection is not a completed run") {
        PIDCalibrationPanel panel;
        panel.init_subjects();
        step.watch(Tool::Pid, panel.calibration_receipt());
        panel.inject_demo_results();
        CHECK(panel.calibration_receipt().event().phase == CalibrationReceipt::Phase::Idle);
        CHECK(WizardChecksTestAccess::done(step, Tool::Pid) == 0);
        step.cleanup();
    }
}
