// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

// NEMESIS uses VoronDeck Orbit for Latin/Russian; Noto supplies other scripts.
// The orbitron_* symbols are retained internal names, not font-family metadata.
LV_FONT_DECLARE(orbitron_8);
LV_FONT_DECLARE(orbitron_10);
LV_FONT_DECLARE(orbitron_11);
LV_FONT_DECLARE(orbitron_12);
LV_FONT_DECLARE(orbitron_14);
LV_FONT_DECLARE(orbitron_16);
LV_FONT_DECLARE(orbitron_18);
LV_FONT_DECLARE(orbitron_20);
LV_FONT_DECLARE(orbitron_24);
LV_FONT_DECLARE(orbitron_26);
LV_FONT_DECLARE(orbitron_28);
LV_FONT_DECLARE(orbitron_32);
LV_FONT_DECLARE(orbitron_40);
LV_FONT_DECLARE(orbitron_48);
LV_FONT_DECLARE(orbitron_64);
LV_FONT_DECLARE(orbitron_bold_14);
LV_FONT_DECLARE(orbitron_bold_16);
LV_FONT_DECLARE(orbitron_bold_18);
LV_FONT_DECLARE(orbitron_bold_20);
LV_FONT_DECLARE(orbitron_bold_24);
LV_FONT_DECLARE(orbitron_bold_28);
LV_FONT_DECLARE(orbitron_bold_32);
LV_FONT_DECLARE(orbitron_bold_40);
LV_FONT_DECLARE(orbitron_bold_48);
LV_FONT_DECLARE(orbitron_bold_64);

/// Switch registered text faces in place; existing widgets keep valid font pointers.
/// Call on the LVGL thread when the active theme changes, and restore at shutdown.
void theme_manager_apply_font_family(bool nemesis);

namespace helix::ui {
/// Navigation labels retain the unmodified Noto face while the theme uses Orbit.
const lv_font_t* original_text_font(const lv_font_t* font);
/// Keep runtime language fallbacks behind the original text face during a theme swap.
void set_text_font_fallback(lv_font_t* font, const lv_font_t* fallback);
} // namespace helix::ui
