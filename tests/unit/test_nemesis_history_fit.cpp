// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "../test_helpers/config_test_access.h"
#include "nemesis_fonts.h"
#include "wizard_config_paths.h"
#include "nemesis_theme.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "translation_loader.h"
#include "ui_button.h"
#include "ui_panel_print_status.h"
#include "../catch_amalgamated.hpp"

using namespace helix;

class NemesisHistoryFitFixture : public LVGLUITestFixture {
  public:
    NemesisHistoryFitFixture() : original_(theme_manager_get_active_theme()),
                                 dark_(theme_manager_is_dark_mode()),
                                 config_(ConfigTestAccess::data(*Config::get_instance())) {
        PrintStatusWidget::destroy_formatter_for_test();
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        ToolState::instance().init_subjects(false);
        auto& panel = get_global_print_status_panel();
        if (!panel.are_subjects_initialized())
            panel.init_subjects();
        ui::ensure_translation_loaded("ru");
        lv_translation_set_language("ru");
    }
    ~NemesisHistoryFitFixture() override {
        PrintStatusWidget::destroy_formatter_for_test();
        lv_translation_set_language(ui::kIdentityLocale);
        ConfigTestAccess::data(*Config::get_instance()) = config_;
        theme_manager_apply_theme(original_, dark_);
    }
  private:
    ThemeData original_;
    bool dark_;
    nlohmann::json config_;
};

TEST_CASE_METHOD(NemesisHistoryFitFixture,
                 "NEMESIS history captions fit the existing row without hiding metadata",
                 "[nemesis_font][print_status][nemesis_history_fit]") {
    ScopedResolution resolution(lv_display_get_default(), 800, 480);
    theme_manager_refresh_layout_constants(lv_display_get_default());
    const auto nemesis = load_theme_from_file(kNemesisTheme);
    const auto standard = load_theme_from_file(kDefaultTheme);
    REQUIRE(nemesis.is_valid());
    REQUIRE(standard.is_valid());
    REQUIRE(std::string(lv_tr("Reprint Last")) != "Reprint Last");

    auto apply = [&](bool use_nemesis) {
        auto* config = Config::get_instance();
        config->set<std::string>(config->df() + PRINTER_IMAGE,
                                 use_nemesis ? kNemesisImage : "shipped:voron-v2");
        theme_manager_apply_theme(use_nemesis ? nemesis : standard, true);
        const auto* body = theme_manager_get_font("font_body");
        REQUIRE((ui::original_text_font(body) != body) == use_nemesis);
        if (use_nemesis)
            REQUIRE(body->dsc == orbitron_18.dsc);
    };

    // Repeat creation under NEMESIS to cover startup as well as the live switch.
    for (bool starts_nemesis : {false, true}) {
        apply(starts_nemesis);
        PanelWidgetHarness<PrintStatusWidget> h(
            test_screen(), HarnessConfig{nlohmann::json{{"layout_style", "detailed"}}});
        auto settle = [&](int width) {
            h.resize(6, 3, width, 158);
            process_lvgl(50);
            auto* queue_hidden = lv_xml_get_subject(nullptr, "print_status_queue_hidden");
            REQUIRE(queue_hidden);
            lv_subject_set_int(queue_hidden, 0);
            h.widget().on_activate();
            process_lvgl(50);
            REQUIRE(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) == 2);
            for (const char* name : {"print_status_idle_when", "print_status_idle_meta"}) {
                auto* subject = lv_xml_get_subject(nullptr, name);
                REQUIRE(subject);
                lv_subject_copy_string(subject, "12.11 · 2ч 15м");
            }
            lv_obj_update_layout(h.root());
        };
        auto check = [&](int width, const char* font_token, bool one_row) {
            settle(width);
            auto* reprint = h.child("idle_reprint_btn");
            auto* queue = h.child("idle_queue_btn");
            auto* details = h.child("idle_data_col");
            REQUIRE(reprint);
            REQUIRE(queue);
            REQUIRE(details);
            REQUIRE_FALSE(lv_obj_has_flag(queue, LV_OBJ_FLAG_HIDDEN));
            for (auto* button : {reprint, queue}) {
                auto* label = ui_button_get_label(button);
                REQUIRE(label);
                CHECK(std::string(lv_label_get_text(label)) ==
                      lv_tr(button == reprint ? "Reprint Last" : "Job Queue"));
                CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
                      theme_manager_get_font(font_token));
                CHECK(lv_obj_get_width(label) <= lv_obj_get_content_width(button));
            }
            if (one_row) {
                lv_area_t a{}, b{}, data{}, card{};
                lv_obj_get_coords(reprint, &a);
                lv_obj_get_coords(queue, &b);
                lv_obj_get_coords(details, &data);
                lv_obj_get_coords(h.root(), &card);
                CHECK(a.y1 == b.y1);
                CHECK(a.x2 < b.x1);
                CHECK(b.y2 <= card.y2);
                const auto needs = measure_idle_card_needs(width);
                CHECK(lv_obj_get_height(details) >= needs.detail_h);
                CHECK(data.y2 < a.y1);
                CHECK(h.widget().fits_at(width, 158));
            }
        };
        check(466, starts_nemesis ? "font_small" : "font_body", true);
        apply(true);
        check(466, "font_small", true);
        check(700, "font_body", true);
        check(466, "font_small", true);
        apply(false);
        check(466, "font_body", true);
    }
}
