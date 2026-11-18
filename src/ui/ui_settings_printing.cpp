// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.cpp
 * @brief Implementation of PrintingSettingsOverlay
 */

#include "ui_settings_printing.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_overlay_retraction_settings.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_settings_machine_limits.h"
#include "ui_settings_macro_buttons.h"
#include "ui_settings_material_temps.h"
#include "ui_settings_motion.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "plr_backend.h"
#include "plr_plugin_rpc.h"
#include "post_op_cooldown_manager.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "config.h"
#include "i_moonraker_client.h"
#include "static_subject_registry.h"
#include "ui_settings_printer_maintenance.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::event_selected;

namespace {

/// Ask the plugin whether printer.cfg carries the mesh include, and publish
/// the answer into the row's subject. Asked each time the overlay opens, so
/// a hand edit of printer.cfg is picked up; a failed ask keeps the last
/// answer and the next open asks again.
void query_plr_mesh_include() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    if (get_printer_state().plugin_status_state().plugin_feature(
            kPlrFeatureName, kPlrFeatureMinVersion) == PluginFeatureState::Absent) {
        return;
    }
    plr_rpc::request_mesh_include_status(
        *api,
        [](bool present) {
            helix::ui::queue_update(
                "PrintingSettingsOverlay::plr_mesh_include_status", [present]() {
                    get_printer_state().plugin_status_state().set_plr_mesh_include(present);
                });
        },
        [](const MoonrakerError&) {});
}

/// The restart a mesh-include change ends in. The restart is machine-moving,
/// so it keeps its own confirmation, separate from the change's.
void offer_firmware_restart() {
    helix::ui::modal_confirm(
        lv_tr("Restart Firmware?"),
        lv_tr("The change takes effect after Klipper restarts. This will restart Klipper "
              "firmware. Any active operations will be interrupted."),
        ModalSeverity::Warning, lv_tr("Restart"),
        [] { EmergencyStopOverlay::instance().firmware_restart(); });
}

/// Send the mesh-include action the user confirmed. The reply republishes
/// the row's state and offers the restart; both touch LVGL, and the client
/// delivers on its own thread, so they hop to the main thread.
void apply_plr_mesh_include(bool add) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    plr_rpc::mesh_include_action(
        *api, add,
        [](bool present, bool changed) {
            helix::ui::queue_update(
                "PrintingSettingsOverlay::plr_mesh_include_applied", [present, changed]() {
                    get_printer_state().plugin_status_state().set_plr_mesh_include(present);
                    // A no-op action (the include appeared between the ask and
                    // the confirm) changed nothing, so no restart is owed.
                    if (changed) {
                        offer_firmware_restart();
                    }
                });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[Settings] Changing the plr mesh include failed: {}", err.message);
            helix::ui::queue_update("PrintingSettingsOverlay::plr_mesh_include_failed", []() {
                ToastManager::instance().show(ToastSeverity::ERROR,
                                              lv_tr("Could not change printer.cfg"), 5000);
            });
        });
}

} // namespace

PrintingSettingsOverlay::~PrintingSettingsOverlay() { deinit_subjects(); }

void PrintingSettingsOverlay::init_subjects() {
    if (!init_subjects_guarded([this] {
        UI_MANAGED_SUBJECT_INT(recovery_enabled_, 0, "macro_recovery_enabled", subjects_);
        UI_MANAGED_SUBJECT_INT(recovery_disabled_, 1, "macro_recovery_disabled", subjects_);
        UI_MANAGED_SUBJECT_STRING(recovery_status_, recovery_status_buf_, "",
                                 "macro_recovery_status", subjects_);
    })) return;
    StaticSubjectRegistry::instance().register_deinit("PrintingSettingsOverlay", [] {
        if (auto* overlay = lazy_global_if_exists<PrintingSettingsOverlay>()) overlay->deinit_subjects();
    });
}

