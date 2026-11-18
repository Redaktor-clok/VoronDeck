// SPDX-License-Identifier: GPL-3.0-or-later
#include "heater_bank_layout.h"

#include "../catch_amalgamated.hpp"
using namespace helix;
TEST_CASE("Heater bank inventories exclude passive and targetless objects", "[heater-bank]") {
    for (auto id : {"extruder", "extruder1", "extruder12", "heater_bed", "heater_generic chamber",
                    "heater_generic dryer"})
        REQUIRE(controllable_heater_id(id));
    for (auto id : {"", "extruder_sensor", "extruder_stepper aux", "temperature_sensor chamber",
                    "temperature_fan chamber", "heater_generic ", "bme280 chamber"})
        REQUIRE_FALSE(controllable_heater_id(id));
}
TEST_CASE("Heater bank wraps after three devices in both orientations", "[heater-bank]") {
    for (size_t n : {1u, 3u, 4u, 6u}) {
        auto landscape = heater_bank_shape(n, false);
        auto portrait = heater_bank_shape(n, true);
        CHECK(landscape.columns == (n < 3 ? n : 3));
        CHECK(landscape.rows == (n + 2) / 3);
        CHECK(portrait.rows == landscape.columns);
        CHECK(portrait.columns == landscape.rows);
    }
    CHECK(heater_bank_shape(0, true).rows == 1);
}
TEST_CASE("Heater selection resizing preserves neighbors and rejects a full grid atomically",
          "[heater-bank]") {
    GridLayout grid(UiBreakpoint::Small, {12, 8});
    GridPlacement tile{"heater_bank", 0, 0, 3, 3};
    REQUIRE(grid.place(tile));
    REQUIRE(grid.place({"printer_image", 3, 0, 9, 3}));
    REQUIRE(fit_heater_bank(grid, tile, 4, false));
    CHECK(tile.colspan == 9);
    CHECK(tile.rowspan == 5);
    CHECK(tile.row == 3);
    REQUIRE(grid.find_placement("printer_image"));
    CHECK(grid.find_placement("printer_image")->row == 0);
    CHECK(grid.find_placement("printer_image")->col == 3);
    const auto before = tile;
    REQUIRE_FALSE(fit_heater_bank(grid, tile, 6, true));
    CHECK(tile.col == before.col);
    CHECK(tile.row == before.row);
    CHECK(tile.colspan == before.colspan);
    CHECK(tile.rowspan == before.rowspan);
    CHECK(grid.find_placement("heater_bank")->rowspan == before.rowspan);
    REQUIRE(fit_heater_bank(grid, tile, 1, false));
    CHECK(tile.colspan == 3);
    CHECK(tile.rowspan == 3);
}
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "config.h"
#include "home_screen_preferences.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/heater_bank_widget.h"
#include "temperature_sensor_manager.h"

