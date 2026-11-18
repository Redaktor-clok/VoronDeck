// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "config.h"
#include "home_screen_preferences.h"
#include "src/ui/panel_widgets/active_spool_widget.h"
#include "src/ui/panel_widgets/ams_status_widget.h"
#include "src/ui/panel_widgets/spool_display_picker.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_mini_status.h"
#include "ui_spool_canvas.h"
#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
void require_inside(lv_obj_t* child, lv_obj_t* root) {
    REQUIRE(child != nullptr);
    lv_area_t c, r;
    lv_obj_get_coords(child, &c);
    lv_obj_get_coords(root, &r);
    CHECK(c.x1 >= r.x1);
    CHECK(c.x2 <= r.x2);
    CHECK(c.y1 >= r.y1);
    CHECK(c.y2 <= r.y2);
}
void require_numbers_fit(lv_obj_t* label) {
    REQUIRE(label != nullptr);
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    CHECK(size.x <= lv_obj_get_width(label));
    CHECK(size.y <= lv_obj_get_height(label));
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "active spool preserves key readings inside an existing small tile",
                 "[spool-responsive][spool-simple][active_spool]") {
    const bool simplified = GENERATE(false, true);
    auto& ams = AmsState::instance();
    ams.clear_backends();
    ams.init_subjects(false);
    auto* config = Config::get_instance();
    const auto old_units = HomeScreenPreferences(*config).spool_units();
    HomeScreenPreferences(*config).set_spool_units({true, true, true});
    SlotInfo slot;
    slot.material = "A very long filament material description";
    slot.brand = "A very long filament vendor description";
    slot.color_rgb = 0xAA0000;
    slot.total_weight_g = 1000;
    slot.remaining_weight_g = 850;
    slot.remaining_length_m = 80;
    ams.set_external_spool_info_in_memory(slot);
    PanelWidgetHarness<ActiveSpoolWidget> h(test_screen(), HarnessConfig{{{"simplified", simplified}}}, api());
    h.resize(2, 2, 230, 103);
    process_lvgl(30);
    lv_obj_update_layout(h.root());
    require_inside(h.child("spool_wide"), h.root());
    require_inside(h.child("spoolman_material"), h.root());
    auto* percent = h.child("spool_simple_percent");
    auto* amount = h.child("spoolman_weight");
    require_inside(percent, h.root());
    require_inside(amount, h.root());
    require_numbers_fit(percent);
    require_numbers_fit(amount);
    CHECK(std::string(lv_label_get_text(percent)) == "85%");
    const std::string text = lv_label_get_text(amount);
    CHECK(text.find("850") != std::string::npos);
    CHECK(text.find("80") != std::string::npos);
    CHECK(ui_spool_canvas_get_fill_level(h.child("spool_wide")) == Catch::Approx(0.85f));
    ams.clear_external_spool_info();
    HomeScreenPreferences(*config).set_spool_units(old_units);
}

