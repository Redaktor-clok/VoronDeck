// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../catch_amalgamated.hpp"
#include "helix-xml/src/xml/lv_xml.h"

namespace {
int fan_clicks;
int notification_clicks;
}

TEST_CASE_METHOD(LVGLUITestFixture, "Home fan card contains no notification shortcut",
                 "[vorondeck][home-design][home-notifications]") {
    fan_clicks = 0;
    notification_clicks = 0;
    lv_xml_register_event_cb(nullptr, "on_fan_stack_clicked",
                             [](lv_event_t*) { ++fan_clicks; });
    lv_xml_register_event_cb(nullptr, "status_notification_history_clicked",
                             [](lv_event_t*) { ++notification_clicks; });
    auto* root = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_fan_portrait", nullptr));
    REQUIRE(root != nullptr);
    auto* notifications = lv_obj_find_by_name(root, "home_notification_button");
    auto* fans = lv_obj_find_by_name(root, "home_fan_bank");
    CHECK(notifications == nullptr);
    REQUIRE(fans != nullptr);
    lv_obj_send_event(fans, LV_EVENT_CLICKED, nullptr);
    CHECK(fan_clicks == 1);
    CHECK(notification_clicks == 0);
}
