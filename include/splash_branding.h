// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "data_root_resolver.h"

#include <algorithm>
#include <lvgl.h>

namespace helix {

// Both startup paths use the same lightweight, preconverted logo.
inline lv_obj_t* create_splash_branding(lv_obj_t* screen, int width, int height,
                                        lv_color_t text_color, const lv_font_t* font) {
    lv_obj_t* group = lv_obj_create(screen);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(group, 12, 0);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    const auto path = asset_component_uri("assets/images/vorondeck-splash-logo.bin");
    lv_image_header_t header{};
    if (lv_image_decoder_get_info(path.c_str(), &header) == LV_RESULT_OK) {
        lv_obj_t* logo = lv_image_create(group);
        lv_image_set_src(logo, path.c_str());
        lv_image_set_inner_align(logo, LV_IMAGE_ALIGN_CONTAIN);
        const int size = std::min(256, std::min(width, height) / 2);
        lv_obj_set_size(logo, size, size);
    }

    lv_obj_t* title = lv_label_create(group);
    lv_label_set_text(title, "VoronDeck");
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, text_color, 0);
    lv_obj_center(group);
    return group;
}

} // namespace helix
