// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_updates.h"

#include "ui_callback_helpers.h"
#include "ui_icon.h"
#include "ui_info_qr_modal.h"
#include "ui_modal.h"
#include "ui_settings_printer_maintenance.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "config.h"
#include "exception_policy.h"
#include "helix_version.h"
#include "host_identity.h"
#include "http_executor.h"
#include "i_moonraker_client.h"
#include "json_fwd.h"
#include "json_utils.h"
#include "lvgl.h"
#include "platform_info.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "replace_method_callback.h"
#include "system/config_trust.h"
#include "system/run_capture.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#ifdef __ANDROID__
#include "system/android_update_source.h"

#include <SDL.h>
#elif defined(HELIX_PLATFORM_X86_DESKTOP)
#include "system/release_page.h"

#include <SDL.h>
#endif

namespace helix::settings {

#if defined(__ANDROID__) || defined(HELIX_PLATFORM_X86_DESKTOP)
namespace {
/// The release the update check is offering, for builds that open a page
/// instead of running the tarball updater.
helix::OfferedRelease offered_release() {
    auto& checker = UpdateChecker::instance();
    helix::OfferedRelease offered;
    if (auto info = checker.get_cached_update()) {
        offered.tag_name = info->tag_name;
        offered.version = info->version;
    }
    offered.on_github = checker.get_channel() != UpdateChecker::UpdateChannel::Dev;
    return offered;
}
} // namespace
#endif

void UpdatesSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_vorondeck_progress_close",
         [](lv_event_t*) {
             auto& self = get_updates_settings_overlay();
             if (self.package_progress_modal_) {
                 helix::ui::modal_hide(self.package_progress_modal_);
                 self.package_progress_modal_ = nullptr;
             }
         }},
        {"on_vorondeck_component_updates",
         [](lv_event_t*) { get_printer_maintenance_overlay().open(lv_screen_active(), true); }},
        {"on_vorondeck_test_update",
         [](lv_event_t*) { get_updates_settings_overlay().confirm_test_update(); }},
        {"on_vorondeck_stable_update",
         [](lv_event_t*) { get_updates_settings_overlay().confirm_release_update("stable"); }},
        {"on_vorondeck_beta_update",
         [](lv_event_t*) { get_updates_settings_overlay().confirm_release_update("beta"); }},
        {"on_update_download_start",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Starting update download");
             UpdateChecker::instance().start_download();
         }},
        {"on_update_download_cancel",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Download cancelled by user");
             UpdateChecker::instance().cancel_download();
             get_updates_settings_overlay().hide_update_download_modal();
         }},
        {"on_update_download_dismiss",
         [](lv_event_t*) { get_updates_settings_overlay().hide_update_download_modal(); }},
    });
}

void UpdatesSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    if (vorondeck_update_text_.empty()) {
        vorondeck_update_text_ = std::string("VoronDeck ") + HELIX_VERSION;
    }
    publish_vorondeck_status();
    sync_update_channel_rows(overlay_root_,
                             static_cast<int>(UpdateChecker::instance().get_channel()));
}

