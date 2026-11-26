// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_notification_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

namespace {
int fan_clicks;
int notification_clicks;
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Home fan card contains no notification shortcut",
                 "[vorondeck][home-design][home-notifications]") {
    fan_clicks = 0;
    notification_clicks = 0;
    lv_xml_register_event_cb(nullptr, "on_fan_stack_clicked", [](lv_event_t*) { ++fan_clicks; });
    lv_xml_register_event_cb(nullptr, "status_notification_history_clicked",
                             [](lv_event_t*) { ++notification_clicks; });
    auto* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_fan_portrait", nullptr));
    REQUIRE(root != nullptr);
    auto* notifications = lv_obj_find_by_name(root, "home_notification_button");
    auto* fans = lv_obj_find_by_name(root, "home_fan_bank");
    CHECK(notifications == nullptr);
    REQUIRE(fans != nullptr);
    lv_obj_send_event(fans, LV_EVENT_CLICKED, nullptr);
    CHECK(fan_clicks == 1);
    CHECK(notification_clicks == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Home notification indicator follows count and severity",
                 "[vorondeck][home-design][home-notifications]") {
    auto& notifications = NotificationManager::instance();
    notifications.init_subjects();
    auto* root = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "home_notification_indicator", nullptr));
    REQUIRE(root);
    auto* count = lv_obj_find_by_name(root, "home_notification_count");
    auto* icon = lv_obj_find_by_name(root, "home_notification_severity");
    REQUIRE(count);
    REQUIRE(icon);
    notifications.update_notification_count(0);
    CHECK(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));
    notifications.update_notification_count(3);
    CHECK_FALSE(lv_obj_has_flag(count, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(count)) == "3");
    notifications.update_notification(NotificationStatus::WARNING);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN),
                      theme_manager_get_color("warning")));
    notifications.update_notification(NotificationStatus::ERROR);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN),
                      theme_manager_get_color("danger")));
    lv_obj_delete(root);
    notifications.deinit_subjects();
}
