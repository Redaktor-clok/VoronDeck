// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/fan_spin_animation.h"

namespace helix::ui {

void fan_spin_anim_cb(void* var, int32_t value) {
    auto* icon = static_cast<lv_obj_t*>(var);
    // Keep the animation clock running, but do not dirty a hidden/clipped tree.
    // Its next visible frame picks up the current phase automatically.
    if (!icon || !lv_obj_is_visible(icon) ||
        lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) == value)
        return;
    lv_obj_set_style_transform_rotation(icon, value, 0);
}

void fan_spin_stop(lv_obj_t* icon) {
    if (!icon)
        return;
    lv_anim_delete(icon, fan_spin_anim_cb);
    if (lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) != 0)
        lv_obj_set_style_transform_rotation(icon, 0, 0);
}

void fan_spin_start(lv_obj_t* icon, int speed_pct) {
    if (!icon || speed_pct <= 0)
        return;

    // Scale duration inversely with speed: 100% → MIN, 1% → MAX
    uint32_t duration =
        kFanSpinMaxDurationMs -
        static_cast<uint32_t>((kFanSpinMaxDurationMs - kFanSpinMinDurationMs) * speed_pct / 100);

    // Rebinding an unchanged speed must not restart the phase or allocate again.
    if (auto* running = lv_anim_get(icon, fan_spin_anim_cb);
        running && lv_anim_get_time(running) == duration)
        return;

    // Center the rotation pivot so the icon spins in place.
    lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);

    // Delete existing animation and restart with new duration
    lv_anim_delete(icon, fan_spin_anim_cb);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, icon);
    lv_anim_set_exec_cb(&anim, fan_spin_anim_cb);
    lv_anim_set_values(&anim, 0, 3600); // 0° to 360° (LVGL uses 0.1° units)
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_start(&anim);
}

} // namespace helix::ui
