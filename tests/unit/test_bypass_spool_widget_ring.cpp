// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_bypass_spool_widget_ring.cpp
 * @brief The bypass node wears the same active ring a lane slot does.
 *
 * A lane slot gets a primary border plus outer glow while it is the node
 * feeding the toolhead (apply_current_slot_highlight). Bypass is a node on the
 * same path and feeds the toolhead the same way, but had no such marker - so an
 * engaged bypass looked identical to an idle one, and the only clue was the
 * filament panel's own controls.
 *
 * The styling lives in ams_draw::set_active_ring() precisely so the two cannot
 * drift; these tests pin the widget's use of it, including the change-detection
 * pair that keeps panel refreshes from restyling on every tick.
 */

#include "ui_bypass_spool_widget.h"
#include "ui_spool_canvas.h"
#include "ams_types.h"

#include "../test_fixtures.h"
#include "ui/ams_drawing_utils.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "bypass face-on ring preserves actual remaining across presence updates",
                 "[ams][bypass][spool_widget][remaining-rings]") {
    auto widget = helix::ui::bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(widget.valid());
    CHECK(helix::ui::spool_canvas_get_view(widget.spool_canvas) == helix::ui::SpoolView::Ring);
    helix::SlotInfo slot;
    slot.color_rgb = 0xE9434B;
    slot.total_weight_g = 1000;
    for (float grams : {0.0f, 500.0f, 1000.0f, -1.0f}) {
        slot.remaining_weight_g = grams;
        helix::ui::bypass_spool_set_filament(widget, slot);
        helix::ui::bypass_spool_set_has_spool(widget, true);
        CHECK(ui_spool_canvas_get_fill_level(widget.spool_canvas) ==
              Catch::Approx(grams < 0 ? 0 : grams / 1000));
        CHECK(lv_color_eq(ui_spool_canvas_get_color(widget.spool_canvas), lv_color_hex(slot.color_rgb)));
    }
    helix::ui::bypass_spool_set_has_spool(widget, false);
    CHECK(ui_spool_canvas_get_fill_level(widget.spool_canvas) == 0.0f);
    helix::ui::bypass_spool_destroy(widget);
}

using helix::ui::bypass_spool_create;
using helix::ui::bypass_spool_set_active;
using helix::ui::BypassSpoolWidgets;

TEST_CASE_METHOD(LVGLTestFixture, "bypass captions stay below the ring and clear of hub controls",
                 "[ams][bypass][spool_widget][caption-layout]") {
    auto* parent = lv_obj_create(lv_screen_active());
    lv_obj_set_style_pad_all(parent, 0, 0);
    auto w = bypass_spool_create(parent, nullptr, nullptr);
    for (int width : {220, 430}) {
        lv_obj_set_size(parent, width, 220);
        lv_obj_update_layout(parent);
        for (const char* material : {"PLA+", "A very long custom filament material", ""}) {
            helix::ui::bypass_spool_set_material(w, material);
            helix::ui::bypass_spool_set_position(w, width - 30, 140);
            lv_obj_update_layout(parent);
            lv_area_t ring, caption, mat, bounds;
            lv_obj_get_coords(w.box, &ring);
            lv_obj_get_coords(w.bypass_label, &caption);
            lv_obj_get_coords(w.material_label, &mat);
            lv_obj_get_content_coords(parent, &bounds);
            CHECK(caption.y1 > ring.y2);
            CHECK(caption.x2 <= bounds.x2);
            CHECK(caption.y2 <= bounds.y2);
            if (*material) {
                CHECK(mat.y1 > ring.y2);
                CHECK(mat.x1 >= bounds.x1);
                CHECK(mat.x2 < caption.x1);
                CHECK(mat.y1 == caption.y1);
            }
        }
    }
    helix::ui::bypass_spool_destroy(w);
    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture, "bypass node draws the active ring when engaged",
                 "[ams][bypass][spool_widget]") {
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());

    bypass_spool_set_active(w, true);

    CHECK(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingBorderWidth);
    CHECK(lv_obj_get_style_border_opa(w.box, LV_PART_MAIN) == LV_OPA_COVER);
    // The glow is what makes it read as "active" rather than merely outlined.
    CHECK(lv_obj_get_style_shadow_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingGlowWidth);
    CHECK(lv_obj_get_style_shadow_spread(w.box, LV_PART_MAIN) == ams_draw::kActiveRingGlowSpread);

    helix::ui::bypass_spool_destroy(w);
}

TEST_CASE_METHOD(LVGLTestFixture, "bypass node clears the ring when disengaged",
                 "[ams][bypass][spool_widget]") {
    // Disengaging must actually remove it. A ring left behind says the printer
    // is feeding from bypass when it is not - worse than never drawing one.
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());

    bypass_spool_set_active(w, true);
    REQUIRE(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingBorderWidth);

    bypass_spool_set_active(w, false);

    CHECK(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_shadow_width(w.box, LV_PART_MAIN) == 0);

    helix::ui::bypass_spool_destroy(w);
}

TEST_CASE_METHOD(LVGLTestFixture, "bypass ring applies on the first call even when inactive",
                 "[ams][bypass][spool_widget]") {
    // The same trap the colour guard fell into: "off" is the natural default,
    // so a bare cached_active could not tell "never applied" from "already
    // off". active_applied is the bit that separates them.
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());
    REQUIRE_FALSE(w.active_applied);

    bypass_spool_set_active(w, false);
    CHECK(w.active_applied);
    CHECK_FALSE(w.cached_active);

    // And the guard still dedups once applied.
    bypass_spool_set_active(w, true);
    CHECK(w.cached_active);
    CHECK(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingBorderWidth);

    helix::ui::bypass_spool_destroy(w);
}