void PrintingSettingsOverlay::deinit_subjects() {
    ++recovery_generation_;
    recovery_connection_observer_.reset();
    recovery_capability_observer_.reset();
    deinit_subjects_base(subjects_);
}

void PrintingSettingsOverlay::recovery_status(const char* text) {
    lv_subject_copy_string(&recovery_status_, text);
}

void PrintingSettingsOverlay::recovery_request(const std::string& method, const json& params,
                                               std::function<void(const json&)> done,
                                               uint32_t timeout) {
    auto* client = get_moonraker_client();
    if (!client || client->get_connection_state() != ConnectionState::CONNECTED) {
        recovery_supported_ = false;
        lv_subject_set_int(&recovery_disabled_, 1);
        recovery_status(lv_tr("Printer is disconnected"));
        return;
    }
    if (recovery_busy_) return;
    recovery_busy_ = true;
    lv_subject_set_int(&recovery_disabled_, 1);
    recovery_status(lv_tr("Please wait..."));
    const auto generation = ++recovery_generation_;
    const auto printer = Config::get_instance()->get_active_printer_id();
    auto token = lifetime_.token();
    client->send_jsonrpc(method, params,
        [this, token, generation, printer, client, done](const json& response) {
            token.defer("PrintingSettings::recovery", [this, generation, printer, client, done, response] {
                if (!subjects_initialized_ || generation != recovery_generation_ ||
                    client != get_moonraker_client() || client->get_connection_state() != ConnectionState::CONNECTED ||
                    printer != Config::get_instance()->get_active_printer_id()) return;
                recovery_busy_ = false;
                try {
                    // The mock's successful G-code acknowledgement is an empty object;
                    // state reads still require their complete status/version payload.
                    done(response.contains("result") ? response.at("result") : response);
                    lv_subject_set_int(&recovery_disabled_, recovery_supported_ && !recovery_busy_ ? 0 : 1);
                } catch (const json::exception&) {
                    recovery_supported_ = false;
                    lv_subject_set_int(&recovery_disabled_, 1);
                    recovery_status(lv_tr("Invalid Moonraker response"));
                }
            });
        },
        [this, token, generation, printer, client](const MoonrakerError&) {
            token.defer("PrintingSettings::recovery_error", [this, generation, printer, client] {
                if (!subjects_initialized_ || generation != recovery_generation_ ||
                    client != get_moonraker_client() ||
                    printer != Config::get_instance()->get_active_printer_id()) return;
                recovery_busy_ = false;
                recovery_supported_ = false;
                lv_subject_set_int(&recovery_disabled_, 1);
                recovery_status(lv_tr("Recovery request failed. Reopen this page to refresh its state."));
            });
        }, timeout);
}

void PrintingSettingsOverlay::query_macro_recovery() {
    recovery_request("printer.objects.query", {{"objects", {
        {"save_variables", json::array({"variables"})},
        {"gcode_macro RECOVERY_SET_ENABLED", nullptr}}}}, [this](const json& result) {
        const auto& status = result.at("status");
        const auto macro = status.value("gcode_macro RECOVERY_SET_ENABLED", json::object());
        recovery_supported_ = macro.value("protocol", 0) == 1;
        if (!recovery_supported_) {
            recovery_status(lv_tr("Update recovery macros to enable this switch, then restart Klipper."));
            return;
        }
        const auto variables = status.at("save_variables").at("variables");
        const int enabled = variables.value("recovery_enabled", 1);
        if (enabled != 0 && enabled != 1) {
            recovery_supported_ = false;
            recovery_status(lv_tr("Invalid Moonraker response"));
            return;
        }
        lv_subject_set_int(&recovery_enabled_, enabled);
        lv_subject_notify(&recovery_enabled_);
        const auto version = macro.value("version", std::string{});
        const auto text = std::string("VoronDeck Recovery ") + version + "\n" +
            lv_tr(enabled ? "Recovery checkpoints enabled" : "Recovery checkpoints disabled");
        recovery_status(text.c_str());
    });
}