void UpdatesSettingsOverlay::publish_vorondeck_status() {
    const auto result =
        release_status_.empty() ? json::object() : json::parse(release_status_, nullptr, false);
    const auto channels = helix::json_util::safe_object(result, "channels");
    for (const auto* kind : {"stable", "test", "beta"}) {
        if (auto* row = helix::ui::find_optional(
                overlay_root_, (std::string("row_") + kind + "_release").c_str())) {
            const bool stable = std::string(kind) == "stable";
            const bool beta = std::string(kind) == "beta";
            const auto& offer = stable ? stable_offer_ : beta ? beta_offer_ : test_offer_;
            const auto channel = helix::json_util::safe_object(channels, kind);
            const auto version = helix::json_util::safe_string(channel, "version");
            const auto state = helix::json_util::safe_string(channel, "status");
            lv_obj_set_state(row, LV_STATE_DISABLED,
                             vorondeck_install_busy_ ||
                                 (vorondeck_check_busy_ && !vorondeck_check_background_));
            if (auto* icon = helix::ui::find_optional(row, "row_icon")) {
                helix::ui::icon::set_source(icon, state == "current" ? "check" : "download");
            }
            if (auto* label = helix::ui::find_optional(row, "label")) {
                auto text = std::string(lv_tr(stable ? "Stable release"
                                              : beta ? "Beta"
                                                     : "Test release"));
                if (!version.empty())
                    text += " · " + version;
                lv_label_set_text(label, text.c_str());
                lv_obj_set_style_text_color(
                    label, theme_manager_get_color(stable ? "success" : "warning"), 0);
            }
            const char* description = "Check for updates";
            if (vorondeck_check_busy_)
                description = "Checking VoronDeck updates...";
            else if (!offer.empty())
                description = stable ? "Completed release"
                                     : "Optional prerelease · install only if you choose";
            else if (state == "access_required")
                description = "Beta access required";
            else if (state == "current")
                description = "This release is installed";
            else if (state == "older")
                description = "The installed version is newer";
            else if (state == "none")
                description = "No newer test release";
            else if (!state.empty() || !release_status_.empty())
                description = "Update check failed. Tap to retry.";
            if (auto* label = helix::ui::find_optional(row, "description"))
                lv_label_set_text(label, lv_tr(description));
        }
    }
    if (auto* label = helix::ui::find_optional(overlay_root_, "vorondeck_update_status")) {
        lv_label_set_text(label, vorondeck_update_text_.c_str());
    }
}

bool UpdatesSettingsOverlay::check_vorondeck_updates(bool background) {
#if defined(HELIX_PLATFORM_ESP32)
    // Firmware has no local Python/Moonraker installation or Linux package updater.
    (void)background;
    return false;
#else
    if (vorondeck_check_busy_ || vorondeck_install_busy_)
        return false;
    const int selected_channel = SystemSettingsManager::instance().get_update_channel();
    if (selected_channel != 0 && selected_channel != 1)
        return false;
    if (background && (is_wizard_active() ||
                       job_holds_machine(get_printer_state().print_state().get_print_lifecycle())))
        return false;
    auto& worker = helix::http::HttpExecutor::slow();
    if (!worker.running())
        return false;
    vorondeck_check_busy_ = true;
    vorondeck_check_background_ = background;
    if (!background) {
        test_offer_.clear();
        stable_offer_.clear();
        beta_offer_.clear();
        release_status_.clear();
    }
    vorondeck_update_text_ = lv_tr("Checking VoronDeck updates...");
    publish_vorondeck_status();
    // Finish the object-owned check even if its overlay is closed meanwhile.
    auto token = object_lifetime_.token();
    worker.submit([this, token, background, selected_channel]() {
        json result = {{"status", "error"}};
        helix::contain_exceptions("VoronDeck update check", [&] {
            int exit_code = -1;
            // Constant shipped path: no user data or shell substitutions. The helper
            // bounds git/SSH timeouts and never installs a fetched artifact.
            auto output = helix::run_capture(
                selected_channel == 1
                    ? "python3 assets/updates/check-vorondeck-update.py --channel beta 2>/dev/null"
                    : "python3 assets/updates/check-vorondeck-update.py --channel stable "
                      "2>/dev/null",
                exit_code);
            if (exit_code == 0 && output.size() < 8192) {
                auto parsed = json::parse(output, nullptr, false);
                if (parsed.is_object())
                    result = std::move(parsed);
            }
        });
        token.defer("UpdatesSettings::vorondeck_check", [this, result, background,
                                                         selected_channel]() {
            vorondeck_check_busy_ = false;
            if (vorondeck_install_busy_)
                return;
            if (selected_channel != SystemSettingsManager::instance().get_update_channel()) {
                vorondeck_update_text_ = std::string("VoronDeck ") + HELIX_VERSION;
                publish_vorondeck_status();
                return;
            }
            const auto state = helix::json_util::safe_string(result, "status", "error");
            const auto offers = result.find("offers");
            const bool has_offers =
                offers != result.end() && offers->is_array() && !offers->empty();
            vorondeck_update_text_ =
                std::string("VoronDeck ") +
                helix::json_util::safe_string(result, "installed_version", HELIX_VERSION);
            if (has_offers) {
                vorondeck_update_text_ +=
                    "\n" + std::string(lv_tr("A new VoronDeck package is available"));
            } else if (!result.contains("channels")) {
                vorondeck_update_text_ +=
                    "\n" +
                    std::string(
                        lv_tr(state == "not_installed"
                                  ? "Update checks are available on an installed VoronDeck screen"
                                  : "Update check failed. Check the network and beta SSH access."));
            }
            release_status_ = result.dump();
            test_offer_.clear();
            stable_offer_.clear();
            beta_offer_.clear();
            if (auto offers = result.find("offers"); offers != result.end() && offers->is_array()) {
                for (const auto& offer : *offers) {
                    if (!offer.is_object())
                        continue;
                    const auto kind = helix::json_util::safe_string(offer, "kind");
                    if (kind == "stable" || kind == "beta" || kind == "test") {
                        auto selected = offer;
                        selected["installed_source"] =
                            helix::json_util::safe_string(result, "installed_source");
                        auto& target = kind == "stable" ? stable_offer_
                                       : kind == "beta" ? beta_offer_
                                                        : test_offer_;
                        target = selected.dump();
                    }
                }
            }
            publish_vorondeck_status();
            const auto& notice_offer = !test_offer_.empty()     ? test_offer_
                                       : !stable_offer_.empty() ? stable_offer_
                                                                : beta_offer_;
            const auto test =
                notice_offer.empty() ? json::object() : json::parse(notice_offer, nullptr, false);
            const auto notice = helix::json_util::safe_string(
                test, "notification_id", helix::json_util::safe_string(result, "notification_id"));
            auto* config = Config::get_instance();
            if (!notice.empty() && config && !is_wizard_active() &&
                !job_holds_machine(get_printer_state().print_state().get_print_lifecycle()) &&
                (!background ||
                 config->get<std::string>("/update/vorondeck_notified_package", "") != notice)) {
                const auto channel = helix::json_util::safe_string(
                    test, "kind", helix::json_util::safe_string(result, "active_channel"));
                const auto message =
                    fmt::format(lv_tr("VoronDeck {} is available ({})"),
                                helix::json_util::safe_string(
                                    test, "display_version",
                                    helix::json_util::safe_string(result, "available_version")),
                                lv_tr(channel == "test"   ? "Test release"
                                      : channel == "beta" ? "Beta"
                                                          : "Stable"));
                ToastManager::instance().show_with_action(
                    ToastSeverity::INFO, message.c_str(), lv_tr("Open updates"),
                    [](void*) { get_updates_settings_overlay().show(lv_screen_active()); }, nullptr,
                    0);
                config->set<std::string>("/update/vorondeck_notified_package", notice);
                config->save();
            }
        });
    });
    return true;
#endif
}