TEST_CASE_METHOD(LVGLUITestFixture, "four AMS spools keep complete rings and readings at one baseline",
                 "[spool-responsive][spool-simple][ams_mini]") {
    const bool simplified = GENERATE(false, true);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    auto* backend = mock.get();
    ams.set_backend(std::move(mock));
    backend->start();
    auto* config = Config::get_instance();
    const auto old_units = HomeScreenPreferences(*config).spool_units();
    HomeScreenPreferences(*config).set_spool_units({true, true, true});
    for (int i = 0; i < 4; ++i) {
        auto slot = backend->get_slot_info(i);
        slot.material = i == 1 ? "Silk PLA" : "ABS";
        slot.total_weight_g = 1000;
        slot.remaining_weight_g = 850 - i * 100;
        test::apply_edit(*backend, i, slot);
        backend->force_slot_status(i, SlotStatus::AVAILABLE);
        backend->force_slot_remaining(i, 80 - i * 10);
    }
    ams.sync_from_backend();
    ui_ams_mini_status_init();
    PanelWidgetHarness<AmsStatusWidget> h(test_screen(), HarnessConfig{{{"simplified", simplified}}});
    h.resize(2, 2, 230, 103);
    process_lvgl(30);
    lv_obj_update_layout(h.root());
    int baseline = -1;
    for (int i = 0; i < 4; ++i) {
        auto* cell = h.child(("spool_cell_" + std::to_string(i)).c_str());
        REQUIRE(cell != nullptr);
        auto* ring = lv_obj_find_by_name(cell, "spool_graphic");
        require_inside(ring, h.root());
        lv_area_t area;
        lv_obj_get_coords(ring, &area);
        if (baseline < 0)
            baseline = area.y1;
        CHECK(area.y1 == baseline);
        auto* percent = h.child(("spool_pct_" + std::to_string(i)).c_str());
        auto* amount = h.child(("spool_amount_" + std::to_string(i)).c_str());
        require_inside(percent, h.root());
        require_inside(amount, h.root());
        require_numbers_fit(percent);
        require_numbers_fit(amount);
        CHECK(std::string(lv_label_get_text(percent)) == std::to_string(85 - i * 10) + "%");
        CHECK(std::string(lv_label_get_text(amount)).find(std::to_string(850 - i * 100)) != std::string::npos);
        CHECK(ui::ams_lane_spool_get_fill_level(lv_obj_find_by_name(cell, "lane_spool")) ==
              Catch::Approx((850 - i * 100) / 1000.0f));
    }
    backend->stop();
    ams.clear_backends();
    HomeScreenPreferences(*config).set_spool_units(old_units);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool display choice keeps Full and Simplified selectable",
                 "[spool-simple][picker]") {
    ActiveSpoolWidget active(api());
    CHECK(active.get_component_name() == "panel_widget_active_spool");
    active.set_config({{"simplified", true}});
    CHECK(active.get_component_name() == "panel_widget_active_spool_simple");
    active.set_config({{"simplified", false}});
    CHECK(active.get_component_name() == "panel_widget_active_spool");
    SpoolDisplayPicker picker;
    int selected = -2;
    picker.set_action_callback([&selected](int action, int) { selected = action; });
    auto* anchor = lv_obj_create(test_screen());
    lv_obj_set_size(anchor, 200, 100);
    for (bool simplified : {true, false}) {
        REQUIRE(picker.show_below_widget(test_screen(), anchor));
        auto* row = lv_obj_find_by_name(lv_screen_active(),
            simplified ? "spool_display_simple" : "spool_display_full");
        REQUIRE(row != nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        CHECK(selected == (simplified ? 1 : 0));
        CHECK_FALSE(picker.is_visible());
        process_lvgl(20);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "simplified active spool uses one source for ring and readings",
                 "[spool-simple][active_spool]") {
    auto& ams = AmsState::instance();
    ams.clear_backends();
    // A prior AMS case can deinitialize the process-wide singleton; initialize
    // its observable subjects before constructing the tile, as app startup does.
    ams.init_subjects(false);
    auto* config = Config::get_instance();
    const auto old_units = HomeScreenPreferences(*config).spool_units();
    HomeScreenPreferences(*config).set_spool_units({true, true, true});
    SlotInfo slot;
    slot.material = "ABS";
    slot.color_rgb = 0xFF0000;
    slot.total_weight_g = 1000;
    slot.remaining_weight_g = 500;
    slot.remaining_length_m = 200;
    ams.set_external_spool_info_in_memory(slot);
    PanelWidgetHarness<ActiveSpoolWidget> h(test_screen(), HarnessConfig{{{"simplified", true}}}, api());
    h.resize(2, 2, 240, 100);
    CHECK(h.widget().get_component_name() == "panel_widget_active_spool_simple");
    CHECK(h.widget().has_edit_configure());
    auto* ring = h.child("spool_wide");
    REQUIRE(ring != nullptr);
    CHECK(ui::spool_canvas_get_view(ring) == ui::SpoolView::Ring);
    CHECK(lv_obj_get_width(ring) == 58);
    CHECK(ui_spool_canvas_get_color(ring).red == 255);
    CHECK(std::string(lv_label_get_text(h.child("spoolman_material"))) == "ABS");
    for (int remaining : {0, 500, 1000, 850}) {
        slot.remaining_weight_g = static_cast<float>(remaining);
        ams.set_external_spool_info_in_memory(slot);
        process_lvgl(20);
        CHECK(ui_spool_canvas_get_fill_level(ring) == Catch::Approx(remaining / 1000.0f));
        CHECK(std::string(lv_label_get_text(h.child("spool_simple_percent"))) ==
              std::to_string(remaining / 10) + "%");
        CHECK(std::string(lv_label_get_text(h.child("spoolman_weight"))) ==
              std::to_string(remaining) + " g · 200 m");
    }
    HomeScreenPreferences(*config).set_spool_units({false, false, true});
    h.widget().on_activate();
    CHECK(std::string(lv_label_get_text(h.child("spool_simple_percent"))).empty());
    CHECK(std::string(lv_label_get_text(h.child("spoolman_weight"))) == "200 m");
    slot.remaining_weight_g = -1;
    slot.total_weight_g = -1;
    slot.remaining_length_m = 0;
    ams.set_external_spool_info_in_memory(slot);
    process_lvgl(20);
    CHECK(ui_spool_canvas_get_fill_level(ring) == 0.0f);
    CHECK(std::string(lv_label_get_text(h.child("spool_simple_percent"))).empty());
    CHECK(std::string(lv_label_get_text(h.child("spoolman_weight"))).empty());
    ams.clear_external_spool_info();
    HomeScreenPreferences(*config).set_spool_units(old_units);
}

TEST_CASE_METHOD(LVGLUITestFixture, "simplified AMS keeps material percent ring and amount in order",
                 "[spool-simple][ams_mini]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    auto* backend = mock.get();
    ams.set_backend(std::move(mock));
    backend->start();
    auto* config = Config::get_instance();
    const auto old_units = HomeScreenPreferences(*config).spool_units();
    HomeScreenPreferences(*config).set_spool_units({true, true, true});
    for (int i = 0; i < 4; ++i) {
        auto slot = backend->get_slot_info(i);
        slot.material = "ABS";
        slot.color_rgb = 0xFF0000;
        slot.total_weight_g = 1000;
        slot.remaining_weight_g = i < 3 ? static_cast<float>(i * 500) : -1.0f;
        test::apply_edit(*backend, i, slot);
        backend->force_slot_status(i, SlotStatus::AVAILABLE);
        backend->force_slot_remaining(i, i < 3 ? 80 : 0);
    }
    ams.sync_from_backend();
    ui_ams_mini_status_init();
    PanelWidgetHarness<AmsStatusWidget> h(test_screen(), HarnessConfig{{{"simplified", true}}});
    h.resize(4, 2, 500, 150);
    process_lvgl(30);
    CHECK(h.widget().has_edit_configure());
    int material_top = -1;
    for (int i = 0; i < 4; ++i) {
        auto find = [&](const char* prefix) {
            return h.child((std::string(prefix) + std::to_string(i)).c_str());
        };
        auto* cell = find("spool_cell_");
        auto* material = find("spool_material_");
        auto* percent = find("spool_pct_");
        auto* amount = find("spool_amount_");
        REQUIRE(cell != nullptr);
        REQUIRE(material != nullptr);
        REQUIRE(percent != nullptr);
        REQUIRE(amount != nullptr);
        auto* spool = lv_obj_find_by_name(cell, "lane_spool");
        auto* ring = lv_obj_find_by_name(cell, "spool_graphic");
        REQUIRE(spool != nullptr);
        REQUIRE(ring != nullptr);
        CHECK(ui::spool_canvas_get_view(ring) == ui::SpoolView::Ring);
        CHECK(lv_obj_get_width(ring) == 36);
        CHECK(ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(i < 3 ? i * 0.5f : 0.0f));
        CHECK(std::string(lv_label_get_text(percent)) == (i < 3 ? std::to_string(i * 50) + "%" : ""));
        CHECK(std::string(lv_label_get_text(amount)) ==
              (i < 3 ? std::to_string(i * 500) + " g · 80 m" : ""));
        lv_area_t ma, ra, aa;
        lv_obj_get_coords(material, &ma);
        lv_obj_get_coords(ring, &ra);
        lv_obj_get_coords(amount, &aa);
        if (material_top < 0)
            material_top = ma.y1;
        CHECK(ma.y1 == material_top); // Unknown percent keeps the material header band.
        CHECK(ma.y2 < ra.y1);
        if (i < 3)
            CHECK(ra.y2 < aa.y1);
    }
    // Changing the spool's remaining amount updates its ring and both readings.
    auto changed = backend->get_slot_info(0);
    changed.remaining_weight_g = 850;
    test::apply_edit(*backend, 0, changed);
    ams.sync_from_backend();
    process_lvgl(30);
    auto* first = h.child("spool_cell_0");
    CHECK(ui::ams_lane_spool_get_fill_level(lv_obj_find_by_name(first, "lane_spool")) == Catch::Approx(0.85f));
    CHECK(std::string(lv_label_get_text(h.child("spool_pct_0"))) == "85%");
    backend->stop();
    ams.clear_backends();
    HomeScreenPreferences(*config).set_spool_units(old_units);
}
