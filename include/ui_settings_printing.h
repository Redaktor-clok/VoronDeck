// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.h
 * @brief Printing Settings overlay - machine limits, retraction, multi-filament system,
 *        filament temps and cooldown behaviour, timelapse
 *
 * This overlay allows users to configure:
 * - Machine velocity/acceleration limits
 * - Firmware retraction settings (when available)
 * - Enclosure marking
 * - Multi-filament system management (when an AMS is detected)
 * - Material temperature presets
 * - Cold extrude / post-op nozzle cooldown behaviour
 * - Timelapse recording (when available)
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SettingsManager for persistence
 * @see DisplaySettingsManager for G-code render mode
 */

#pragma once

#include "lvgl/lvgl.h"
#include "observer_factory.h"
#include "overlay_base.h"
#include "static_panel_registry.h"
#include "json_fwd.h"

namespace helix::settings {

/**
 * @class PrintingSettingsOverlay
 * @brief Overlay for configuring printing-related settings
 *
 * @code
 * helix::settings::get_printing_settings_overlay().show(parent_screen);
 * @endcode
 */
class PrintingSettingsOverlay : public OverlayBase {
  public:
    ~PrintingSettingsOverlay() override;
    void init_subjects() override;
    void deinit_subjects();
    const char* get_name() const override {
        return "Printing Settings";
    }
    const char* xml_component() const override {
        return "settings_printing_overlay";
    }

    void register_callbacks() override;
    void on_activate() override;

  private:
    void query_macro_recovery();
    void set_macro_recovery(bool enabled);
    void check_recovery_updates();
    void recovery_request(const std::string& method, const json& params,
                          std::function<void(const json&)> done, uint32_t timeout = 30000);
    void recovery_status(const char* text);
    SubjectManager subjects_;
    lv_subject_t recovery_enabled_{}, recovery_disabled_{}, recovery_status_{};
    char recovery_status_buf_[768]{};
    uint64_t recovery_generation_ = 0;
    bool recovery_busy_ = false;
    bool recovery_supported_ = false;
    /// Re-asks the mesh-include status whenever the plr feature publishes a
    /// non-Absent state, so the rows cannot sit on a stale or missing answer
    /// while the page is open.
    ObserverGuard plr_feature_observer_;
    ObserverGuard recovery_connection_observer_;
    ObserverGuard recovery_capability_observer_;
};

inline PrintingSettingsOverlay& get_printing_settings_overlay() {
    return lazy_global<PrintingSettingsOverlay>("PrintingSettingsOverlay");
}

} // namespace helix::settings