void UpdatesSettingsOverlay::confirm_test_update() {
    confirm_release_update("test");
}

void UpdatesSettingsOverlay::confirm_release_update(const std::string& kind) {
    if (kind != "stable" && kind != "beta" && kind != "test")
        return;
    const auto selection = kind == "stable" ? stable_offer_
                           : kind == "beta" ? beta_offer_
                                            : test_offer_;
    if ((vorondeck_check_busy_ && !vorondeck_check_background_) || vorondeck_install_busy_)
        return;
    if (selection.empty()) {
        check_vorondeck_updates();
        return;
    }
    auto* client = get_moonraker_client();
    const auto printer = Config::get_instance()->get_active_printer_id();
    const auto offer = json::parse(selection, nullptr, false);
    const auto version = helix::json_util::safe_string(offer, "display_version");
    if (!offer.is_object() || version.empty()) {
        show_package_progress(
            lv_tr("The selected or installed version has changed. Check updates again."), "", -1);
        return;
    }
    const auto message = fmt::format(
        lv_tr(kind == "stable"
                  ? "Install VoronDeck {}? The screen will restart."
                  : "Install VoronDeck {}? This is a test release and may contain errors."),
        version);
    helix::ui::ConfirmOptions options;
    options.cancel_text = lv_tr("Not now");
    options.owner_token = object_lifetime_.token();
    helix::ui::modal_confirm(
        lv_tr(kind == "stable" ? "Stable release"
              : kind == "beta" ? "Beta"
                               : "Test release"),
        message.c_str(), ModalSeverity::Warning, lv_tr("Update"),
        [this, selection, client, printer] {
            if (client != get_moonraker_client() ||
                printer != Config::get_instance()->get_active_printer_id())
                return;
            install_test_update(selection);
        },
        options);
}

