// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "cjk_font_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "nemesis_fonts.h"
#include "theme_manager.h"
#include "ui_fonts.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include <algorithm>
#include <array>

#include "../catch_amalgamated.hpp"

class NemesisFontFixture : public LVGLTestFixture {
  public:
    NemesisFontFixture() {
        theme_manager_apply_font_family(false);
    }
    ~NemesisFontFixture() override {
        helix::system::CjkFontManager::instance().shutdown();
        theme_manager_apply_font_family(false);
    }
};

TEST_CASE_METHOD(NemesisFontFixture, "NEMESIS changes existing and new text but preserves icons",
                 "[nemesis_font]") {
    const auto original = noto_sans_20;
    const auto icon_data = mdi_icons_24.dsc;
    const auto mono_data = source_code_pro_14.dsc;
    auto* existing = lv_label_create(test_screen());
    lv_obj_set_style_text_font(existing, &noto_sans_20, 0);
    lv_label_set_text(existing, "NEMESIS 530 CNC");
    lv_obj_update_layout(existing);
    const auto original_width = lv_obj_get_width(existing);

    theme_manager_apply_font_family(true);
    lv_obj_update_layout(existing);
    CHECK(lv_obj_get_style_text_font(existing, LV_PART_MAIN) == &noto_sans_20);
    CHECK(noto_sans_20.dsc == orbitron_20.dsc);
    CHECK(noto_sans_bold_20.dsc == orbitron_bold_20.dsc);
    CHECK(noto_sans_light_12.dsc == orbitron_12.dsc);
    CHECK(noto_sans_20.line_height == original.line_height);
    CHECK(noto_sans_20.base_line == original.base_line);
    CHECK(lv_obj_get_width(existing) != original_width);
    CHECK(mdi_icons_24.dsc == icon_data);
    CHECK(source_code_pro_14.dsc == mono_data);

    auto* added = lv_label_create(test_screen());
    lv_obj_set_style_text_font(added, &noto_sans_20, 0);
    lv_label_set_text(added, "NEMESIS 530 CNC");
    lv_obj_update_layout(added);
    CHECK(lv_obj_get_width(added) == lv_obj_get_width(existing));

    theme_manager_apply_font_family(true); // Repeated applies must not save Orbitron as Noto.
    theme_manager_apply_font_family(false);
    lv_obj_update_layout(existing);
    CHECK(noto_sans_20.dsc == original.dsc);
    CHECK(noto_sans_20.fallback == original.fallback);
    CHECK(lv_obj_get_width(existing) == original_width);
}

TEST_CASE_METHOD(NemesisFontFixture, "NEMESIS renders Russian from its own face with a stable baseline",
                 "[nemesis_font]") {
    lv_font_glyph_dsc_t before{};
    REQUIRE(lv_font_get_glyph_dsc(&noto_sans_20, &before, 0x042F, 0));
    const auto noto_data = noto_sans_20.dsc;
    theme_manager_apply_font_family(true);

    lv_font_glyph_dsc_t latin{};
    lv_font_glyph_dsc_t russian{};
    REQUIRE(lv_font_get_glyph_dsc(&noto_sans_20, &latin, 'N', 0));
    REQUIRE(lv_font_get_glyph_dsc(&noto_sans_20, &russian, 0x042F, 0));
    CHECK(latin.resolved_font->dsc == orbitron_20.dsc);
    CHECK(russian.resolved_font->dsc == orbitron_20.dsc);
    CHECK(russian.resolved_font->dsc != noto_data);
    CHECK(russian.resolved_font->line_height == noto_sans_20.line_height);
    CHECK(russian.resolved_font->base_line == noto_sans_20.base_line);
    for (uint32_t cp = 0x410; cp <= 0x44F; ++cp) {
        lv_font_glyph_dsc_t glyph{};
        REQUIRE(lv_font_get_glyph_dsc(&noto_sans_20, &glyph, cp, 0));
        CHECK_FALSE(glyph.is_placeholder);
        CHECK(glyph.resolved_font->dsc == orbitron_20.dsc);
    }
    for (uint32_t cp : {0x401, 0x451, 0xB7, 0xD7, 0x2116, 0x2212, 0x2248}) {
        lv_font_glyph_dsc_t glyph{};
        REQUIRE(lv_font_get_glyph_dsc(&noto_sans_bold_20, &glyph, cp, 0));
        CHECK_FALSE(glyph.is_placeholder);
        CHECK(glyph.resolved_font->dsc == orbitron_bold_20.dsc);
    }

    const auto* nav_font = helix::ui::original_text_font(&noto_sans_20);
    REQUIRE(lv_font_get_glyph_dsc(nav_font, &russian, 0x042F, 0));
    CHECK(russian.resolved_font->dsc == noto_data);
    CHECK(russian.adv_w == before.adv_w);
    CHECK(russian.box_h == before.box_h);
    CHECK(russian.ofs_y == before.ofs_y);

    theme_manager_apply_font_family(false);
    theme_manager_apply_font_family(true);
    CHECK(noto_sans_20.fallback->dsc == noto_data);
}

