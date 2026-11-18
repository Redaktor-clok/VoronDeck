#include "ui_manual_bed_screws.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "ui_callback_helpers.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include <spdlog/fmt/fmt.h>

using namespace helix;
using manual_bed_screws::Action;

namespace {
ManualBedScrewsPanel& panel() {
    return lazy_global<ManualBedScrewsPanel>("ManualBedScrewsPanel");
}
json query_params() {
    return {{"objects", {{"bed_screws", nullptr}, {"toolhead", {"homed_axes"}},
                         {"print_stats", {"state"}}, {"idle_timeout", {"state"}},
                         {"configfile", {"settings"}}, {"manual_probe", {"is_active"}},
                         {"webhooks", {"state"}}}}};
}
}

void ui_manual_bed_screws_register_callbacks() {
    register_xml_callbacks({{"on_manual_bed_screws_clicked", [](lv_event_t*) {
        panel().show(lv_display_get_screen_active(nullptr));
    }}});
}

ManualBedScrewsPanel::~ManualBedScrewsPanel() {
    if (client_ && subscription_) client_->unsubscribe_notify_update(subscription_);
    connection_observer_.reset();
    subjects_.deinit_all();
}

void ManualBedScrewsPanel::init_subjects() {
    if (subjects_initialized_) return;
    UI_MANAGED_SUBJECT_INT(phase_, 0, "manual_screws_phase", subjects_);
    UI_MANAGED_SUBJECT_INT(busy_, 1, "manual_screws_busy", subjects_);
    UI_MANAGED_SUBJECT_INT(start_allowed_, 0, "manual_screws_start_allowed", subjects_);
    UI_MANAGED_SUBJECT_INT(next_allowed_, 0, "manual_screws_next_allowed", subjects_);
    UI_MANAGED_SUBJECT_INT(abort_allowed_, 0, "manual_screws_abort_allowed", subjects_);
    UI_MANAGED_SUBJECT_STRING(status_, status_buf_, "", "manual_screws_status", subjects_);
    subjects_initialized_ = true;
}

void ManualBedScrewsPanel::register_callbacks() {
    register_xml_callbacks({
        {"manual_screws_start", [](lv_event_t*) { panel().act(Action::Start); }},
        {"manual_screws_accept", [](lv_event_t*) { panel().act(Action::Accept); }},
        {"manual_screws_adjusted", [](lv_event_t*) { panel().act(Action::Adjusted); }},
        {"manual_screws_abort", [](lv_event_t*) { panel().act(Action::Abort); }},
        {"manual_screws_refresh", [](lv_event_t*) { panel().refresh(); }},
    });
}

void ManualBedScrewsPanel::on_activate() {
    OverlayBase::on_activate();
    pending_ = false;
    client_ = get_moonraker_client();
    const auto printer = Config::get_instance()->get_active_printer_id();
    auto token = lifetime_.token();
    if (client_ && !subscription_) {
        subscription_ = client_->register_notify_update([this, token, printer](const json& message) {
            if (!message.contains("params") || !message["params"].is_array() ||
                message["params"].empty()) return;
            const auto& changed = message["params"][0];
            if (!changed.contains("bed_screws") && !changed.contains("idle_timeout") &&
                !changed.contains("print_stats") && !changed.contains("manual_probe") &&
                !changed.contains("webhooks") &&
                !(changed.contains("toolhead") && changed["toolhead"].contains("homed_axes"))) return;
            token.defer("ManualBedScrews::status", [this, printer] {
                if (printer == Config::get_instance()->get_active_printer_id() && !pending_)
                    refresh();
            });
        });
    }
    connection_observer_ = helix::ui::observe<int>(
        get_printer_state().network_state().get_moonraker_connection_state_subject(), this,
        [](ManualBedScrewsPanel* self, int connected) {
            ++self->generation_;
            self->pending_ = false;
            if (connected == 1) self->refresh();
            else self->unavailable(lv_tr("Printer is disconnected"));
        }, get_printer_state().get_subjects_lifetime());
    refresh();
}

void ManualBedScrewsPanel::on_deactivating(DeactivateReason) {
    ++generation_;
    pending_ = false;
    connection_observer_.reset();
    if (client_ && subscription_) client_->unsubscribe_notify_update(subscription_);
    subscription_ = 0;
    client_ = nullptr;
    // Navigation is not an implicit ABORT: reopening reads Klipper's active helper.
}

