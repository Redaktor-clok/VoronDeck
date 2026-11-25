// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_plr_state.cpp
 * @brief Unit tests for PrinterState::pl_env_valid / pl_recovery_file
 *
 * Snapmaker-fork Power-Loss-Recovery fields carried on virtual_sdcard.
 * Absent (harmless) on mainline Klipper; Moonraker sends explicit null for
 * subscribed fields the connected firmware doesn't populate, so parsing must
 * type-check every field rather than assume presence implies a usable value.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "printer_print_state.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

class PlrStateTestFixture : public LVGLTestFixture {
  public:
    PlrStateTestFixture() {
        PrinterStateTestAccess::reset(state_);
        state_.init_subjects(false);
    }

    ~PlrStateTestFixture() {
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

    bool pl_env_valid() {
        return state_.print_state().is_plr_snapshot_valid();
    }

    const std::string& pl_recovery_file() {
        return state_.print_state().pl_recovery_file();
    }

    bool plr_power_loss_signal() {
        return state_.print_state().is_plr_power_loss_signal();
    }

    bool qidi_plr_capable() {
        return state_.print_state().is_plr_resume_macro_present();
    }

    bool qidi_was_interrupted() {
        return state_.print_state().is_plr_interrupted_flag();
    }

  private:
    PrinterState state_;
};

} // namespace

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: default is invalid with no recovery file",
                 "[plr][state]") {
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

TEST_CASE_METHOD(PlrStateTestFixture,
                 "PLR state: retired snapshot flag is ignored, generic file_path parses",
                 "[plr][state]") {
    json status = {{"virtual_sdcard",
                    {{"progress", 0.0},
                     {"layer", nullptr},
                     {"layer_count", nullptr},
                     {"is_active", false},
                     {"pl_env_valid", true},
                     {"file_path", "benchy.gcode"}}}};
    state().update_from_status(status);
    REQUIRE_FALSE(pl_env_valid());
    REQUIRE(pl_recovery_file() == "benchy.gcode");
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: explicit null pl_env_valid leaves default",
                 "[plr][state]") {
    // Mainline Klipper (no power_loss_check module) subscribes the field but
    // Moonraker sends an explicit null since virtual_sdcard never sets it.
    json status = {{"virtual_sdcard",
                    {{"progress", 0.0},
                     {"is_active", false},
                     {"pl_env_valid", nullptr},
                     {"file_path", nullptr}}}};
    state().update_from_status(status);
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: missing fields entirely leaves default",
                 "[plr][state]") {
    json status = {{"virtual_sdcard", {{"progress", 0.0}, {"is_active", false}}}};
    state().update_from_status(status);
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

TEST_CASE_METHOD(PlrStateTestFixture, "PLR state: file_path change is applied (changed-guard)",
                 "[plr][state]") {
    // update_from_status only assigns pl_recovery_file_ when the incoming
    // file_path differs (avoids a per-status temporary). A genuine change must
    // still flow through.
    json first = {{"virtual_sdcard",
                   {{"progress", 0.0},
                    {"is_active", false},
                    {"pl_env_valid", true},
                    {"file_path", "first.gcode"}}}};
    state().update_from_status(first);
    REQUIRE(pl_recovery_file() == "first.gcode");

    json second = {{"virtual_sdcard",
                    {{"progress", 0.0},
                     {"is_active", false},
                     {"pl_env_valid", true},
                     {"file_path", "second.gcode"}}}};
    state().update_from_status(second);
    REQUIRE(pl_recovery_file() == "second.gcode");
}

TEST_CASE_METHOD(PlrStateTestFixture,
                 "PLR state: disconnect-edge reset clears pl_env_valid + recovery file",
                 "[plr][state]") {
    json valid = {{"virtual_sdcard",
                   {{"progress", 0.0},
                    {"is_active", false},
                    {"pl_env_valid", true},
                    {"file_path", "interrupted.gcode"}}}};
    state().update_from_status(valid);
    REQUIRE_FALSE(pl_env_valid());
    REQUIRE(pl_recovery_file() == "interrupted.gcode");

    // The offer controller performs exactly this on a CONNECTED->not-CONNECTED
    // edge so a reconnect re-derives a genuine 0->1 pl_env_valid edge from the
    // fresh status: force the subject to 0 and drop the stale recovery file.
    lv_subject_set_int(state().print_state().get_plr_snapshot_valid_subject(), 0);
    state().print_state().clear_pl_recovery_file();
    REQUIRE(pl_env_valid() == false);
    REQUIRE(pl_recovery_file().empty());
}

// ===========================================================================
// Creality backend capability — print_stats.power_loss
//
// The key exists ONLY in Creality's Klipper fork; mainline has no such field.
// PRESENCE (as a JSON number) is the capability marker, not the value — it
// normally reads 0 and only becomes 1 after the side-effectful detect probe.
// ===========================================================================

TEST_CASE_METHOD(PlrStateTestFixture, "PLR signal: default is unset", "[plr][state][creality]") {
    REQUIRE(plr_power_loss_signal() == false);
}

// ===========================================================================
// Qidi backend - save_variables.variables.was_interrupted
//
// The stock macros maintain it: PRINT_START's save_last_file sets it True, a
// normal end or cancel (CLEAR_LAST_FILE) sets it False, so it reads true at
// boot after power loss. Moonraker sends deltas at top-level-field granularity
// (the whole `variables` dict arrives when it changes), so a frame without the
// key must leave the flag alone.
// ===========================================================================

// The disconnect-edge reset of these subjects is covered at the controller
// level in test_plr_offer_preparing.cpp: a reset performed by the test itself
// would only assert the test's own write.
