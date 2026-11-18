// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "A tiny spool survives construction before selecting Ring",
                 "[spool-responsive][spool_canvas][ring]") {
    for (int edge : {1, 2, 3}) {
        // Creation renders the default Side view before a caller can set Ring.
        // Rounded ellipse radii are zero at these valid positive canvas sizes.
        auto* canvas = ui_spool_canvas_create(test_screen(), edge);
        REQUIRE(canvas != nullptr);
        helix::ui::spool_canvas_set_view(canvas, helix::ui::SpoolView::Ring);
        ui_spool_canvas_set_fill_level(canvas, 0.5f);
        CHECK(ui_spool_canvas_get_fill_level(canvas) == Catch::Approx(0.5f));
        lv_obj_delete(canvas);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Home spool ring consumes area and preserves its hub",
                 "[vorondeck][spool_canvas][ring]") {
    const char* attrs[] = {"size", "100", "view", "ring", "color", "0xFF0000", nullptr};
    auto* canvas = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spool_canvas", attrs));
    REQUIRE(canvas != nullptr);
    REQUIRE(helix::ui::spool_canvas_get_view(canvas) == helix::ui::SpoolView::Ring);
    auto red_area = [&] {
        const auto* buf = lv_canvas_get_draw_buf(canvas);
        REQUIRE(buf != nullptr);
        int count = 0;
        for (uint32_t y = 0; y < buf->header.h; ++y) {
            const auto* row =
                reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride);
            for (uint32_t x = 0; x < buf->header.w; ++x)
                if (row[x].alpha > 200 && row[x].red > row[x].green + 100)
                    ++count;
        }
        const auto* middle =
            reinterpret_cast<const lv_color32_t*>(buf->data + 50 * buf->header.stride);
        CHECK(middle[50].alpha == 0);
        return count;
    };
    ui_spool_canvas_set_fill_level(canvas, 1.0f);
    const int full = red_area();
    REQUIRE(full > 3000);
    ui_spool_canvas_set_fill_level(canvas, 0.5f);
    CHECK(red_area() == Catch::Approx(full * 0.5).margin(full * 0.04));
    ui_spool_canvas_set_fill_level(canvas, 0.0f);
    CHECK(red_area() == 0);
    ui_spool_canvas_set_fill_level(canvas, 1.0f);
    CHECK(red_area() == full); // A cached full ring must not retain the empty state.
}
