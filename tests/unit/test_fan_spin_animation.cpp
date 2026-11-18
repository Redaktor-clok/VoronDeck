// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/fan_spin_animation.h"
#include "lvgl_test_fixture.h"
#include "catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE_METHOD(LVGLTestFixture, "Fan animation skips hidden ancestors and resumes on reveal",
                 "[fan_spin][animation]") {
    auto* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 100, 100);
    auto* icon = lv_obj_create(parent);
    lv_obj_set_size(icon, 24, 24);
    lv_obj_update_layout(test_screen());
    REQUIRE(lv_obj_is_visible(icon));
    fan_spin_anim_cb(icon, 300);
    REQUIRE(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) == 300);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 1000; ++i)
        fan_spin_anim_cb(icon, i);
    CHECK(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) == 300);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_HIDDEN);
    fan_spin_anim_cb(icon, 900);
    CHECK(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) == 900);
    fan_spin_stop(icon);
    CHECK(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Fan rebinding at the same speed retains its phase",
                 "[fan_spin][animation]") {
    auto* icon = lv_obj_create(test_screen());
    lv_obj_set_size(icon, 24, 24);
    lv_obj_update_layout(test_screen());
    fan_spin_start(icon, 50);
    auto* running = lv_anim_get(icon, fan_spin_anim_cb);
    REQUIRE(running);
    running->act_time = 123;
    for (int i = 0; i < 100; ++i)
        fan_spin_start(icon, 50);
    REQUIRE(lv_anim_get(icon, fan_spin_anim_cb) == running);
    CHECK(running->act_time == 123);
    fan_spin_start(icon, 100);
    REQUIRE(lv_anim_get(icon, fan_spin_anim_cb));
    CHECK(lv_anim_get_time(lv_anim_get(icon, fan_spin_anim_cb)) == kFanSpinMinDurationMs);
    fan_spin_stop(icon);
    CHECK_FALSE(lv_anim_get(icon, fan_spin_anim_cb));
    CHECK(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) == 0);
    fan_spin_start(nullptr, 100);
    fan_spin_stop(nullptr);
    fan_spin_anim_cb(nullptr, 100);
}
