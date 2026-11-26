// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The post-boot brightness re-assert is scheduled by the board row, not by which
// backlight driver happens to be in use.

#include "backlight_backend.h"
#include "board_identity.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "test_helpers/display_manager_test_access.h"
#include "test_helpers/scoped_board.h"

#include <memory>

#include "../../catch_amalgamated.hpp"

namespace {

// A driver whose name is not the one the old gate keyed on.
class NamedBacklight : public BacklightBackend {
  public:
    bool set_brightness(int) override {
        return true;
    }
    int get_brightness() const override {
        return 100;
    }
    bool is_available() const override {
        return true;
    }
    const char* name() const override {
        return "Sysfs";
    }
};

int timer_count() {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        ++n;
    }
    return n;
}

int scheduled_by(helix::board::Board board) {
    helix::test::ScopedBoard scoped(board);
    DisplayManager dm;
    DisplayManagerTestAccess::set_backlight(dm, std::make_unique<NamedBacklight>());
    const int before = timer_count();
    DisplayManagerTestAccess::schedule_brightness_reassert(dm);
    return timer_count() - before;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Linux hosts do not schedule vendor brightness overrides",
                 "[display][board]") {
    for (int id = 0; id < helix::board::kBoardCount; ++id) {
        INFO("persisted board id: " << id);
        CHECK(scheduled_by(static_cast<helix::board::Board>(id)) == 0);
    }
}
