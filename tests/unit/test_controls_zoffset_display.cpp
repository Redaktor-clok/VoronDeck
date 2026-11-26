// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using nlohmann::json;

namespace {

class ControlsZOffsetFixture : public LVGLUITestFixture {
  public:
    ControlsZOffsetFixture() : panel(state(), nullptr) {}

    ~ControlsZOffsetFixture() override {
        if (panel_obj) {
            panel.on_deactivate(DeactivateReason::NavigateAway);
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        helix::ui::UpdateQueue::instance().drain();
        panel.deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    void build_and_activate() {
        panel.init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel.setup(panel_obj, test_screen());
        lv_obj_update_layout(test_screen());
        process_lvgl(20);
        panel.on_activate();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    void set_live_offset(double mm) {
        state().update_from_status(
            json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, mm, 0.0}}}}});
        settle();
    }
    void set_persisted_offset(double mm) {
        state().update_from_status(
            json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", mm}}}}}}}});
        settle();
    }
    void set_printing(bool printing) {
        state().update_from_status(
            json{{"print_stats", json{{"state", printing ? "printing" : "standby"}}}});
        settle();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    std::string row_text() {
        lv_subject_t* subj = lv_xml_get_subject(nullptr, "controls_z_offset");
        if (!subj) {
            return {};
        }
        const char* s = lv_subject_get_string(subj);
        return s ? std::string(s) : std::string();
    }

    ControlsPanel panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ControlsZOffsetFixture,
                 "Controls row shows the stored persistent-offset offset while idle, not 0.000",
                 "[ui_integration][controls][zoffset][persistence][regression]") {
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);

    build_and_activate();

    CHECK(row_text().find("0.150") != std::string::npos);
    CHECK(row_text().find("0.000") == std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture, "Controls row follows the live offset during a print",
                 "[ui_integration][controls][zoffset][persistence]") {
    set_persisted_offset(-0.15);
    set_live_offset(-0.15);
    set_printing(true);
    build_and_activate();

    set_live_offset(-0.16);

    CHECK(row_text().find("0.160") != std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture, "Controls row re-picks its source when the print ends",
                 "[ui_integration][controls][zoffset][persistence][regression]") {
    set_persisted_offset(-0.16);
    set_live_offset(-0.16);
    set_printing(true);
    build_and_activate();
    REQUIRE(row_text().find("0.160") != std::string::npos);

    set_live_offset(0.0);
    set_printing(false);

    CHECK(row_text().find("0.160") != std::string::npos);
    CHECK(row_text().find("0.000") == std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture,
                 "Controls row updates when the stored offset changes underneath it",
                 "[ui_integration][controls][zoffset][persistence]") {
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    build_and_activate();
    REQUIRE(row_text().find("0.150") != std::string::npos);

    set_persisted_offset(-0.075);

    CHECK(row_text().find("0.075") != std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture,
                 "Controls row is unchanged on a non-persistent-offset printer",
                 "[ui_integration][controls][zoffset][regression]") {
    set_live_offset(-0.2);
    set_printing(false);
    build_and_activate();

    CHECK(row_text().find("0.200") != std::string::npos);

    set_live_offset(0.0);
    CHECK(row_text().find("0.000") != std::string::npos);
}