namespace helix {
void register_heater_bank_widget();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Heater picker Cancel discards and OK saves selection with native footprint",
                 "[heater-bank]") {
    const int scenario = GENERATE(0, 1, 2, 3, 4);
    const int widths[] = {290, 349, 228, 344, 527};
    const int heights[] = {103, 103, 178, 178, 264};
    const bool portrait_case = scenario == 2 || scenario == 3;
    const int grant_width = widths[scenario];
    const int grant_height = heights[scenario];
    CAPTURE(grant_width, grant_height);
    register_heater_bank_widget();
    const auto hardware = test::discovery_of({"extruder", "heater_bed", "heater_generic dryer",
                                              "heater_generic dryer2", "temperature_sensor mcu"});
    state().set_hardware(hardware);
    state().temperature_state().init_extruders({"extruder"});
    auto& sensor_manager = sensors::TemperatureSensorManager::instance();
    sensor_manager.init_subjects();
    sensor_manager.discover(hardware);
    auto& manager = PanelWidgetManager::instance();
    auto& config = manager.get_widget_config("home");
    config.load();
    config.switch_to_grid(12, 8);
    config.page_entries_mut(0).clear();
    config.place_entry("heater_bank", 0, 0, 0, 9, 3);
    config.save();
    HeaterBankWidget tile(state());
    tile.set_panel_id("home");
    auto* definition = find_widget_def("heater_bank");
    REQUIRE(definition);
    REQUIRE(definition->init_subjects);
    definition->init_subjects();
    auto* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_heater_bank", nullptr));
    REQUIRE(root);
    tile.attach_tile(root, test_screen());
    // Start below the maximum density face so a larger grant exercises scaling.
    tile.notify_size_changed(9, 3, 290, 103);
    process_lvgl(20);
    REQUIRE(lv_obj_find_by_name(root, "heater_bank_device_0"));
    REQUIRE(lv_obj_find_by_name(root, "home_nozzle_cold_glyph"));
    auto* rung = lv_xml_get_subject(nullptr, "heater_bank_value_rung");
    REQUIRE(rung);
    const int small_rung = lv_subject_get_int(rung);
    CHECK_FALSE(tile.fits_at(20, 20));
    auto* display = lv_display_get_default();
    const int display_w = lv_display_get_horizontal_resolution(display);
    const int display_h = lv_display_get_vertical_resolution(display);
    lv_display_set_resolution(display, 480, 800);
    CHECK(tile.fits_at(220, 260));
    CHECK(tile.fits_at(344, 178));
    CHECK(tile.fits_at(226, 178));
    lv_display_set_resolution(display, display_w, display_h);
    tile.notify_size_changed(9, 3, 780, 280);
    process_lvgl(20);
    CHECK(lv_subject_get_int(rung) > small_rung);
    CHECK(config.page_entries(0).front().colspan == 9);
    lv_display_set_resolution(display, portrait_case ? 480 : 800, portrait_case ? 800 : 480);
    REQUIRE(tile.fits_at(grant_width, grant_height));
    tile.notify_size_changed(6, 3, grant_width, grant_height);
    const int four_device_rung = lv_subject_get_int(rung);
    const int four_icon_height =
        lv_obj_get_style_text_font(lv_obj_find_by_name(root, "home_nozzle_cold_glyph"),
                                   LV_PART_MAIN)
            ->line_height;
    CHECK_FALSE(lv_obj_find_by_name(root, "heater_bank_settings"));
    GridEditMode editor;
    REQUIRE(editor.enter(test_screen(), &config));
    editor.select_widget(root);
    auto* configure = GridEditModeTestAccess::configure_button(editor);
    REQUIRE(configure);
    const auto original = config.get_widget_config("heater_bank");
    lv_obj_send_event(configure, LV_EVENT_CLICKED, nullptr);
    process_lvgl(20);
    auto* modal = Modal::get_top();
    REQUIRE(modal);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "heater_bank_cfg_count")) == 4);
    auto* sw = lv_obj_find_by_name(modal, "heater_enabled_1");
    REQUIRE(sw);
    lv_obj_send_event(lv_obj_find_by_name(modal, "heater_layout_list"), LV_EVENT_CLICKED, nullptr);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "heater_bank_cfg_mode")) == 1);
    lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "heater_bank_cfg_1_on")) == 0);
    lv_obj_send_event(lv_obj_find_by_name(modal, "btn_secondary"), LV_EVENT_CLICKED, nullptr);
    process_lvgl(400);
    CHECK(config.get_widget_config("heater_bank") == original);
    lv_obj_send_event(configure, LV_EVENT_CLICKED, nullptr);
    process_lvgl(20);
    modal = Modal::get_top();
    REQUIRE(modal);
    sw = lv_obj_find_by_name(modal, "heater_enabled_1");
    REQUIRE(lv_obj_has_state(sw, LV_STATE_CHECKED));
    lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "heater_bank_cfg_1_on")) == 0);
    lv_obj_send_event(lv_obj_find_by_name(modal, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    process_lvgl(400);
    CHECK(config.get_widget_config("heater_bank")["heaters"]["heater_bed"] == false);
    CHECK_FALSE(HomeScreenPreferences(*Config::get_instance())
                    .is_visible(HomeScreenPreferences::DeviceKind::Heater, "heater_bed"));
    CHECK(config.page_entries(0).front().colspan == 9);
    CHECK(config.page_entries(0).front().rowspan == 3);
    tile.attach(root, test_screen());
    tile.notify_size_changed(6, 3, grant_width, grant_height);
    CHECK(lv_subject_get_int(rung) > four_device_rung);
    if (portrait_case) {
        auto* glyph = lv_obj_find_by_name(root, "home_nozzle_cold_glyph");
        REQUIRE(glyph);
        CHECK(lv_obj_get_style_text_font(glyph, LV_PART_MAIN)->line_height > four_icon_height);
    }
    for (bool enabled : {true, false}) {
        lv_obj_send_event(configure, LV_EVENT_CLICKED, nullptr);
        process_lvgl(20);
        modal = Modal::get_top();
        REQUIRE(modal);
        sw = lv_obj_find_by_name(modal, "heater_enabled_1");
        REQUIRE(sw);
        if (enabled)
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_send_event(lv_obj_find_by_name(modal, "btn_primary"), LV_EVENT_CLICKED, nullptr);
        process_lvgl(400);
        tile.attach(root, test_screen());
        tile.notify_size_changed(6, 3, grant_width, grant_height);
        if (enabled)
            CHECK(lv_subject_get_int(rung) == four_device_rung);
        else
            CHECK(lv_subject_get_int(rung) > four_device_rung);
        CHECK(config.page_entries(0).front().colspan == 9);
        CHECK(config.page_entries(0).front().rowspan == 3);
    }
    lv_obj_send_event(configure, LV_EVENT_CLICKED, nullptr);
    process_lvgl(20);
    modal = Modal::get_top();
    REQUIRE(modal);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "heater_bank_cfg_mode")) == 0);
    lv_obj_send_event(lv_obj_find_by_name(modal, "heater_layout_list"), LV_EVENT_CLICKED, nullptr);
    lv_obj_send_event(lv_obj_find_by_name(modal, "btn_primary"), LV_EVENT_CLICKED, nullptr);
    process_lvgl(400);
    CHECK(config.get_widget_config("heater_bank")["display_mode"] == "list");
    CHECK(config.page_entries(0).front().colspan == 9);
    CHECK(config.page_entries(0).front().rowspan == 3);
    editor.exit();
    // Returning from Home settings reuses the tile; inventory has not changed.
    auto* row_count = lv_xml_get_subject(nullptr, "heater_bank_count");
    REQUIRE(row_count);
    HomeScreenPreferences preferences(*Config::get_instance());
    for (bool visible : {false, true}) {
        for (const auto& name : {"extruder", "heater_bed", "heater_generic dryer",
                                 "heater_generic dryer2"})
            preferences.set_visible(HomeScreenPreferences::DeviceKind::Heater, name, visible);
        tile.on_activate();
        process_lvgl(100);
        CHECK(lv_subject_get_int(row_count) == (visible ? 4 : 0));
    }
    lv_display_set_resolution(display, display_w, display_h);
    tile.detach_tile();
    lv_obj_delete(root);
    sensor_manager.deinit_subjects();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Compact heater faces fit actual 1 3 4 6 device inventories",
                 "[heater-bank][heater-stable-scale]") {
    const int count = GENERATE(1, 3, 4, 6);
    const bool list = GENERATE(false, true);
    const bool portrait = GENERATE(false, true);
    CAPTURE(count, portrait);
    ScopedResolution resolution(lv_display_get_default(), portrait ? 480 : 800,
                                portrait ? 800 : 480);
    register_heater_bank_widget();
    std::vector<std::string> devices{"extruder"};
    if (count > 1)
        devices.push_back("heater_bed");
    while (devices.size() < static_cast<size_t>(count))
        devices.push_back("heater_generic aux" + std::to_string(devices.size()));
    auto hardware = test::discovery_of(devices);
    state().set_hardware(hardware);
    state().temperature_state().init_extruders({"extruder"});
    auto& sensors = sensors::TemperatureSensorManager::instance();
    sensors.init_subjects();
    sensors.discover(hardware);
    auto* definition = find_widget_def("heater_bank");
    REQUIRE(definition);
    definition->init_subjects();
    HeaterBankWidget tile(state());
    if (list)
        tile.set_config({{"display_mode", "list"}});
    auto* root = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), tile.get_component_name().c_str(), nullptr));
    REQUIRE(root);
    tile.attach_tile(root, test_screen());
    const int width = portrait ? 228 : 290;
    const int height = portrait ? 178 : GENERATE(93, 103);
    lv_obj_set_size(root, width, height);
    if (portrait && !list && count == 3) {
        // Match the user's live228x178 tile:5px outer padding and row gaps.
        lv_obj_set_style_pad_all(root, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_gap(lv_obj_find_by_name(root, "heater_bank_cells"), 5, LV_PART_MAIN);
    }
    if (list) {
        // Native Home can still expose the old full-canvas geometry at grant notification.
        lv_obj_set_height(root, portrait ? 800 : 480);
        lv_obj_update_layout(root);
    }
    tile.notify_size_changed(6, 3, width, height);
    lv_obj_set_size(root, width, height);
    lv_obj_update_layout(root);
    process_lvgl(20);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "heater_bank_count")) == count);
    REQUIRE(tile.fits_at(width, height));
    if (portrait && !list && count == 3) {
        auto* cells = lv_obj_find_by_name(root, "heater_bank_cells");
        auto* glyph = lv_obj_find_by_name(root, "home_nozzle_cold_glyph");
        REQUIRE(glyph);
        CHECK(lv_obj_get_style_pad_top(cells, LV_PART_MAIN) == 0);
        CHECK(lv_obj_get_style_text_font(glyph, LV_PART_MAIN) ==
              lv_xml_get_font_silent(nullptr, "mdi_icons_32"));
        // The reference now reserves a third text line and power below the icon.
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "heater_bank_value_rung")) >= 2);
        auto* name = lv_obj_find_by_name(root, "heater_bank_name");
        REQUIRE(name);
        CHECK(lv_obj_get_style_text_font(name, LV_PART_MAIN) ==
              lv_xml_get_font_silent(nullptr, "noto_sans_light_11"));
    }
    if (list) {
        auto* cells = lv_obj_find_by_name(root, "heater_bank_cells");
        REQUIRE(cells);
        lv_obj_scroll_to(cells, 0, 0, LV_ANIM_OFF);
        lv_obj_update_layout(cells);
        CHECK(lv_obj_get_scroll_dir(cells) == LV_DIR_VER);
        auto* first = lv_obj_find_by_name(root, "heater_bank_device_0");
        const auto last_name = "heater_bank_device_" + std::to_string(count - 1);
        auto* last = lv_obj_find_by_name(root, last_name.c_str());
        REQUIRE(first);
        REQUIRE(last);
        CHECK(lv_obj_get_x(first) == lv_obj_get_x(last));
        CHECK_FALSE(lv_obj_find_by_name(root, "heater_bank_settings"));
        lv_area_t root_box{}, first_box{};
        lv_obj_get_coords(root, &root_box);
        lv_obj_get_coords(first, &first_box);
        CHECK(first_box.y1 - root_box.y1 <= 8);
        auto* name = lv_obj_find_by_name(first, "heater_bank_name");
        auto* reading = lv_obj_find_by_name(first, "heater_bank_value");
        if (portrait) {
            CHECK(lv_obj_get_style_text_font(reading, LV_PART_MAIN)->line_height <=
                  lv_xml_get_font_silent(nullptr, "noto_sans_20")->line_height);
            CHECK(lv_obj_get_style_text_font(name, LV_PART_MAIN)->line_height <=
                  lv_xml_get_font_silent(nullptr, "noto_sans_16")->line_height);
        }
        CHECK(lv_obj_get_x(reading) > lv_obj_get_x(name));
        // Portrait names intentionally use a smaller face than the reading.
        // They share a row center, not the top edge of different line heights.
        const int reading_center = lv_obj_get_y(reading) + lv_obj_get_height(reading) / 2;
        const int name_center = lv_obj_get_y(name) + lv_obj_get_height(name) / 2;
        CHECK(std::abs(reading_center - name_center) <= 1);
        if (count == 6) {
            // Shared fixture focus can leave the container at its last child.
            // Measure the available extent from an explicit start position.
            lv_obj_scroll_to(cells, 0, 0, LV_ANIM_OFF);
            lv_obj_update_layout(cells);
            CHECK(lv_obj_get_scroll_bottom(cells) > 0);
            lv_obj_scroll_to(cells, 0, 1000, LV_ANIM_OFF);
            CHECK(lv_obj_get_scroll_y(cells) > 0);
            lv_obj_update_layout(cells);
            lv_area_t viewport{}, last_box{};
            lv_obj_get_coords(cells, &viewport);
            lv_obj_get_coords(last, &last_box);
            CHECK(last_box.y1 >= viewport.y1);
            CHECK(last_box.y2 <= viewport.y2);
            CHECK(lv_obj_get_height(last) <= lv_obj_get_content_height(cells));
        }
    }
    auto* value = lv_obj_find_by_name(root, "heater_bank_value");
    auto* caption = lv_obj_find_by_name(root, "heater_bank_name");
    REQUIRE(value);
    REQUIRE(caption);
    REQUIRE(lv_xml_get_font_silent(nullptr, "noto_sans_12"));
    REQUIRE(lv_xml_get_font_silent(nullptr, "noto_sans_light_10"));
    CHECK(lv_obj_get_style_text_font(value, LV_PART_MAIN)->line_height >=
          lv_xml_get_font_silent(nullptr, "noto_sans_12")->line_height);
    CHECK(lv_obj_get_style_text_font(caption, LV_PART_MAIN)->line_height >=
          lv_xml_get_font_silent(nullptr, list ? "noto_sans_light_10" : "noto_sans_light_8")->line_height);
    CHECK(lv_obj_find_by_name(root, "heater_bank_footer") == nullptr);
    if (!list && !portrait && count == 3) {
        CHECK(lv_obj_get_style_text_font(value, LV_PART_MAIN)->line_height >=
              lv_xml_get_font_silent(nullptr, "noto_sans_20")->line_height);
    }
    auto* setpoint = lv_obj_find_by_name(root, list ? "heater_bank_status" : "heater_bank_setpoint");
    auto* power_label = lv_obj_find_by_name(root, "heater_bank_power");
    auto* device = lv_obj_find_by_name(root, "heater_bank_device_0");
    auto* primary = lv_obj_find_by_name(device, "heater_bank_primary");
    REQUIRE(setpoint);
    REQUIRE(power_label);
    REQUIRE(device);
    lv_obj_scroll_to(lv_obj_find_by_name(root, "heater_bank_cells"), 0, 0, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    if (list)
        CHECK(std::string(lv_label_get_text(setpoint)) == lv_tr("Off"));
    else
        CHECK(std::string(lv_label_get_text(setpoint)).empty());

    // Every device keeps its primary fonts, even when another heater starts.
    std::vector<std::pair<lv_obj_t*, const lv_font_t*>> fonts;
    for (int i = 0; i < count; ++i) {
        auto* item = lv_obj_find_by_name(root, ("heater_bank_device_" + std::to_string(i)).c_str());
        REQUIRE(item);
        for (const char* name : {"heater_bank_value", "heater_bank_name", "home_nozzle_cold_glyph",
                                 "nozzle_icon_glyph", "bed_icon_glyph", "chamber_icon_glyph"}) {
            if (auto* obj = lv_obj_find_by_name(item, name))
                fonts.emplace_back(obj, lv_obj_get_style_text_font(obj, LV_PART_MAIN));
        }
    }
    std::vector<std::pair<lv_subject_t*, int>> rungs;
    for (const char* name : {"heater_bank_value_rung", "heater_bank_icon_rung", "heater_bank_label_rung"}) {
        auto* subject = lv_xml_get_subject(nullptr, name);
        REQUIRE(subject);
        rungs.emplace_back(subject, lv_subject_get_int(subject));
    }
    auto center_y = [](lv_obj_t* obj) {
        lv_area_t box{};
        lv_obj_get_coords(obj, &box);
        return box.y1 + box.y2;
    };
    const int cold_center = primary ? center_y(primary) : 0;
    const int cold_target_height = lv_obj_get_height(setpoint);
    const int cold_power_height = lv_obj_get_height(power_label);
    if (primary)
        CHECK(std::abs(cold_center - center_y(device)) <= 1);

    SubjectLifetime target_life, current_life, power_life;
    auto* target = state().temperature_state().get_extruder_target_subject("extruder", target_life);
    auto* current = state().temperature_state().get_extruder_temp_subject("extruder", current_life);
    auto* power = state().temperature_state().get_extruder_power_subject("extruder", power_life);
    REQUIRE(target);
    REQUIRE(current);
    REQUIRE(power);
    lv_subject_set_int(current, 3000);
    for (int target_deci : {0, 2220, 3000, 0}) {
        for (int percent : {0, 100}) {
            CAPTURE(count, portrait, list, target_deci, percent);
            lv_subject_set_int(target, target_deci);
            lv_subject_set_int(power, percent);
            process_lvgl(20);
            // Explicitly repeating layout must give the same result too.
            tile.notify_size_changed(6, 3, width, height);
            lv_obj_update_layout(root);
            for (const auto& [obj, font] : fonts)
                CHECK(lv_obj_get_style_text_font(obj, LV_PART_MAIN) == font);
            for (const auto& [subject, rung] : rungs)
                CHECK(lv_subject_get_int(subject) == rung);
            if (primary) {
                CHECK(std::abs(center_y(primary) - cold_center) <= 1);
                CHECK(std::abs(center_y(primary) - center_y(device)) <= 1);
            }
            CHECK(lv_obj_get_height(setpoint) == cold_target_height);
            CHECK(lv_obj_get_height(power_label) == cold_power_height);
            if (target_deci > 0) {
                CHECK_FALSE(lv_obj_is_hidden(setpoint));
                CHECK_FALSE(lv_obj_is_hidden(power_label));
                CHECK(std::string(lv_label_get_text(power_label)) == std::to_string(percent) + "%");
                lv_area_t cell{}, value_box{}, target_box{};
                lv_obj_get_coords(device, &cell);
                lv_obj_get_coords(value, &value_box);
                lv_obj_get_coords(setpoint, &target_box);
                for (auto* label : {value, setpoint, power_label}) {
                    lv_area_t box{};
                    lv_obj_get_coords(label, &box);
                    CHECK(box.x1 >= cell.x1);
                    CHECK(box.x2 <= cell.x2);
                    CHECK(box.y1 >= cell.y1);
                    CHECK(box.y2 <= cell.y2);
                    lv_point_t text{};
                    lv_text_get_size(&text, lv_label_get_text(label),
                        lv_obj_get_style_text_font(label, LV_PART_MAIN), 0, 0,
                        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                    CHECK(text.x <= lv_obj_get_content_width(label));
                }
                if (list) {
                    CHECK(target_box.x1 > value_box.x2);
                    CHECK(target_box.y1 >= value_box.y1);
                    CHECK(target_box.y2 <= value_box.y2);
                } else {
                    lv_area_t name_box{}, power_box{}, icon_box{};
                    lv_obj_get_coords(caption, &name_box);
                    lv_obj_get_coords(power_label, &power_box);
                    lv_obj_get_coords(lv_obj_find_by_name(device, "heater_bank_icon_slot"), &icon_box);
                    CHECK(target_box.x1 == value_box.x1);
                    CHECK(target_box.y1 > value_box.y2);
                    CHECK(name_box.y1 > target_box.y2);
                    CHECK(power_box.x2 < name_box.x1);
                    CHECK(power_box.y1 > icon_box.y2);
                    CHECK(power_box.y2 == name_box.y2);
                }
            } else if (list) {
                CHECK(std::string(lv_label_get_text(setpoint)) == lv_tr("Off"));
            } else {
                CHECK(std::string(lv_label_get_text(setpoint)).empty());
                CHECK(std::string(lv_label_get_text(power_label)).empty());
                CHECK_FALSE(lv_obj_is_hidden(setpoint));
                CHECK_FALSE(lv_obj_is_hidden(power_label));
            }
        }
    }
    tile.detach_tile();
    lv_obj_delete(root);
    sensors.deinit_subjects();
}