void PrintingSettingsOverlay::set_macro_recovery(bool enabled) {
    // The switch displays confirmed printer state, never the optimistic tap.
    lv_subject_notify(&recovery_enabled_);
    if (recovery_busy_ || !recovery_supported_) return;
    if (lv_subject_get_int(get_printer_state().print_state().get_job_holds_machine_subject()) ||
        get_printer_state().is_paused()) {
        recovery_status(lv_tr("Stop printing before changing recovery settings."));
        return;
    }
    recovery_request("printer.gcode.script",
                     {{"script", enabled ? "RECOVERY_SET_ENABLED ENABLE=1" : "RECOVERY_SET_ENABLED ENABLE=0"}},
                     [this](const json&) { query_macro_recovery(); });
}

void PrintingSettingsOverlay::check_recovery_updates() {
    recovery_request("machine.update.refresh", {{"name", "vorondeck_recovery"}}, [this](const json& result) {
        const auto versions = result.value("version_info", json::object());
        const auto component = versions.value("vorondeck_recovery", json::object());
        if (component.empty()) {
            recovery_status(lv_tr("Recovery update component is not configured."));
            return;
        }
        const auto version = component.value("version", std::string{});
        const auto remote = component.value("remote_version", std::string{});
        std::string text = "VoronDeck Recovery " + version + "\n";
        if (component.value("restart_required", false)) {
            text += lv_tr("Recovery updated. Restart Klipper to activate it.");
        } else if (!component.value("is_valid", false) || remote.empty() ||
                   !component.value("last_error", std::string{}).empty()) {
            text += lv_tr("Recovery update check failed. Check the network and repository access.");
        } else {
            text += lv_tr(version == remote ? "Up to date" : "Update available");
            if (version != remote) text += " · " + remote;
        }
        recovery_status(text.c_str());
    }, 360000);
}

void PrintingSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_macro_recovery_changed", [](lv_event_t* e) {
            get_printing_settings_overlay().set_macro_recovery(event_checked(e));
        }},
        {"on_recovery_check_updates", [](lv_event_t*) {
            get_printing_settings_overlay().check_recovery_updates();
        }},
        {"on_recovery_component_updates", [](lv_event_t*) {
            get_printer_maintenance_overlay().open(lv_screen_active(), true);
        }},
        {"on_enclosure_style_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_enclosure_style(
                 static_cast<helix::bed_drying::EnclosureStyle>(event_selected(e)));
         }},
        {"on_machine_limits_clicked",
         [](lv_event_t*) {
             auto& overlay = get_machine_limits_overlay();
             overlay.set_api(get_moonraker_api());
             overlay.show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_motion_settings_clicked", [](lv_event_t*) { show_motion_settings_overlay(); }},
        {"on_ams_settings_clicked",
         [](lv_event_t*) {
             helix::ui::get_ams_device_operations_overlay().show(
                 get_printing_settings_overlay().parent_screen_);
         }},
        {"on_material_temps_clicked",
         [](lv_event_t*) {
             get_material_temps_overlay().show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_allow_cold_extrude_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_allow_cold_extrude(event_checked(e));
         }},
        {"on_filament_auto_cooldown_changed",
         [](lv_event_t* e) {
             const bool enabled = event_checked(e);
             SettingsManager::instance().set_filament_auto_cooldown(enabled);
             // Turning it off mid-countdown takes effect now, not when the countdown ends.
             if (!enabled) {
                 PostOpCooldownManager::instance().cancel();
             }
         }},
        {"on_tool_change_confirm_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_tool_change_require_confirmation(
                 event_checked(e));
         }},
        {"on_filament_auto_open_editor_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_filament_auto_open_editor(event_checked(e));
         }},
        {"on_retraction_row_clicked",
         [](lv_event_t*) {
             get_global_retraction_settings().show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_timelapse_settings_clicked", [](lv_event_t*) { open_timelapse_settings(); }},
        {"on_power_loss_recovery_changed",
         [](lv_event_t* e) {
             IMoonrakerAPI* api = get_moonraker_api();
             if (!api) {
                 return;
             }
             // The plugin's own verdict comes back through the features map,
             // which is also what puts the switch right again after a failure.
             plr_rpc::write_user(
                 *api, event_checked(e), [api]() { plr_rpc::refresh_features(*api); },
                 [api](const MoonrakerError& err) {
                     spdlog::warn("[Settings] Writing plr_user failed: {}", err.message);
                     plr_rpc::refresh_features(*api);
                 });
         }},
        {"on_macro_buttons_clicked",
         [](lv_event_t*) {
             get_macro_buttons_overlay().show(get_printing_settings_overlay().parent_screen_);
         }},
        {"on_plr_mesh_include_add_clicked",
         [](lv_event_t*) {
             helix::ui::modal_confirm(
                 lv_tr("Reload the saved bed mesh?"),
                 lv_tr("VoronDeck adds [include helix-plr/*.cfg] to printer.cfg, so a resume "
                       "reloads the mesh the print started with. A backup is kept beside "
                       "printer.cfg, and Klipper must restart for it to take effect."),
                 ModalSeverity::Warning, lv_tr("Add"), [] { apply_plr_mesh_include(true); });
         }},
        {"on_plr_mesh_include_remove_clicked",
         [](lv_event_t*) {
             helix::ui::modal_confirm(
                 lv_tr("Stop reloading the saved bed mesh?"),
                 lv_tr("VoronDeck removes its [include helix-plr/*.cfg] lines from "
                       "printer.cfg. A backup is kept beside printer.cfg, and Klipper must "
                       "restart for it to take effect. A resume then uses the printer's "
                       "default mesh profile."),
                 ModalSeverity::Warning, lv_tr("Remove"), [] { apply_plr_mesh_include(false); });
         }},
    });

    auto& plugin = get_printer_state().plugin_status_state();
    plr_feature_observer_ = helix::ui::observe<int>(
        plugin.get_plr_feature_state_subject(), this,
        [](PrintingSettingsOverlay*, int state) {
            if (state != static_cast<int>(PluginFeatureState::Absent)) {
                query_plr_mesh_include();
            }
        },
        get_printer_state().get_subjects_lifetime());
    recovery_connection_observer_ = helix::ui::observe<int>(
        get_printer_state().network_state().get_moonraker_connection_state_subject(), this,
        [](PrintingSettingsOverlay* self, int state) {
            ++self->recovery_generation_;
            self->recovery_busy_ = false;
            self->recovery_supported_ = false;
            lv_subject_set_int(&self->recovery_disabled_, 1);
            if (state != 1) {
                self->recovery_status(lv_tr("Printer is disconnected"));
            } else if (self->visible_ && lv_subject_get_int(get_printer_state().capabilities_state().subject(helix::Capability::HasRecoveryMacros))) {
                self->query_macro_recovery();
            }
        }, get_printer_state().get_subjects_lifetime());
    recovery_capability_observer_ = helix::ui::observe<int>(
        get_printer_state().capabilities_state().subject(helix::Capability::HasRecoveryMacros), this,
        [](PrintingSettingsOverlay* self, int present) {
            ++self->recovery_generation_;
            self->recovery_busy_ = false;
            self->recovery_supported_ = false;
            lv_subject_set_int(&self->recovery_disabled_, 1);
            if (present && self->visible_) self->query_macro_recovery();
        }, get_printer_state().get_subjects_lifetime());
}

void PrintingSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    query_plr_mesh_include();
    ++recovery_generation_;
    recovery_busy_ = false;
    recovery_supported_ = false;
    lv_subject_set_int(&recovery_disabled_, 1);
    if (lv_subject_get_int(get_printer_state().capabilities_state().subject(helix::Capability::HasRecoveryMacros))) {
        query_macro_recovery();
    }
}

} // namespace helix::settings