namespace {
std::string package_error(const MoonrakerError& error) {
    if (error.is_transport_loss())
        return lv_tr("Connection to Moonraker was lost or timed out. The update may still be "
                     "running. Check component versions before retrying.");
    const auto& message = error.message;
    if (message.find("Another update is running") != std::string::npos)
        return lv_tr("Another component is being updated. Wait for it to finish, then try again.");
    if (message.find("90 seconds") != std::string::npos)
        return lv_tr(
            "The background update check did not finish within 90 seconds. Try again later.");
    if (message.find("disk space") != std::string::npos)
        return lv_tr("Not enough free disk space to download the update.");
    if (message.find("idle") != std::string::npos || message.find("heater") != std::string::npos)
        return lv_tr("The printer must be idle with heaters off before updating.");
    if (message.find("changed") != std::string::npos)
        return lv_tr("The selected or installed version has changed. Check updates again.");
    if (message.find("moonraker.asvc") != std::string::npos)
        return lv_tr(
            "Moonraker cannot restart VoronDeck. Repair the update manager using the installer.");
    if (message.find("SHA256") != std::string::npos ||
        message.find("Package differs") != std::string::npos)
        return lv_tr("Package verification failed. Check updates and download again.");
    return std::string(lv_tr("Moonraker rejected the update:")) + " " +
           (message.empty() ? std::to_string(error.code)
                            : std::string(lv_tr(message.c_str())).substr(0, 300));
}
} // namespace