void ManualBedScrewsPanel::unavailable(const char* text) {
    lv_subject_set_int(&busy_, 0);
    lv_subject_set_int(&start_allowed_, 0);
    lv_subject_set_int(&next_allowed_, 0);
    lv_subject_set_int(&abort_allowed_, 0);
    lv_subject_copy_string(&status_, text);
}

void ManualBedScrewsPanel::request(const std::string& method, const json& params,
                                 std::function<void(const json&)> done) {
    auto* client = get_moonraker_client();
    if (pending_) return;
    if (!client || client != client_ || client->get_connection_state() != ConnectionState::CONNECTED) {
        unavailable(lv_tr("Printer is disconnected"));
        return;
    }
    pending_ = true;
    lv_subject_set_int(&busy_, 1);
    const auto generation = ++generation_;
    const auto printer = Config::get_instance()->get_active_printer_id();
    auto token = lifetime_.token();
    client->send_jsonrpc(method, params,
        [this, token, generation, printer, client, done](const json& response) {
            token.defer("ManualBedScrews::response", [this, generation, printer, client, done, response] {
                if (generation != generation_ || client != get_moonraker_client() ||
                    client->get_connection_state() != ConnectionState::CONNECTED ||
                    printer != Config::get_instance()->get_active_printer_id()) return;
                pending_ = false;
                lv_subject_set_int(&busy_, 0);
                try {
                    done(response.contains("result") ? response.at("result") : response);
                } catch (const json::exception&) {
                    unavailable(lv_tr("Invalid Moonraker response"));
                }
            });
        },
        [this, token, generation, printer, client](const MoonrakerError&) {
            token.defer("ManualBedScrews::error", [this, generation, printer, client] {
                if (generation != generation_ || client != get_moonraker_client() ||
                    printer != Config::get_instance()->get_active_printer_id()) return;
                pending_ = false;
                unavailable(lv_tr("Request failed. Refresh to check the printer state."));
            });
        }, 120000);
}

void ManualBedScrewsPanel::display(const json& status) {
    const auto& bed = status.at("bed_screws");
    const bool active = bed.at("is_active").get<bool>();
    lv_subject_set_int(&phase_, active ? 1 : 0);
    lv_subject_set_int(&start_allowed_, manual_bed_screws::allowed(Action::Start, status));
    lv_subject_set_int(&next_allowed_, manual_bed_screws::allowed(Action::Accept, status));
    lv_subject_set_int(&abort_allowed_, manual_bed_screws::allowed(Action::Abort, status));
    std::string text;
    if (active) {
        const int index = bed.at("current_screw").get<int>();
        if (index < 0 || index > 98) throw json::out_of_range::create(401, "Invalid screw", &bed);
        const auto& settings = status.at("configfile").at("settings").at("bed_screws");
        int screw = index + 1;
        const bool fine = bed.value("state", "adjust") == "fine";
        if (fine) {
            int fine_index = 0;
            screw = 0;
            for (int i = 1; i <= 99; ++i) {
                if (settings.contains("screw" + std::to_string(i) + "_fine_adjust") && fine_index++ == index) {
                    screw = i;
                    break;
                }
            }
            if (!screw) { unavailable(lv_tr("Invalid Moonraker response")); return; }
        }
        const std::string key = "screw" + std::to_string(screw);
        std::string name = settings.value(key + "_name", key);
        text = fmt::format("{} {}\n{}\n{}: {}", lv_tr("Screw"), screw, name,
                          lv_tr("Stage"), lv_tr(fine ?
                          "Fine adjustment" : "Main adjustment"));
    } else {
        text = lv_tr(manual_bed_screws::allowed(Action::Start, status) ?
                     "Ready. Start moves the nozzle to the first screw." :
                     "Home all axes and finish other operations before starting.");
    }
    lv_subject_copy_string(&status_, text.c_str());
}

void ManualBedScrewsPanel::refresh() {
    request("printer.objects.query", query_params(), [this](const json& result) {
        display(result.at("status"));
    });
}

void ManualBedScrewsPanel::act(Action action) {
    request("printer.objects.query", query_params(), [this, action](const json& result) {
        const auto& status = result.at("status");
        display(status);
        if (!manual_bed_screws::allowed(action, status)) return;
        request("printer.gcode.script", {{"script", manual_bed_screws::command(action)}},
                [this](const json&) { refresh(); });
    });
}
