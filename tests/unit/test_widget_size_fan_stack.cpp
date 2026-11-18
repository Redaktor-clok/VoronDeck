// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Fan names follow the available width; compact grants retain single letters.
 * Glyph/readout sizes independently use measured row height and worst-case100%
 * width, so a narrow115x103 bank can show24px icons without enabling long names.
 * Tiny grants retain the native floor. Rows keep shared icon/name/value edges,
 * taller authored areas retain icon-above layout, and carousel mode is unchanged.
 */

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_portrait_layout.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "home_screen_preferences.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/fan_stack_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

TEST_CASE_METHOD(LVGLUITestFixture, "portrait fan percentages fit the granted strip after resizing",
                 "[fan_stack][home-fan-fit]") {
    const auto size = GENERATE(std::pair{480, 42}, std::pair{600, 52}, std::pair{800, 68});
    ScopedPortraitLayout portrait(size.first, size.first * 5 / 3);
    struct HomeFan : FanStackWidget {
        explicit HomeFan(PrinterState& state) : FanStackWidget("fan_stack", state) {
            set_panel_id("home");
        }
    };
    state().fan_state().init_fans(
        {"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"});
    state().fan_state().update_fan_speed("fan", 1.0);
    helix::ui::UpdateQueue::instance().drain();
    PanelWidgetHarness<HomeFan> h(test_screen(), state());
    REQUIRE(h.child("home_fan_bank"));
    for (int height : {size.second, 120, size.second}) {
        h.resize(2, 1, 282, height);
        process_lvgl(30);
        for (const char* role : {"part", "hotend", "aux"}) {
            const auto prefix = std::string("fan_stack_") + role;
            auto* row = h.child((prefix + "_row").c_str());
            auto* speed = h.child((prefix + "_speed").c_str());
            REQUIRE(row);
            REQUIRE(speed);
            CHECK(lv_obj_get_height(row) <= height);
            CHECK(lv_obj_get_y(speed) + lv_obj_get_height(speed) <= height);
            CHECK(lv_obj_get_width(speed) <= lv_obj_get_width(row));
        }
        CHECK(std::string(lv_label_get_text(h.child("fan_stack_part_speed"))) == "100%");
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan bank clicks recover the owning tile in both orientations",
                 "[fan_stack][fan-config]") {
    init_widget_registrations();
    const auto* component = GENERATE("panel_widget_fan_stack", "panel_widget_fan_portrait");
    FanStackWidget widget("fan_stack", state());
    auto* root = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), component, widget.xml_attrs()));
    REQUIRE(root != nullptr);
    widget.attach_tile(root, test_screen());
    auto callback = lv_xml_get_event_cb(nullptr, "on_fan_stack_clicked");
    REQUIRE(callback != nullptr);
    bool bound_on_root = false;
    for (uint32_t i = 0; i < lv_obj_get_event_count(root); ++i)
        bound_on_root |= lv_event_dsc_get_cb(lv_obj_get_event_dsc(root, i)) == callback;
    REQUIRE(bound_on_root);
    // Probe recovery without opening the global navigation overlay in this fixture.
    lv_obj_remove_event_cb(root, callback);
    FanStackWidget* recovered = nullptr;
    lv_obj_add_event_cb(root, [](lv_event_t* e) {
        *static_cast<FanStackWidget**>(lv_event_get_user_data(e)) =
            panel_widget_from_event<FanStackWidget>(e);
    }, LV_EVENT_CLICKED, &recovered);
    auto* icon = lv_obj_find_by_name(root, "fan_stack_part_icon");
    REQUIRE(icon != nullptr);
    lv_obj_send_event(icon, LV_EVENT_CLICKED, nullptr);
    CHECK(recovered == &widget);
    widget.detach_tile();
    lv_obj_delete(root);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "hidden Home fans retain an editable placeholder and refresh on return",
                 "[fan_stack][fan-config]") {
    const std::string component = GENERATE("panel_widget_fan_stack", "panel_widget_fan_carousel",
                                           "panel_widget_fan_portrait");
    auto* config = Config::get_instance();
    const std::string path = config->df() + "home_screen";
    const auto previous = config->get<nlohmann::json>(path, nlohmann::json::object());
    state().fan_state().init_fans(
        {"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"});
    HomeScreenPreferences preferences(*config);
    for (const auto* name : {"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"})
        preferences.set_visible(HomeScreenPreferences::DeviceKind::Fan, name, false);
    FanStackWidget widget("fan_stack", state());
    widget.set_panel_id("home");
    widget.set_config(
        {{"display_mode", component == "panel_widget_fan_carousel" ? "carousel" : "stack"}});
    auto* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), component.c_str(), widget.xml_attrs()));
    REQUIRE(root != nullptr);
    lv_obj_set_size(root, 200, 120);
    widget.attach_tile(root, test_screen());
    widget.notify_size_changed(2, 2, 200, 120);
    process_lvgl(30);
    auto* placeholder = lv_obj_find_by_name(root, "fan_hidden_placeholder");
    REQUIRE(placeholder != nullptr);
    CHECK_FALSE(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN));
    CHECK(widget.has_edit_configure());
    widget.on_edit_configure();
    REQUIRE(lv_obj_find_by_name(test_screen(), "fan_mode_carousel") != nullptr);
    REQUIRE(ui::ContextMenu::active() != nullptr);
    ui::ContextMenu::active()->hide();
    for (const auto* name : {"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"})
        CHECK_FALSE(preferences.is_visible(HomeScreenPreferences::DeviceKind::Fan, name));

    // Returning from Settings must re-read preferences even without a fan-version event.
    preferences.set_visible(HomeScreenPreferences::DeviceKind::Fan, "fan", true);
    widget.on_activate();
    process_lvgl(30);
    CHECK(lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN));
    if (component == "panel_widget_fan_carousel") {
        CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(root, "fan_carousel"), LV_OBJ_FLAG_HIDDEN));
    } else {
        CHECK_FALSE(
            lv_obj_has_flag(lv_obj_find_by_name(root, "fan_stack_part_row"), LV_OBJ_FLAG_HIDDEN));
    }
    CHECK_FALSE(
        preferences.is_visible(HomeScreenPreferences::DeviceKind::Fan, "heater_fan hotend_fan"));
    widget.detach_tile();
    lv_obj_delete(root);
    config->set<nlohmann::json>(path, previous);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "fan picker persists both modes and requests component replacement",
                 "[fan_stack][fan-config]") {
    auto& manager = PanelWidgetManager::instance();
    const std::string panel = "test_fan_mode_switch";
    auto& config = manager.get_widget_config(panel);
    REQUIRE(config.place_entry("fan_stack", 0, 0, 0, 2, 2) >= 0);
    FanStackWidget widget("fan_stack", state());
    widget.set_panel_id(panel);
    widget.set_config(nlohmann::json::object());
    lv_obj_t* root = nullptr;
    int rebuilt = 0;
    const auto create = [&]() {
        root = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), widget.get_component_name().c_str(), widget.xml_attrs()));
        REQUIRE(root != nullptr);
        lv_obj_set_size(root, 220, 150);
        widget.attach_tile(root, test_screen());
        widget.notify_size_changed(2, 2, 220, 150);
    };
    create();
    manager.register_rebuild_callback(panel, [&]() {
        widget.detach_tile();
        lv_obj_delete(root);
        widget.set_config(manager.get_widget_config(panel).get_widget_config("fan_stack"));
        create();
        ++rebuilt;
    });
    for (const bool carousel : {true, false, true}) {
        widget.on_edit_configure();
        auto* row =
            lv_obj_find_by_name(test_screen(), carousel ? "fan_mode_carousel" : "fan_mode_stack");
        REQUIRE(row != nullptr);
        const int before = rebuilt;
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        CHECK(rebuilt == before); // The callback does not destroy its own dispatch tree.
        process_lvgl(30);
        CHECK(rebuilt == before + 1);
        const auto stored = manager.get_widget_config(panel).get_widget_config("fan_stack");
        CHECK((stored.value("display_mode", "stack") == "carousel") == carousel);
        CHECK((lv_obj_find_by_name(root, "fan_carousel") != nullptr) == carousel);
        CHECK((lv_obj_find_by_name(root, "fan_stack_part_row") != nullptr) != carousel);
    }
    manager.unregister_rebuild_callback(panel);
    widget.detach_tile();
    lv_obj_delete(root);
    manager.clear_panel_config(panel);
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack labels/names follow pixels, not spans",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());

    lv_obj_t* part_speed = h.child("fan_stack_part_speed");
    lv_obj_t* hotend_speed = h.child("fan_stack_hotend_speed");
    lv_obj_t* aux_speed = h.child("fan_stack_aux_speed");
    REQUIRE(part_speed != nullptr);
    REQUIRE(hotend_speed != nullptr);
    REQUIRE(aux_speed != nullptr);

    lv_obj_t* part_name = h.child("fan_stack_part_name");
    lv_obj_t* hotend_name = h.child("fan_stack_hotend_name");
    lv_obj_t* aux_name = h.child("fan_stack_aux_name");
    REQUIRE(part_name != nullptr);
    REQUIRE(hotend_name != nullptr);
    REQUIRE(aux_name != nullptr);

    lv_obj_t* part_icon = h.child("fan_stack_part_icon");
    lv_obj_t* hotend_icon = h.child("fan_stack_hotend_icon");
    lv_obj_t* aux_icon = h.child("fan_stack_aux_icon");
    REQUIRE(part_icon != nullptr);
    REQUIRE(hotend_icon != nullptr);
    REQUIRE(aux_icon != nullptr);

    lv_obj_t* part_row = h.child("fan_stack_part_row");
    REQUIRE(part_row != nullptr);

    // No fans are discovered under this fixture (PrinterState starts with
    // an empty fan list), so bind_fans() never populates the display-name
    // strings and on_size_changed falls back to the hardcoded English
    // names. That fallback still differs from the compact single letter,
    // so it is a valid target for the "bigger" text assertion.
    const char* part_bigger_text = lv_tr("Part");
    const char* hotend_bigger_text = lv_tr("Hotend");
    const char* aux_bigger_text = lv_tr("Aux");

    // --- Neither flag: large span, sub-threshold pixels on both axes. ---
    // Compact names remain letters, but the measured grant now fits body text
    // and24px icons; it must not inherit the old width-only16px glyph floor.
    //
    // rowspan stays at 2 (one authored cell) so the icon-above layout is out
    // of play Р Р†Р вЂљРІР‚Сњ that one IS span-gated, deliberately, and promotes the text to
    // font_body. colspan carries the span-contradiction this case is about.
    h.resize(4, 2, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_get_style_text_font(hotend_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_get_style_text_font(aux_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
    CHECK(std::string(lv_label_get_text(hotend_name)) == lv_tr("H"));
    CHECK(std::string(lv_label_get_text(aux_name)) == lv_tr("A"));

    CHECK(lv_obj_get_style_text_font(part_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(hotend_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(aux_icon, LV_PART_MAIN) == &mdi_icons_24);

    // Compact: rows level to the widest and left-align content within the
    // equal-width block, same as the bigger form Р Р†Р вЂљРІР‚Сњ per-row centering is what
    // this used to assert, and it is exactly what made the icons ragged.
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

    // --- Width alone: at the width threshold, height still sub-threshold. ---
    h.resize(1, 1, w_normal(), h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));
    CHECK(lv_obj_get_style_text_font(hotend_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));
    CHECK(lv_obj_get_style_text_font(aux_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_small"));
    CHECK(std::string(lv_label_get_text(part_name)) == part_bigger_text);
    CHECK(std::string(lv_label_get_text(hotend_name)) == hotend_bigger_text);
    CHECK(std::string(lv_label_get_text(aux_name)) == aux_bigger_text);

    CHECK(lv_obj_get_style_text_font(part_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(hotend_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(aux_icon, LV_PART_MAIN) == &mdi_icons_24);

    // Bigger: row switches to LV_FLEX_ALIGN_START (left-aligned content,
    // widened to the widest row) instead of compact's CENTER. Unlike a row
    // width comparison this does not depend on how wide the grant is or how
    // long the resolved fan name is Р Р†Р вЂљРІР‚Сњ it is set unconditionally per branch
    // (see file header for why a width-based assertion was dropped).
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

    // --- Height alone, at a real Large-tier 1x1 extent (107x141, from the
    // measured tier table): names remain compact while glyphs/readouts fill the grant.
    // Before this fix, height_px(141) >= h_tall() alone flipped this to
    // "bigger" and squeezed "Hotend"/"Aux" into a 107px-wide row.
    // Contradicting span: 1x1 (old rowspan/colspan predicate -> compact, so
    // this also proves pixels don't silently regress to spans).
    h.resize(1, 1, 107, 141);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_get_style_text_font(hotend_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_get_style_text_font(aux_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
    CHECK(std::string(lv_label_get_text(hotend_name)) == lv_tr("H"));
    CHECK(std::string(lv_label_get_text(aux_name)) == lv_tr("A"));

    CHECK(lv_obj_get_style_text_font(part_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(hotend_icon, LV_PART_MAIN) == &mdi_icons_24);
    CHECK(lv_obj_get_style_text_font(aux_icon, LV_PART_MAIN) == &mdi_icons_24);

    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

    // --- XLarge-tier 1x1 (134x169): same story, taller still.
    h.resize(1, 1, 134, 169);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
    CHECK(lv_obj_get_style_flex_main_place(part_row, LV_PART_MAIN) == LV_FLEX_ALIGN_START);

    // --- Medium-tier 1x1 (114x112): both axes below their floor anyway Р Р†Р вЂљРІР‚Сњ
    // never promoted the old way either, kept here as a same-shape baseline.
    h.resize(1, 1, 114, 112);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));

    // --- Micro-tier 1x2 (70x131): the legitimate tall case. Narrower than
    // any of the Large/XLarge false positives above, and genuinely
    // rowspan==2 Р Р†Р вЂљРІР‚Сњ but this widget's "bigger" is width-only now, so a
    // colspan==1 widget stays compact here regardless of row count. That is
    // a real behavior change from the pre-fix OR (which showed resolved
    // names in this same 70px row), traded deliberately: full names never
    // fit a 70px row named/iconed/valued in three parts, tall or not.
    h.resize(1, 2, 70, 131);
    process_lvgl(30);

    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) == theme_manager_get_font("font_xs"));
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));
}

/**
 * The user-facing property the row leveling exists for: all three fan icons
 * sit at the same x, at every tier, even when the rows hold different-width
 * text. The setup gives the fans deliberately mismatched speeds before the
 * first resize (part "35%", the rest "0%") so the rows genuinely differ in
 * width Р Р†Р вЂљРІР‚Сњ under the old per-row centering the part icon sat measurably right
 * of the others, which is the red this case was verified against.
 *
 * The second half pins the speed-label reserve: rows are leveled once at
 * on_size_changed while speed text keeps changing afterwards, so every speed
 * label reserves the width of the widest string the formatter can emit
 * ("100%"). Without it, a fan ramping from "0%" to "100%" after layout
 * would outgrow its fixed row and clip Р Р†Р вЂљРІР‚Сњ the same defect shape the
 * fit-the-cell case below guards against at the tier band.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack icons line up one under the other",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    // Real fans so all three rows stay visible and the roles resolve. The
    // bare `fan` object maps to "Part Cooling Fan" (device_display_name.cpp
    // kDirectMappings) Р Р†Р вЂљРІР‚Сњ same trio as the fit-the-cell case below.
    state().fan_state().init_fans(
        {"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"});
    state().fan_state().update_fan_speed("fan", 0.35);
    helix::ui::UpdateQueue::instance().drain();

    PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());

    lv_obj_t* part_icon = h.child("fan_stack_part_icon");
    lv_obj_t* hotend_icon = h.child("fan_stack_hotend_icon");
    lv_obj_t* aux_icon = h.child("fan_stack_aux_icon");
    REQUIRE(part_icon != nullptr);
    REQUIRE(hotend_icon != nullptr);
    REQUIRE(aux_icon != nullptr);
    REQUIRE(!lv_obj_has_flag(h.child("fan_stack_aux_row"), LV_OBJ_FLAG_HIDDEN));

    // --- Compact tier: one shared left edge despite "35%" vs "0%" rows. ---
    h.resize(1, 1, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(hotend_icon));
    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(aux_icon));

    // Speeds justify against the shared right edge: right-aligned inside
    // their reserved boxes, so a "0%" row ends where the "100%" row does
    // and the leveled block reads as justified, not left-packed.
    lv_obj_t* part_speed = h.child("fan_stack_part_speed");
    lv_obj_t* hotend_speed = h.child("fan_stack_hotend_speed");
    REQUIRE(part_speed != nullptr);
    CHECK(lv_obj_get_style_text_align(part_speed, LV_PART_MAIN) == LV_TEXT_ALIGN_RIGHT);
    // The name column is leveled too, so the speed boxes share one x (and
    // with equal reserved widths, one right edge) Р Р†Р вЂљРІР‚Сњ without it the P/H/C
    // advance-width differences rag the speed column by a pixel or two.
    REQUIRE(hotend_speed != nullptr);
    CHECK(lv_obj_get_x(part_speed) == lv_obj_get_x(hotend_speed));

    // --- Speed ramp after layout: "0%" -> "100%" must fit the fixed row. ---
    state().fan_state().update_fan_speed("heater_fan hotend_fan", 1.0);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(30);

    lv_obj_t* hotend_row = h.child("fan_stack_hotend_row");
    REQUIRE(hotend_row != nullptr);
    CHECK(lv_obj_get_x(hotend_speed) + lv_obj_get_width(hotend_speed) <=
          lv_obj_get_width(hotend_row));

    // --- Bigger tier: same shared edge with resolved display names. ---
    h.resize(1, 1, w_normal(), h_tall() - 1);
    process_lvgl(30);

    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(hotend_icon));
    CHECK(lv_obj_get_x(part_icon) == lv_obj_get_x(aux_icon));
}

/**
 * Carousel mode takes the early return at the top of on_size_changed
 * (`!widget_obj_ || is_carousel_mode()`), below which Task 4's pixel
 * predicate lives. This proves two things: the carousel component is
 * genuinely what gets built (not the stack component silently substituted
 * by a harness that forgot set_config()), and driving resize() across the
 * same w_normal()/h_tall() threshold used above never reaches that predicate.
 *
 * `fan_stack_part_name`/`fan_stack_part_speed`/`fan_stack_part_row` (the
 * objects the stack-mode test above asserts on) do not exist anywhere in
 * `panel_widget_fan_carousel.xml` or the `fan_arc_core` component it embeds
 * Р Р†Р вЂљРІР‚Сњ confirmed below by asserting the lookups miss. So the strong per-object
 * "this label's font moved" check from the stack test is not available
 * here; there is nothing named that to check. What carousel mode DOES have,
 * from bind_carousel_fans() (fan_stack_widget.cpp), is a `speed_label`
 * (text_heading) and `fan_icon` per carousel page, explicitly given
 * "font_xs" at construction time Р Р†Р вЂљРІР‚Сњ those are the closest carousel
 * equivalent of the stack test's font assertions, so this test pins them
 * instead.
 *
 * Mutation testing this against `is_carousel_mode()` removed from the guard
 * (leaving only `!widget_obj_`) does NOT turn this test red: every branch in
 * on_size_changed's body is gated behind either a `fan_stack_*` name lookup
 * (all miss, per the CHECKs above) or a `part_label_`/`hotend_label_`/
 * `aux_label_`/`part_icon_`/`hotend_icon_`/`aux_icon_` member pointer, none
 * of which attach_carousel() ever populates Р Р†Р вЂљРІР‚Сњ those stay their default
 * nullptr for a carousel-mode widget. So the whole function body is
 * unreachable from a carousel-built widget regardless of the guard, and
 * `is_carousel_mode()` is defensive rather than load-bearing *as far as this
 * test's own observables go* Р Р†Р вЂљРІР‚Сњ it does not prove no other path could ever
 * depend on it. Recorded here rather than papering over it with an
 * assertion that happens to fail.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack carousel mode ignores the pixel size predicate",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    PanelWidgetHarness<FanStackWidget> h(
        test_screen(), HarnessConfig{{{"display_mode", "carousel"}}}, "fan_stack", state());

    // Proves the carousel component was actually built, not the stack one
    // (see harness's own test for what a missing HarnessConfig would do).
    REQUIRE(h.child("fan_carousel") != nullptr);

    // The stack-only names on_size_changed's pixel predicate would move
    // simply do not exist under the carousel component.
    CHECK(h.child("fan_stack_part_name") == nullptr);
    CHECK(h.child("fan_stack_part_speed") == nullptr);
    CHECK(h.child("fan_stack_part_row") == nullptr);

    // Placeholder fan entries (no fans discovered under this fixture) still
    // produce a carousel page with a real speed_label/fan_icon pair.
    lv_obj_t* speed_label = h.child("speed_label");
    lv_obj_t* fan_icon = h.child("fan_icon");
    REQUIRE(speed_label != nullptr);
    REQUIRE(fan_icon != nullptr);

    const lv_font_t* xs_font = theme_manager_get_font("font_xs");
    const lv_font_t* speed_font_before = lv_obj_get_style_text_font(speed_label, LV_PART_MAIN);
    std::string speed_text_before = lv_label_get_text(speed_label);
    CHECK(speed_font_before == xs_font);

    // Drive resize() past the threshold in both directions Р Р†Р вЂљРІР‚Сњ the same
    // stimuli that flip the stack test's labels to font_small/full names.
    h.resize(4, 4, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);
    CHECK(lv_obj_get_style_text_font(speed_label, LV_PART_MAIN) == speed_font_before);
    CHECK(std::string(lv_label_get_text(speed_label)) == speed_text_before);

    h.resize(1, 1, w_normal(), h_tall() - 1);
    process_lvgl(30);
    CHECK(lv_obj_get_style_text_font(speed_label, LV_PART_MAIN) == speed_font_before);
    CHECK(std::string(lv_label_get_text(speed_label)) == speed_text_before);

    h.resize(1, 1, w_normal() - 1, h_tall());
    process_lvgl(30);
    CHECK(lv_obj_get_style_text_font(speed_label, LV_PART_MAIN) == speed_font_before);
    CHECK(std::string(lv_label_get_text(speed_label)) == speed_text_before);
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan carousel fits a low portrait strip and a narrow tall tile",
                 "[widget_size][fan_stack]") {
    PanelWidgetHarness<FanStackWidget> h(
        test_screen(), HarnessConfig{{{"display_mode", "carousel"}}}, "fan_stack", state());
    for (const auto [w, height] : {std::pair{228, 56}, std::pair{115, 155}, std::pair{228, 56}}) {
        h.resize(2, 2, w, height);
        process_lvgl(40);
        auto* core = h.child("dial_container");
        auto* arc = h.child("dial_arc");
        auto* label = h.child("fan_carousel_name");
        auto* center = h.child("center_content");
        REQUIRE(core);
        REQUIRE(arc);
        REQUIRE(label);
        REQUIRE(center);
        lv_area_t bounds{}, dial{}, name{}, content{};
        lv_obj_get_coords(h.root(), &bounds);
        lv_obj_get_coords(arc, &dial);
        lv_obj_get_coords(label, &name);
        lv_obj_get_coords(center, &content);
        CHECK(dial.x1 >= bounds.x1);
        CHECK(dial.x2 <= bounds.x2);
        CHECK(dial.y1 >= bounds.y1);
        CHECK(dial.y2 <= bounds.y2);
        CHECK(content.y1 >= bounds.y1);
        CHECK(content.y2 <= bounds.y2);
        CHECK(name.x1 >= bounds.x1);
        CHECK(name.x2 <= bounds.x2);
        CHECK(name.y2 <= bounds.y2);
        CHECK(lv_obj_get_width(arc) >= 24);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "fan icon selection is applied on load and while the picker stays open",
                 "[fan_stack][fan-config]") {
    const bool carousel = GENERATE(false, true);
    PanelWidgetHarness<FanStackWidget> h(test_screen(),
        HarnessConfig{{{"display_mode", carousel ? "carousel" : "stack"}, {"icon", "fan_off"}}},
        "fan_stack", state());
    auto* icon = h.child(carousel ? "fan_icon" : "fan_stack_part_icon");
    REQUIRE(icon);
    CHECK(std::string(lv_label_get_text(icon)) == helix::ui::icon::lookup_codepoint("fan_off"));
    h.widget().on_edit_configure();
    auto* grid = lv_obj_find_by_name(test_screen(), "icon_grid");
    REQUIRE(grid);
    REQUIRE(lv_obj_get_child_count(grid) == 4);
    lv_obj_send_event(lv_obj_get_child(grid, 0), LV_EVENT_CLICKED, nullptr);
    CHECK(std::string(lv_label_get_text(icon)) == helix::ui::icon::lookup_codepoint("fan"));
    REQUIRE(ui::ContextMenu::active());
    ui::ContextMenu::active()->hide();
}

/**
 * The threshold assertions above are threshold-relative: they drive the
 * harness from the band itself (`W_NORMAL - 1`, `W_NORMAL`) and check that the
 * layout flipped. That proves the predicate is wired to the band. It cannot
 * prove the band is calibrated, because both sides of the comparison move
 * together Р Р†Р вЂљРІР‚Сњ which is why they stayed green through the defect this case is
 * about.
 *
 * The defect: the band was a flat 135px, measured against the Small tier's
 * 14px font_body. On the roomier tiers font_body is 18/20/24/32px, so a cell
 * that clears 135px there buys far fewer glyphs than the same 135px bought at
 * Small. `bigger` fired anyway, the rows switched to LV_SIZE_CONTENT with
 * resolved fan names in the tier's larger font, and the resulting row was
 * WIDER than the cell it was drawn into Р Р†Р вЂљРІР‚Сњ "Part Cooling Fan" clipped where the
 * compact layout would have drawn "P".
 *
 * So this case asserts the property that actually matters and that no
 * threshold-relative check can express: whatever layout the predicate picks,
 * the row it produces has to fit inside the width the widget was granted.
 * Each case names a real tier, moves the display there (which is what moves
 * the font ladder Р Р†Р вЂљРІР‚Сњ theme_manager_refresh_layout_constants), and grants a
 * width inside the window where the flat band promoted and the tier's own
 * band does not.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack rows fit the cell they were granted",
                 "[widget_size][fan_stack]") {
    require_font_tokens_distinct();

    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    // Real discovered fans, so the "bigger" branch draws the resolved display
    // names it draws on a printer rather than the short hardcoded fallbacks.
    // The bare Klipper `fan` object maps to "Part Cooling Fan"
    // (device_display_name.cpp kDirectMappings) Р Р†Р вЂљРІР‚Сњ the exact string the defect
    // report showed clipped.
    state().fan_state().init_fans(
        {"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"});
    helix::ui::UpdateQueue::instance().drain();

    struct FitCase {
        const char* name;
        int32_t hor;
        int32_t ver;
        int width_px;
        int height_px;
    };

    // Widths sit above the flat 135px band and below the tier's own band, the
    // exact window the flat band got wrong. Heights are a real single-row
    // extent for the tier so nothing else about the case is artificial.
    const FitCase cases[] = {
        {"medium 800x480", 800, 480, 136, 112},
        {"large 1024x600", 1024, 600, 160, 141},
        {"xxlarge 1080x1920", 1080, 1920, 211, 200},
    };

    for (const auto& c : cases) {
        ScopedResolution res(disp, c.hor, c.ver);
        theme_manager_refresh_layout_constants(disp);

        PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());
        lv_obj_t* part_row = h.child("fan_stack_part_row");
        REQUIRE(part_row != nullptr);

        h.resize(1, 1, c.width_px, c.height_px);
        process_lvgl(30);

        // on_size_changed levels all three rows to the widest, so any one of
        // them reports the overflow; check every row rather than assuming
        // which name resolved longest on this tier.
        const int cell_w = lv_obj_get_content_width(h.root());
        for (const char* rn : {"fan_stack_part_row", "fan_stack_hotend_row", "fan_stack_aux_row"}) {
            lv_obj_t* row = h.child(rn);
            REQUIRE(row != nullptr);
            if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN))
                continue;
            INFO(c.name << ": granted " << c.width_px << "px, cell content " << cell_w << "px, "
                        << rn << " " << lv_obj_get_width(row) << "px, band " << w_normal()
                        << ", part name '" << lv_label_get_text(h.child("fan_stack_part_name"))
                        << "'");
            CHECK(lv_obj_get_width(row) <= cell_w);
        }
    }

    // Put the token table back where the rest of the suite expects it.
    theme_manager_refresh_layout_constants(disp);
}

/**
 * The icon-above layout (stacked_row_layout.h), the fan_stack twin of the
 * temp_stack case in test_widget_size_temp_stack.cpp.
 *
 * Two things are specific to fan_stack and asserted here rather than there.
 * The name and speed must stay on ONE line under the glyph Р Р†Р вЂљРІР‚Сњ that is what the
 * `fan_stack_*_value` wrapper in panel_widget_fan_stack.xml exists for, and a
 * row flipped to a column without it would break them onto separate lines.
 * And the name gets a MEASURED ladder here rather than the width band: a
 * stacked row hands the name+speed pair the widget's whole content width, so
 * the band (which answers "does a resolved name fit BESIDE the glyph") is
 * calibrated for the wrong layout. Both ends of that ladder are pinned Р Р†Р вЂљРІР‚Сњ a
 * sub-band column that still fits a word gets one, a column that fits nothing
 * falls back to the single letter.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "fan_stack stacks icons above name+speed only when it fits",
                 "[widget_size][fan_stack][panel_widget]") {
    PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());

    lv_obj_t* part_row = h.child("fan_stack_part_row");
    lv_obj_t* part_icon = h.child("fan_stack_part_icon");
    lv_obj_t* part_value = h.child("fan_stack_part_value");
    lv_obj_t* part_name = h.child("fan_stack_part_name");
    lv_obj_t* part_speed = h.child("fan_stack_part_speed");
    REQUIRE(part_row != nullptr);
    REQUIRE(part_icon != nullptr);
    REQUIRE(part_value != nullptr);
    REQUIRE(part_name != nullptr);
    REQUIRE(part_speed != nullptr);

    // The name and speed share one inner row in both layouts. Assert it here
    // once: everything below reasons about `part_value` as a single block.
    REQUIRE(lv_obj_get_parent(part_name) == part_value);
    REQUIRE(lv_obj_get_parent(part_speed) == part_value);
    REQUIRE(lv_obj_get_parent(part_value) == part_row);

    auto name_and_speed_share_a_line = [&]() {
        // Vertical overlap, and the name to the left of the speed.
        return lv_obj_get_y(part_name) < lv_obj_get_y(part_speed) + lv_obj_get_height(part_speed) &&
               lv_obj_get_y(part_speed) < lv_obj_get_y(part_name) + lv_obj_get_height(part_name) &&
               lv_obj_get_x(part_name) < lv_obj_get_x(part_speed);
    };

    // One authored cell: compact rows, however tall the cell is.
    h.resize(2, 2, w_normal() - 1, 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_x(part_icon) + lv_obj_get_width(part_icon) <= lv_obj_get_x(part_value));
    CHECK(name_and_speed_share_a_line());

    // Taller than a cell, with room: glyph moves above the pair, text goes to
    // font_body Р Р†Р вЂљРІР‚Сњ a rung past the font_small that the width band alone buys.
    h.resize(2, 4, w_normal() - 1, 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_get_y(part_icon) + lv_obj_get_height(part_icon) <= lv_obj_get_y(part_value));
    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_get_style_text_font(part_name, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));

    // ...and the pair is still one line, not two.
    CHECK(name_and_speed_share_a_line());

    // A stacked row puts the name on its own line, so the glyph is no longer
    // competing for that width and a longer form fits than the width band
    // would allow beside it. This column is BELOW w_normal() Р Р†Р вЂљРІР‚Сњ the band alone
    // would force the single letter Р Р†Р вЂљРІР‚Сњ yet the name is a word.
    CHECK(std::string(lv_label_get_text(part_name)) != lv_tr("P"));

    // The longer form is measured, not assumed: a column with no room for any
    // word still falls back to the single letter rather than overflowing.
    h.resize(2, 4, 40, 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(std::string(lv_label_get_text(part_name)) == lv_tr("P"));

    // Wide AND tall: the name still shares its line with the speed under the
    // glyph.
    h.resize(4, 4, w_normal(), 400);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(std::string(lv_label_get_text(part_name)) != lv_tr("P"));
    CHECK(name_and_speed_share_a_line());

    // Every visible row uses the SAME form, at every width. Picking each row's
    // own longest-that-fits would render "Part / Hotend / C", which reads as a
    // rendering fault rather than a deliberate abbreviation. "Hotend" is the
    // longer of the two visible short words, so the pair disagreeing is
    // exactly the failure this sweeps for.
    lv_obj_t* hotend_name = h.child("fan_stack_hotend_name");
    REQUIRE(hotend_name != nullptr);
    for (int w = 30; w <= w_normal() + 60; w += 7) {
        h.resize(2, 4, w, 400);
        process_lvgl(30);
        const bool part_is_letter = std::string(lv_label_get_text(part_name)) == lv_tr("P");
        const bool hotend_is_letter = std::string(lv_label_get_text(hotend_name)) == lv_tr("H");
        INFO("width " << w << ": part '" << lv_label_get_text(part_name) << "', hotend '"
                      << lv_label_get_text(hotend_name) << "'");
        CHECK(part_is_letter == hotend_is_letter);
    }

    // Taller than a cell but with nowhere to put the second line: compact
    // rows, fully restored rather than left half-converted.
    h.resize(2, 4, w_normal() - 1, 40);
    process_lvgl(30);
    CHECK(lv_obj_get_style_flex_flow(part_row, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_x(part_icon) + lv_obj_get_width(part_icon) <= lv_obj_get_x(part_value));
    CHECK(lv_obj_get_style_text_font(part_speed, LV_PART_MAIN) ==
          theme_manager_get_font("font_xs"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "compact Home fans use available height and retain captions",
                 "[fan_stack][home-fan-fit]") {
    state().fan_state().init_fans(
        {"fan", "heater_fan hotend_fan", "fan_generic chamber_circulation"});
    state().fan_state().update_fan_speed("fan", 1.0);
    helix::ui::UpdateQueue::instance().drain();
    PanelWidgetHarness<FanStackWidget> h(test_screen(), "fan_stack", state());
    h.resize(2, 2, 115, 103);
    process_lvgl(30);
    for (const char* role : {"part", "hotend", "aux"}) {
        const std::string prefix = std::string("fan_stack_") + role;
        auto* icon = h.child((prefix + "_icon").c_str());
        auto* speed = h.child((prefix + "_speed").c_str());
        auto* row = h.child((prefix + "_row").c_str());
        REQUIRE(icon);
        REQUIRE(speed);
        REQUIRE(row);
        CHECK(lv_obj_get_style_text_font(icon, LV_PART_MAIN) == &mdi_icons_24);
        CHECK(lv_obj_get_style_text_font(speed, LV_PART_MAIN)->line_height >
              theme_manager_get_font("font_xs")->line_height);
        CHECK(lv_obj_get_width(row) <= lv_obj_get_content_width(h.root()));
        CHECK(lv_obj_get_y(row) + lv_obj_get_height(row) <= lv_obj_get_height(h.root()));
    }
    auto* portrait =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_fan_portrait", nullptr));
    REQUIRE(portrait);
    for (const auto& pair :
         {std::pair{"part", "P"}, std::pair{"hotend", "H"}, std::pair{"aux", "C"}}) {
        const auto name = std::string("fan_stack_") + pair.first + "_name";
        auto* label = lv_obj_find_by_name(portrait, name.c_str());
        REQUIRE(label);
        CHECK(std::string(lv_label_get_text(label)) == pair.second);
    }
    lv_obj_delete(portrait);
}