void UpdatesSettingsOverlay::show_package_progress(const std::string& stage,
                                                   const std::string& line, int step) {
    if (!package_progress_modal_)
        package_progress_modal_ = helix::ui::modal_show("vorondeck_update_progress_modal");
    if (!package_progress_modal_)
        return;
    auto set = [this](const char* name, const std::string& text) {
        if (auto* label = helix::ui::find_optional(package_progress_modal_, name))
            lv_label_set_text(label, text.c_str());
    };
    set("update_stage", stage);
    set("update_log", line.substr(0, 240));
    set("stage_count", step <= 0 ? "" : fmt::format(lv_tr("Stage {} of 3"), step));
    if (auto* spinner = helix::ui::find_optional(package_progress_modal_, "update_spinner")) {
        if (step < 0 || step == 3)
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (auto* bar = helix::ui::find_optional(package_progress_modal_, "stage_progress")) {
        lv_bar_set_value(bar, step < 0 ? 0 : step * 100 / 3, LV_ANIM_OFF);
        if (step < 0)
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
}

void UpdatesSettingsOverlay::install_test_update(const std::string& selection, bool manager_ready) {
    auto* client = get_moonraker_client();
    if (selection.empty() || vorondeck_install_busy_ ||
        (vorondeck_check_busy_ && !vorondeck_check_background_))
        return;
    if (selection != test_offer_ && selection != stable_offer_ && selection != beta_offer_) {
        show_package_progress(
            lv_tr("The selected or installed version has changed. Check updates again."), "", -1);
        return;
    }
    auto params = json::parse(selection, nullptr, false);
    const auto kind = helix::json_util::safe_string(params, "kind");
    if (!params.is_object() || (kind != "stable" && kind != "test" && kind != "beta")) {
        show_package_progress(
            lv_tr("The selected or installed version has changed. Check updates again."), "", -1);
        return;
    }
    if (!client || client->get_connection_state() != ConnectionState::CONNECTED ||
        job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("Printer or update manager is busy"), 4000);
        return;
    }
    const auto host = helix::extract_host_from_websocket_url(client->get_last_url());
    helix::invalidate_host_identity_cache();
    if (host.empty() || !helix::is_moonraker_on_same_host(host)) {
        ToastManager::instance().show(
            ToastSeverity::WARNING,
            lv_tr("Connect to this screen's local Moonraker before installing its update."), 6000);
        return;
    }
    const auto printer = Config::get_instance()->get_active_printer_id();
    auto token = object_lifetime_.token();
    vorondeck_install_busy_ = true;
    const auto attempt = ++package_update_attempt_;
    auto valid = [this, client, printer, attempt] {
        return attempt == package_update_attempt_ && client == get_moonraker_client() &&
               printer == Config::get_instance()->get_active_printer_id();
    };
    auto finish = [this, token, valid](bool success, std::string reason = "",
                                       bool outdated = false) {
        token.defer("UpdatesSettings::install_finish", [this, valid, success, reason, outdated] {
            vorondeck_install_busy_ = false;
            if (!valid()) {
                if (package_progress_modal_) {
                    helix::ui::modal_hide(package_progress_modal_);
                    package_progress_modal_ = nullptr;
                }
                test_offer_.clear();
                stable_offer_.clear();
                beta_offer_.clear();
                vorondeck_update_text_ = std::string("VoronDeck ") + HELIX_VERSION;
            } else {
                vorondeck_update_text_ = success ? lv_tr("VoronDeck updated successfully") : reason;
                show_package_progress(vorondeck_update_text_, "", success ? 3 : -1);
            }
            publish_vorondeck_status();
            if (outdated && valid()) {
                if (package_progress_modal_) {
                    helix::ui::modal_hide(package_progress_modal_);
                    package_progress_modal_ = nullptr;
                }
                repair_update_manager();
            }
        });
    };
    auto failed = [finish, token](const MoonrakerError& error) {
        token.defer("UpdatesSettings::install_error", [finish, error] {
            finish(false, package_error(error), error.code == -32601 || error.code == 404);
        });
    };
    if (!manager_ready) {
        show_package_progress(lv_tr("Checking the update manager..."), "machine.update.status", 0);
        client->send_jsonrpc(
            "machine.update.status", json::object(),
            [this, token, valid, selection, finish](const json& response) {
                token.defer("UpdatesSettings::manager_ready", [this, valid, selection, response,
                                                               finish] {
                    if (!valid()) {
                        finish(false);
                        return;
                    }
                    const auto result = helix::json_util::safe_object(response, "result");
                    const auto info = helix::json_util::safe_object(result, "version_info");
                    const auto component = helix::json_util::safe_object(info, "vorondeck");
                    if (helix::json_util::safe_int(component, "vorondeck_update_progress", 0) < 1) {
                        finish(
                            false,
                            lv_tr("Update manager is outdated. Run the VoronDeck installer again."),
                            true);
                        return;
                    }
                    vorondeck_install_busy_ = false;
                    install_test_update(selection, true);
                });
            },
            failed, 15000, true);
        return;
    }
    vorondeck_update_text_ = lv_tr("Waiting for the background check to finish...");
    show_package_progress(vorondeck_update_text_, "machine.update.vorondeck_release", 0);
    publish_vorondeck_status();
    helix::replace_method_callback(
        *client, "notify_update_response", "vorondeck_install_progress",
        [this, token, valid](const json& message) {
            if (!message.contains("params") || !message["params"].is_array() ||
                message["params"].empty())
                return;
            const auto& data = message["params"][0];
            if (!data.is_object() || !data.contains("application") ||
                !data["application"].is_string() || data["application"] != "vorondeck" ||
                !data.contains("message") || !data["message"].is_string())
                return;
            const auto line = data["message"].get<std::string>();
            token.defer("UpdatesSettings::install_progress", [this, valid, line] {
                if (!valid() || !vorondeck_install_busy_)
                    return;
                int step = 1;
                const char* stage = lv_tr("Downloading and verifying the package...");
                if (line.find("Activating") != std::string::npos) {
                    step = 2;
                    stage = lv_tr("Installing the package and restarting the screen...");
                } else if (line.find("successfully") != std::string::npos ||
                           line.find("already up to date") != std::string::npos) {
                    step = 3;
                    stage = lv_tr("VoronDeck updated successfully");
                }
                vorondeck_update_text_ = stage;
                // Closing the console hides progress, but never cancels installation.
                if (package_progress_modal_)
                    show_package_progress(vorondeck_update_text_, line, step);
                publish_vorondeck_status();
            });
        });
    params.erase("display_version");
    params.erase("notification_id");
    client->send_jsonrpc(
        helix::json_util::safe_string(params, "kind") == "test"
            ? "machine.update.vorondeck_test"
            : "machine.update.vorondeck_release",
        params,
        [finish, token](const json& response) {
            token.defer("UpdatesSettings::install_response", [finish, response] {
                const bool ok = response.contains("result") && response["result"] == "ok";
                finish(ok, ok ? ""
                              : lv_tr("Moonraker returned an unexpected update response. Check "
                                      "component versions."));
            });
        },
        failed, 1800000, true);
}

void UpdatesSettingsOverlay::repair_update_manager() {
#if defined(HELIX_PLATFORM_ESP32)
    return; // Firmware has no local Moonraker service or Python update manager.
#else
    auto* client = get_moonraker_client();
    auto* config = Config::get_instance();
    if (!client || !config || vorondeck_install_busy_ ||
        (vorondeck_check_busy_ && !vorondeck_check_background_))
        return;
    const auto printer = config->get_active_printer_id();
    const int port = config->get<int>(config->df() + "moonraker_port", 7125);
    if (port < 1 || port > 65535)
        return;
    helix::ui::ConfirmOptions options;
    options.cancel_text = lv_tr("Not now");
    options.owner_token = object_lifetime_.token();
    helix::ui::modal_confirm(
        lv_tr("Update manager"),
        lv_tr("Update the local update manager? Moonraker will restart. Printer settings are "
              "preserved. Then check updates again."),
        ModalSeverity::Warning, lv_tr("Update"),
        [this, client, printer, port] {
            if (client != get_moonraker_client() ||
                printer != Config::get_instance()->get_active_printer_id() ||
                client->get_connection_state() != ConnectionState::CONNECTED ||
                job_holds_machine(get_printer_state().print_state().get_print_lifecycle()) ||
                !helix::is_moonraker_on_same_host(
                    helix::extract_host_from_websocket_url(client->get_last_url())))
                return;
            auto& worker = helix::http::HttpExecutor::slow();
            if (!worker.running())
                return;
            vorondeck_install_busy_ = true;
            vorondeck_update_text_ = lv_tr("Updating the update manager...");
            publish_vorondeck_status();
            auto token = object_lifetime_.token();
            worker.submit([this, token, client, printer, port] {
                int code = -1;
                helix::contain_exceptions("VoronDeck update manager repair", [&] {
                    // Only a validated integer enters this constant local command.
                    helix::run_capture(
                        "python3 assets/updates/repair-vorondeck-updater.py --port " +
                            std::to_string(port) + " 2>/dev/null",
                        code);
                });
                token.defer("UpdatesSettings::repair", [this, client, printer, code] {
                    vorondeck_install_busy_ = false;
                    if (client == get_moonraker_client() &&
                        printer == Config::get_instance()->get_active_printer_id()) {
                        vorondeck_update_text_ =
                            lv_tr(code == 0 ? "Update manager ready. Check updates again."
                                            : "Update manager repair failed. Check that the "
                                              "printer is idle and local Moonraker is available.");
                    }
                    publish_vorondeck_status();
                });
            });
        },
        options);
#endif
}

void UpdatesSettingsOverlay::show_update_download_modal(bool start_immediately) {
#ifdef __ANDROID__
    // Android never runs the tarball updater. Every install intent (the Install
    // Update row and the "New Version Available" notification) opens wherever this
    // APK came from: Play's in-app update flow (or its listing) for a Play install,
    // the GitHub release for a sideload.
    if (helix::is_android_platform()) {
        const std::string installer = helix::android::installer_package();
        if (helix::android::installed_by_play(installer) && helix::android::start_play_update()) {
            spdlog::info("[UpdatesSettings] Installer '{}', starting the Play update flow",
                         installer);
            (void)start_immediately;
            return;
        }
        const std::string url = helix::android::update_url(installer, offered_release());
        spdlog::info("[UpdatesSettings] Installer '{}', opening {}", installer, url);
        if (SDL_OpenURL(url.c_str()) != 0) {
            spdlog::warn("[UpdatesSettings] Opening {} failed: {}", url, SDL_GetError());
            if (url == helix::android::kPlayStoreMarketUrl) {
                SDL_OpenURL(helix::android::kPlayStoreWebUrl);
            }
        }
        (void)start_immediately;
        return;
    }
#elif defined(HELIX_PLATFORM_X86_DESKTOP)
    // The desktop build never rewrites its own install: every install intent opens
    // the release page in the user's browser, where they download the new tarball.
    {
        const std::string url = helix::release_page_url(offered_release());
        spdlog::info("[UpdatesSettings] Desktop build, opening {}", url);
        if (SDL_OpenURL(url.c_str()) != 0) {
            spdlog::warn("[UpdatesSettings] Opening {} failed: {}", url, SDL_GetError());
            ToastManager::instance().show(ToastSeverity::WARNING,
                                          lv_tr("Could not open a browser for the update"), 4000);
        }
        (void)start_immediately;
        return;
    }
#endif

    // The modal can open before the overlay ever has (update notification).
    register_callbacks();

    if (!download_status_observer_) {
        auto& checker = UpdateChecker::instance();
        download_status_observer_ = helix::ui::observe<int>(
            checker.download_status_subject(), this,
            [](UpdatesSettingsOverlay* self, int status) { self->on_download_status(status); },
            checker.get_subjects_lifetime());
    }
    download_modal_closed_by_user_ = false;

    if (!update_download_modal_) {
        // Clear any stale Error/Complete status carried over from a prior
        // download attempt вЂ” otherwise the modal briefly flashes that
        // content before the status update below takes effect.
        UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0,
                                                         "");
        update_download_modal_ = helix::ui::modal_show("update_download_modal");
    }

    if (start_immediately) {
        // User already confirmed on the "New Version Available" notification вЂ”
        // skip the redundant Confirming state and begin the download directly.
        UpdateChecker::instance().start_download();
        return;
    }

    // Set to Confirming state with version info
    auto info = UpdateChecker::instance().get_cached_update();
    std::string text = info ? fmt::format(lv_tr("Download v{}?"), info->version)
                            : std::string(lv_tr("Download update?"));
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Confirming, 0,
                                                     text);
}