TEST_CASE_METHOD(NemesisFontFixture, "NEMESIS preserves CJK fallback load unload and theme restore",
                 "[nemesis_font][cjk_font]") {
    auto& manager = helix::system::CjkFontManager::instance();
    SECTION("Language loaded before theme") {
        manager.on_language_changed("zh");
        REQUIRE(manager.is_loaded());
        const auto* cjk = noto_sans_14.fallback;
        theme_manager_apply_font_family(true);
        const auto* nav_font = helix::ui::original_text_font(&noto_sans_14);
        REQUIRE(noto_sans_14.fallback != nullptr);
        CHECK(noto_sans_14.fallback->fallback == cjk);
        theme_manager_apply_font_family(false);
        CHECK(noto_sans_14.fallback == cjk);
        manager.shutdown();
        CHECK(noto_sans_14.fallback == nullptr);
        CHECK(nav_font->fallback == nullptr);
    }
    SECTION("Language loaded and unloaded during theme") {
        theme_manager_apply_font_family(true);
        const auto* original = noto_sans_14.fallback;
        manager.on_language_changed("ja");
        REQUIRE(manager.is_loaded());
        CHECK(noto_sans_14.fallback == original);
        REQUIRE(original->fallback != nullptr);
        lv_font_glyph_dsc_t glyph{};
        REQUIRE(lv_font_get_glyph_dsc(&noto_sans_14, &glyph, 0x5370, 0));
        CHECK_FALSE(glyph.is_placeholder);
        manager.on_language_changed("en");
        CHECK(original->fallback == nullptr);
        CHECK(noto_sans_14.dsc == orbitron_14.dsc);
        theme_manager_apply_font_family(false);
        CHECK(noto_sans_14.fallback == nullptr);
    }
}

class NemesisNavbarFontFixture : public LVGLUITestFixture {
  public:
    NemesisNavbarFontFixture() {
        theme_manager_apply_font_family(false);
    }
    ~NemesisNavbarFontFixture() override {
        theme_manager_apply_font_family(false);
    }
};

TEST_CASE_METHOD(NemesisNavbarFontFixture,
                 "NEMESIS navigation retains Noto during live switch startup and resize",
                 "[nemesis_font][navigation]") {
    const std::array<const void*, 4> noto_data = {noto_sans_10.dsc, noto_sans_11.dsc,
                                                noto_sans_12.dsc, noto_sans_14.dsc};
    const auto icon_data = mdi_icons_24.dsc;
    const bool at_startup = GENERATE(false, true);
    theme_manager_apply_font_family(at_startup);
    auto* navbar = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
    REQUIRE(navbar != nullptr);
    NavigationManager::instance().wire_events(navbar);

    auto check_nav = [&]() {
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(navbar);
        size_t checked = 0;
        for (const char* name : {"nav_label_home", "nav_label_controls", "nav_label_print_select",
                                 "nav_label_filament", "nav_label_settings", "nav_label_advanced"}) {
            auto* label = lv_obj_find_by_name(navbar, name);
            REQUIRE(label != nullptr);
            REQUIRE(lv_obj_get_content_width(label) > 0);
            const auto* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
            REQUIRE(font != nullptr);
            CHECK(std::find(noto_data.begin(), noto_data.end(), font->dsc) != noto_data.end());
            ++checked;
        }
        CHECK(checked == 6);
        CHECK(mdi_icons_24.dsc == icon_data);
    };
    check_nav();
    theme_manager_apply_font_family(true);
    theme_manager_notify_change();
    check_nav();
    CHECK(noto_sans_20.dsc == orbitron_20.dsc);
    for (int width : {64, 96}) {
        lv_obj_set_width(navbar, width);
        check_nav();
    }
    theme_manager_apply_font_family(false);
    theme_manager_notify_change();
    check_nav();
    CHECK(noto_sans_14.dsc == noto_data[3]);
}
