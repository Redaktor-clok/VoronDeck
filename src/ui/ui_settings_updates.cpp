// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_updates.h"

#include "ui_callback_helpers.h"
#include "ui_info_qr_modal.h"
#include "ui_modal.h"
#include "ui_settings_printer_maintenance.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "config.h"
#include "helix_version.h"
#include "host_identity.h"
#include "http_executor.h"
#include "i_moonraker_client.h"
#include "json_fwd.h"
#include "lvgl.h"
#include "platform_info.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
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
        {"on_vorondeck_check_updates",
         [](lv_event_t*) { get_updates_settings_overlay().check_vorondeck_updates(); }},
        {"on_vorondeck_component_updates",
         [](lv_event_t*) { get_printer_maintenance_overlay().open(lv_screen_active(), true); }},
        {"on_vorondeck_test_update",
         [](lv_event_t*) { get_updates_settings_overlay().confirm_test_update(); }},
        {"on_about_update_channel_changed",
         [](lv_event_t* e) {
             lv_obj_t* dropdown = lv_event_get_current_target_obj(e);
             const int index = helix::ui::event_selected(e);

             if (index == 2 && helix::config_trust::read_update_urls().dev_url.empty()) {
                 spdlog::warn("[UpdatesSettings] Dev channel selected but no dev_url configured");
                 int current = SystemSettingsManager::instance().get_update_channel();
                 lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current));
                 ToastManager::instance().show(
                     ToastSeverity::WARNING,
                     lv_tr("Dev channel requires dev_url in update_urls.json"), 3000);
                 return;
             }

             spdlog::info("[UpdatesSettings] Update channel changed: {} ({})", index,
                          index == 0 ? "Stable" : (index == 1 ? "Beta" : "Dev"));
             SystemSettingsManager::instance().set_update_channel(index);
             // Drops the previous channel's cached verdict, re-snapshots the config
             // for the debug bundle's off-thread reader, and starts a fresh check.
             // The re-check matters when the new channel is BEHIND this install and
             // the only way forward is an explicit switch back.
             UpdateChecker::instance().on_channel_changed();
         }},
        {"on_about_check_updates_clicked",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Check for updates requested");
             UpdateChecker::instance().check_for_updates();
         }},
        {"on_about_install_update_clicked",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Install update requested");

             // Moving backward is never what someone means by "install update", so it is
             // never the one-tap path. Settings written by the newer build are not
             // migrated back either: the older build reads what it recognizes and
             // leaves the rest alone.
             auto cached = UpdateChecker::instance().get_cached_update();
             if (cached && cached->is_downgrade) {
                 spdlog::info(
                     "[UpdatesSettings] Install target v{} is older than installed v{}, confirming",
                     cached->version, HELIX_VERSION);
                 std::string msg =
                     fmt::format(lv_tr("This channel offers v{}, older than the installed v{}. "
                                       "Anything added since then will be removed."),
                                 cached->version, HELIX_VERSION);
                 helix::ui::modal_confirm(
                     lv_tr("Install Older Version?"), msg.c_str(), ModalSeverity::Warning,
                     lv_tr("Install"),
                     [] { get_updates_settings_overlay().show_update_download_modal(); });
             } else {
                 get_updates_settings_overlay().show_update_download_modal();
             }
         }},
        {"on_about_updates_unavailable_clicked",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Updates-unavailable notice tapped");

             // Reached only when self_update_supported() is false and updates are not
             // firmware-managed: this box can see that a new version exists but cannot
             // apply one itself. The command is the whole payload; without it the row
             // states a problem and offers nothing. The QR points at the docs for the
             // longer story.
             const std::string url = helix::docs_url("upgrading/");
             helix::ui::InfoQrModal::show_owned({
                 .icon = "console",
                 .title = lv_tr("Update from a Terminal"),
                 // No command in here on purpose. The one-liner is not portable across the
                 // platforms this runs on (BusyBox firmwares such as K1, K2, AD5M and CC1
                 // ship ash with no bash, and several have wget but no curl), so any single
                 // literal would be wrong somewhere and baked into a binary. The docs can
                 // say the right thing per platform and be corrected without a release.
                 .message = lv_tr("Run the HelixScreen installer with --update from a "
                                  "terminal on this printer. Scan for the command for "
                                  "your platform."),
                 .url = url,
                 .url_text = helix::docs_url_text(url),
             });
         }},
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
    const bool busy = vorondeck_check_busy_ || vorondeck_install_busy_;
    if (auto* group = helix::ui::find_optional(overlay_root_, "group_release_offers")) {
        lv_obj_set_flag(group, LV_OBJ_FLAG_HIDDEN, test_offer_.empty() && stable_version_.empty());
    }
    for (const auto* name : {"row_stable_release", "row_test_release"}) {
        if (auto* row = helix::ui::find_optional(overlay_root_, name)) {
            const bool stable = std::string(name) == "row_stable_release";
            const bool empty = stable ? stable_version_.empty() : test_offer_.empty();
            lv_obj_set_flag(row, LV_OBJ_FLAG_HIDDEN, empty);
            lv_obj_set_state(row, LV_STATE_DISABLED, busy);
            if (!empty) {
                const auto version =
                    stable ? stable_version_
                           : json::parse(test_offer_).value("display_version", std::string{});
                if (auto* label = helix::ui::find_optional(row, "label")) {
                    const auto text =
                        std::string(lv_tr(stable ? "Stable release" : "Test release")) + " · " +
                        version;
                    lv_label_set_text(label, text.c_str());
                    lv_obj_set_style_text_color(
                        label, theme_manager_get_color(stable ? "success" : "warning"), 0);
                }
            }
        }
    }
    if (auto* label = helix::ui::find_optional(overlay_root_, "vorondeck_update_status")) {
        lv_label_set_text(label, vorondeck_update_text_.c_str());
    }
    if (auto* row = helix::ui::find_optional(overlay_root_, "row_vorondeck_check")) {
        if (busy)
            lv_obj_add_state(row, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(row, LV_STATE_DISABLED);
    }
}