void UpdatesSettingsOverlay::on_download_status(int status) {
    if (status != static_cast<int>(UpdateChecker::DownloadStatus::Error) ||
        download_modal_closed_by_user_) {
        return;
    }
    // Asked of the stack, not the pointer: a dismissed dialog stays a valid
    // object for its exit animation, and its address can be reused after.
    const auto& stack = ModalStack::instance();
    lv_obj_t* backdrop =
        update_download_modal_ ? stack.backdrop_for(update_download_modal_) : nullptr;
    if (backdrop && !stack.is_exiting(backdrop)) {
        return;
    }
    // Reopened as-is: the status, text and error are already the failure's, and
    // the modal binds straight to them.
    spdlog::info("[UpdatesSettings] Update failed after its dialog closed - reopening it");
    update_download_modal_ = helix::ui::modal_show("update_download_modal");
}

void UpdatesSettingsOverlay::hide_update_download_modal() {
    download_modal_closed_by_user_ = true;
    if (update_download_modal_) {
        helix::ui::modal_hide(update_download_modal_);
        update_download_modal_ = nullptr;
    }
    // Reset download state
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
}

void UpdatesSettingsOverlay::sync_update_channel_rows(lv_obj_t* root, int effective_channel) {
    if (!root || effective_channel < 0) {
        return;
    }

    const auto selected = static_cast<uint32_t>(effective_channel);

    for (const char* row_name : {"row_update_channel", "row_update_channel_dev"}) {
        lv_obj_t* row = helix::ui::find_optional(root, row_name);
        if (!row) {
            continue;
        }
        // required-names: setting_dropdown_row
        lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", "UpdatesSettingsOverlay");
        if (!dropdown) {
            continue;
        }
        // A row too short for this channel is the one that is hidden right now.
        // Skipping it rather than letting LVGL clamp keeps Dev's index 2 from
        // rendering as Beta on the two-entry row if it ever became visible.
        if (selected < lv_dropdown_get_option_count(dropdown)) {
            lv_dropdown_set_selected(dropdown, selected);
        }
    }
}

} // namespace helix::settings
