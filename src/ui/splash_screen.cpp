// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "splash_screen.h"

#include "ui_utils.h"

#include "helix_timing.h"
#include "helix_version.h"
#include "splash_branding.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix {

void show_splash_screen(int screen_width, int screen_height) {
    spdlog::debug("[Splash Screen] Showing splash screen ({}x{})", screen_width, screen_height);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Apply theme background color (screen_bg runtime constant set by theme_manager_init)
    theme_manager_apply_bg_color(screen, "screen_bg", LV_PART_MAIN);

    // Disable scrollbars on screen
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const bool dark_mode = theme_manager_is_dark_mode();
    lv_obj_t* splash_widget =
        create_splash_branding(screen, screen_width, screen_height, theme_manager_get_color("text"),
                               theme_manager_get_font("font_heading"));
    lv_obj_set_style_opa(splash_widget, LV_OPA_TRANSP, LV_PART_MAIN);

    // Version number in upper-left corner (subtle, theme-aware)
    lv_obj_t* version_label = lv_label_create(screen);
    lv_label_set_text(version_label, "VoronDeck " HELIX_VERSION);
    lv_obj_set_style_text_color(
        version_label, dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_opa(version_label, LV_OPA_40, LV_PART_MAIN);
    lv_obj_align(version_label, LV_ALIGN_TOP_LEFT, 8, 6);

    // Create fade-in animation (0.5 seconds)
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, splash_widget);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 500); // 500ms = 0.5 seconds
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_start(&anim);

    // Run LVGL timer to process fade-in animation and keep splash visible
    // Total display time: 2 seconds (including 0.5s fade-in)
    uint32_t splash_start = helix::timing::get_ticks();
    uint32_t splash_duration = 2000; // 2 seconds total

    while (helix::timing::get_ticks() - splash_start < splash_duration) {
        lv_timer_handler(); // Process animations and rendering
        helix::timing::delay(5);
    }

    // Clean up splash screen (guard against early shutdown)
    helix::ui::safe_delete(version_label);
    helix::ui::safe_delete(splash_widget);

    spdlog::debug("[Splash Screen] complete");
}

} // namespace helix