void UpdatesSettingsOverlay::check_vorondeck_updates(bool background) {
    if (vorondeck_check_busy_ || vorondeck_install_busy_)
        return;
    const int selected_channel = SystemSettingsManager::instance().get_update_channel();
    if (selected_channel != 0 && selected_channel != 1)
        return;
    if (background && (is_wizard_active() ||
                       job_holds_machine(get_printer_state().print_state().get_print_lifecycle())))
        return;
    auto& worker = helix::http::HttpExecutor::slow();
    if (!worker.running())
        return;
    vorondeck_check_busy_ = true;
    test_offer_.clear();
    stable_version_.clear();
    vorondeck_update_text_ = lv_tr("Checking VoronDeck updates...");
    publish_vorondeck_status();
    // Finish the object-owned check even if its overlay is closed meanwhile.
    auto token = object_lifetime_.token();
    worker.submit([this, token, background, selected_channel]() {
        json result = {{"status", "error"}};
        try {
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
                auto parsed = json::parse(output);
                if (parsed.is_object())
                    result = std::move(parsed);
            }
        } catch (const std::exception&) {
            // The UI gets a bounded generic error, never Git/SSH diagnostics.
        }
        token.defer("UpdatesSettings::vorondeck_check", [this, result, background,
                                                         selected_channel]() {
            vorondeck_check_busy_ = false;
            if (selected_channel != SystemSettingsManager::instance().get_update_channel()) {
                vorondeck_update_text_ = std::string("VoronDeck ") + HELIX_VERSION;
                publish_vorondeck_status();
                return;
            }
            const auto state = result.value("status", std::string("error"));
            if (state == "current" || state == "available") {
                const auto installed = result.value("installed_version", std::string{});
                const auto available = result.value("available_version", std::string{});
                vorondeck_update_text_ =
                    std::string("VoronDeck ") + installed + "\n" +
                    lv_tr(state == "current" ? "VoronDeck is up to date"
                                             : "A new VoronDeck package is available");
                if (state == "available") {
                    vorondeck_update_text_ +=
                        "\n" + available + " В· " + result.value("available_build", std::string{}) +
                        "\n" + lv_tr("Open Printer component updates to install VoronDeck.");
                }
            } else {
                vorondeck_update_text_ =
                    std::string("VoronDeck ") + HELIX_VERSION + "\n" +
                    lv_tr(state == "not_installed"
                              ? "Update checks are available on an installed VoronDeck screen"
                              : "Update check failed. Check the network and beta SSH access.");
            }
            if (auto channels = result.find("channels");
                channels != result.end() && channels->is_object()) {
                for (const auto* name : {"stable", "beta"}) {
                    const auto channel = channels->value(name, json::object());
                    const auto version = channel.value("version", std::string{});
                    if (std::string(name) == "stable" && !version.empty())
                        stable_version_ = version;
                    vorondeck_update_text_ +=
                        "\n" +
                        std::string(lv_tr(name == std::string("stable") ? "Stable" : "Beta")) +
                        ": ";
                    vorondeck_update_text_ +=
                        !version.empty()
                            ? version
                            : lv_tr(channel.value("status", std::string{}) == "access_required"
                                        ? "Beta access required"
                                        : "No release available or access denied");
                }
            }
            if (auto offers = result.find("offers"); offers != result.end() && offers->is_array()) {
                for (const auto& offer : *offers) {
                    if (!offer.is_object())
                        continue;
                    if (offer.value("kind", std::string{}) == "stable") {
                        stable_version_ = offer.value("display_version", std::string{});
                    } else if (offer.value("kind", std::string{}) == "test") {
                        auto selected = offer;
                        selected["installed_source"] =
                            result.value("installed_source", std::string{});
                        test_offer_ = selected.dump();
                    }
                }
            }
            if (result.value("test_status", std::string{}) == "unavailable") {
                vorondeck_update_text_ +=
                    "\n" + std::string(lv_tr("Test release check failed. Try again later."));
            }
            publish_vorondeck_status();
            // The helper authenticates the active channel and excludes downgrades.
            const auto test = test_offer_.empty() ? json::object() : json::parse(test_offer_);
            const auto notice =
                test.value("notification_id", result.value("notification_id", std::string{}));
            auto* config = Config::get_instance();
            if (background && !notice.empty() && config && !is_wizard_active() &&
                !job_holds_machine(get_printer_state().print_state().get_print_lifecycle()) &&
                config->get<std::string>("/update/vorondeck_notified_package", "") != notice) {
                const auto channel = result.value("active_channel", std::string{});
                const auto message = fmt::format(
                    lv_tr("VoronDeck {} is available ({})"),
                    test.value("display_version", result.value("available_version", std::string{})),
                    lv_tr(!test.empty()       ? "Test release"
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
}

void UpdatesSettingsOverlay::confirm_test_update() {
    if (test_offer_.empty() || vorondeck_check_busy_ || vorondeck_install_busy_)
        return;
    const auto selection = test_offer_;
    auto* client = get_moonraker_client();
    const auto printer = Config::get_instance()->get_active_printer_id();
    const auto version = json::parse(selection).value("display_version", std::string{});
    const auto message = fmt::format(
        lv_tr("Install VoronDeck {}? This is a test release and may contain errors."), version);
    helix::ui::ConfirmOptions options;
    options.cancel_text = lv_tr("Not now");
    options.owner_token = object_lifetime_.token();
    helix::ui::modal_confirm(
        lv_tr("Test release"), message.c_str(), ModalSeverity::Warning, lv_tr("Update"),
        [this, selection, client, printer] {
            if (client != get_moonraker_client() ||
                printer != Config::get_instance()->get_active_printer_id())
                return;
            install_test_update(selection);
        },
        options);
}

void UpdatesSettingsOverlay::install_test_update(const std::string& selection) {
    auto* client = get_moonraker_client();
    if (selection != test_offer_ || vorondeck_install_busy_ || vorondeck_check_busy_)
        return;
    if (!client || client->get_connection_state() != ConnectionState::CONNECTED ||
        job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      lv_tr("Printer or update manager is busy"), 4000);
        return;
    }
    // Offers describe this screen's package, never a remote printer's screen.
    const auto host = helix::extract_host_from_websocket_url(client->get_last_url());
    helix::invalidate_host_identity_cache();
    if (host.empty() || !helix::is_moonraker_on_same_host(host)) {
        ToastManager::instance().show(
            ToastSeverity::WARNING,
            lv_tr("Connect to this screen's local Moonraker before installing its update."), 6000);
        return;
    }
    auto params = json::parse(selection);
    params.erase("display_version");
    params.erase("notification_id");
    const auto printer = Config::get_instance()->get_active_printer_id();
    auto token = object_lifetime_.token();
    vorondeck_install_busy_ = true;
    vorondeck_update_text_ = lv_tr("Downloading and verifying VoronDeck. The screen will restart.");
    publish_vorondeck_status();
    auto finish = [this, token, client, printer](bool success, bool outdated = false) {
        token.defer("UpdatesSettings::test_install", [this, client, printer, success, outdated] {
            vorondeck_install_busy_ = false;
            if (client != get_moonraker_client() ||
                printer != Config::get_instance()->get_active_printer_id()) {
                test_offer_.clear();
                stable_version_.clear();
                vorondeck_update_text_ = std::string("VoronDeck ") + HELIX_VERSION;
            } else {
                vorondeck_update_text_ = lv_tr(
                    outdated ? "Update manager is outdated. Run the VoronDeck installer again."
                    : success
                        ? "VoronDeck updated successfully"
                        : "Update was not confirmed. Check component updates before retrying.");
            }
            publish_vorondeck_status();
        });
    };
    client->send_jsonrpc(
        "machine.update.vorondeck_test", params,
        [finish](const json& response) {
            finish(response.contains("result") && response["result"] == "ok");
        },
        [finish](const MoonrakerError& error) {
            finish(false, error.code == -32601 || error.code == 404);
        },
        1800000);
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
