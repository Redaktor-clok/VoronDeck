// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_nav_manager.h"
#include "ui_overlay_network_settings.h"
#include "ui_panel_home.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/ui/text_metrics.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"
using namespace helix;

TEST_CASE_METHOD(LVGLUITestFixture, "Russian navbar captions stay inside their destinations",
                 "[nav-shared]") {
    const int scenario = GENERATE(0, 1, 2);
    const int width = scenario == 0 ? 480 : (scenario == 1 ? 800 : 320);
    const int height = scenario == 0 ? 800 : (scenario == 1 ? 480 : 240);
    ScopedResolution resolution(lv_display_get_default(), width, height);
    theme_manager_refresh_layout_constants(lv_display_get_default());
    auto* navbar = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
    REQUIRE(navbar);
    const char* names[] = {"home", "controls", "print_select", "filament", "settings", "advanced"};
    const char* captions[] = {"Главная", "Управление", "Файлы", "Филамент", "Настройки", "Ещё"};
    for (int i = 0; i < 6; ++i) {
        auto name = std::string("nav_label_") + names[i];
        auto* label = lv_obj_find_by_name(navbar, name.c_str());
        REQUIRE(label);
        lv_label_set_text(label, captions[i]);
    }
    process_lvgl(40);
    lv_obj_update_layout(navbar);
    for (int i = 0; i < 6; ++i) {
        auto name = std::string("nav_label_") + names[i];
        auto* label = lv_obj_find_by_name(navbar, name.c_str());
        auto* button = lv_obj_get_parent(label);
        CAPTURE(width, height, name);
        CHECK(lv_obj_get_width(label) <= lv_obj_get_content_width(button));
        lv_area_t label_area{}, button_area{};
        lv_obj_get_coords(label, &label_area);
        lv_obj_get_coords(button, &button_area);
        CHECK(label_area.y2 <= button_area.y2);
        const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        if (scenario != 2)
            CHECK(ui::text_width(captions[i], font) <= lv_obj_get_content_width(label));
        else
            CHECK((lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR ||
                   lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_CLIP));
    }
    lv_obj_delete(navbar);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Printer and native network header persist across main tabs",
                 "[nav-shared]") {
    HomePanel home(state(), nullptr);
    home.init_subjects();
    auto* shell = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "app_layout", nullptr));
    REQUIRE(shell);
    auto* header = lv_obj_find_by_name(shell, "home_header");
    auto* content = lv_obj_find_by_name(shell, "content_area");
    auto* container = lv_obj_find_by_name(shell, "panel_container");
    REQUIRE(header);
    REQUIRE(content);
    REQUIRE(container);
    CHECK(lv_obj_get_parent(header) == content);
    INFO("header " << lv_obj_get_width(header) << "x" << lv_obj_get_height(header) << " content "
                   << lv_obj_get_width(content) << "x" << lv_obj_get_height(content));
    lv_obj_t* panels[kUiPanelCount]{};
    const char* names[] = {"home_panel",     "print_select_panel", "controls_panel",
                           "filament_panel", "settings_panel",     "advanced_panel"};
    for (int i = 0; i < kUiPanelCount; ++i) {
        panels[i] = lv_obj_find_by_name(container, names[i]);
        REQUIRE(panels[i]);
    }
    auto& nav = NavigationManager::instance();
    nav.set_panels(panels);
    for (int i = 0; i < kUiPanelCount; ++i) {
        nav.set_active(static_cast<PanelId>(i));
        process_lvgl(20);
        lv_obj_update_layout(shell);
        INFO("live header " << lv_obj_get_width(header) << "x" << lv_obj_get_height(header));
        CHECK(lv_obj_is_visible(header));
    }
    auto* network = lv_obj_find_by_name(header, "shared_network_button");
    REQUIRE(network);
    lv_obj_send_event(network, LV_EVENT_CLICKED, nullptr);
    process_lvgl(30);
    auto& overlay = get_network_settings_overlay();
    CHECK(overlay.is_created());
    CHECK(overlay.is_visible());
    overlay.close();
    process_lvgl(400);
    overlay.cleanup();
    for (auto& panel : panels)
        panel = nullptr;
    nav.set_panels(panels);
    lv_obj_delete(shell);
}
