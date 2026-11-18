// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_settings_printer_maintenance.h"
#include "app_globals.h"
#include "config.h"
#include "i_moonraker_client.h"
#include "printer_state.h"
#include "print_history_manager.h"
#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_update_queue.h"
#include "static_subject_registry.h"
#include <algorithm>
#include <spdlog/fmt/fmt.h>

namespace helix::settings {
PrinterMaintenanceOverlay::~PrinterMaintenanceOverlay() { deinit_subjects(); }
void PrinterMaintenanceOverlay::init_subjects() {
    if (!init_subjects_guarded([this] {
        UI_MANAGED_SUBJECT_INT(section_,0,"maintenance_section",subjects_);
        UI_MANAGED_SUBJECT_INT(busy_,0,"maintenance_busy",subjects_);
        UI_MANAGED_SUBJECT_INT(count_,0,"maintenance_count",subjects_);
        UI_MANAGED_SUBJECT_STRING(title_,title_buf_,"","maintenance_title",subjects_);
        UI_MANAGED_SUBJECT_STRING(status_,status_buf_,"","maintenance_status",subjects_);
    })) return;
    StaticSubjectRegistry::instance().register_deinit("PrinterMaintenanceOverlay", [] {
        if (auto* o=lazy_global_if_exists<PrinterMaintenanceOverlay>()) o->deinit_subjects();
    });
}
void PrinterMaintenanceOverlay::deinit_subjects() {
    ++request_generation_;
    names_.reclaim(); descriptions_.reclaim(); enabled_.reclaim();
    deinit_subjects_base(subjects_);
}
void PrinterMaintenanceOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_printer_history_reset",[](lv_event_t*) { get_printer_maintenance_overlay().confirm_reset(false); }},
        {"on_printer_totals_reset",[](lv_event_t*) { get_printer_maintenance_overlay().confirm_reset(true); }},
        {"on_printer_updates_refresh",[](lv_event_t*) { get_printer_maintenance_overlay().refresh(true); }},
        {"on_printer_component_update",[](lv_event_t* e) {
            get_printer_maintenance_overlay().confirm_update(ui::event_user_int(e).value_or(-1));
        }},
    });
}
void PrinterMaintenanceOverlay::open(lv_obj_t* parent, bool updates) {
    init_subjects();
    auto id=Config::get_instance()->get_active_printer_id();
    if (id!=printer_id_ || request_client_!=get_moonraker_client()) {
        ++request_generation_; printer_id_=id; components_.clear(); server_busy_=false;
        lv_subject_set_int(&count_,0); lv_subject_set_int(&busy_,0);
    }
    lv_subject_set_int(&section_,updates ? 1 : 0);
    show(parent);
    lv_subject_copy_string(&title_,lv_tr(updates ? "Component updates" : "Printer reset"));
}
void PrinterMaintenanceOverlay::on_activate() {
    OverlayBase::on_activate();
    if (!lv_subject_get_int(&busy_)) refresh();
}
void PrinterMaintenanceOverlay::status(const std::string& text) {
    lv_subject_copy_string(&status_,text.c_str());
}
bool PrinterMaintenanceOverlay::may_act() {
    return subjects_initialized_ && !lv_subject_get_int(&busy_) &&
           printer_id_==Config::get_instance()->get_active_printer_id() &&
           get_moonraker_client() &&
           get_moonraker_client()->get_connection_state()==ConnectionState::CONNECTED &&
           lv_subject_get_int(get_printer_state().print_state().get_job_holds_machine_subject())==0 &&
           !get_printer_state().is_paused();
}
void PrinterMaintenanceOverlay::send(const maintenance::Request& request,
                                   std::function<void(const json&)> done, uint32_t timeout) {
    auto* client=get_moonraker_client();
    if (!client || client->get_connection_state()!=ConnectionState::CONNECTED) {
        status(lv_tr("Printer is disconnected")); return;
    }
    if (lv_subject_get_int(&busy_)) return;
    request_client_=client;
    lv_subject_set_int(&busy_,1);
    status(lv_tr("Please wait..."));
    auto token=lifetime_.token();
    const auto generation=++request_generation_;
    const auto printer=printer_id_;
    client->send_jsonrpc(request.method,request.params,
        [this,token,generation,printer,client,done](const json& response) {
            token.defer("PrinterMaintenance::success",[this,generation,printer,client,done,response] {
                if (!subjects_initialized_ || generation!=request_generation_ || client!=get_moonraker_client() ||
                    printer!=Config::get_instance()->get_active_printer_id()) return;
                lv_subject_set_int(&busy_,0);
                if (!response.contains("result")) { status(lv_tr("Invalid Moonraker response")); return; }
                try { done(response["result"]); }
                catch (const json::exception&) { status(lv_tr("Invalid Moonraker response")); }
            });
        },
        [this,token,generation,printer,client](const MoonrakerError& error) {
            const auto message=error.user_message();
            token.defer("PrinterMaintenance::error",[this,generation,printer,client,message] {
                if (!subjects_initialized_ || generation!=request_generation_ || client!=get_moonraker_client() ||
                    printer!=Config::get_instance()->get_active_printer_id()) return;
                lv_subject_set_int(&busy_,0);
                status(std::string(lv_tr("Request not confirmed. Check status before retrying."))+"\n"+message);
            });
        },timeout);
}
void PrinterMaintenanceOverlay::refresh(bool check_remote) {
    if (lv_subject_get_int(&busy_)) return;
    if (lv_subject_get_int(&section_)==0) {
        send({"server.history.totals",json::object()},[this](const json& result) {
            auto totals=result.value("job_totals",json::object());
            const auto jobs=totals.value("total_jobs",0);
            const double hours=totals.value("total_time",0.0)/3600.0;
            status(fmt::format("{}: {} · {}: {:.1f} {}",lv_tr("Jobs"),jobs,lv_tr("Total time"),hours,lv_tr("h")));
        });
    } else {
        if (check_remote && !may_act()) {
            status(lv_tr("Printer or update manager is busy")); return;
        }
        send({check_remote && !server_busy_ ? "machine.update.refresh" : "machine.update.status",json::object()},
             [this](const json& result) { publish(result); },120000);
    }
}
void PrinterMaintenanceOverlay::publish(const json& result) {
    server_busy_=result.value("busy",false);
    components_=maintenance::parse_components(result);
    names_.ensure_size(components_.size()); descriptions_.ensure_size(components_.size()); enabled_.ensure_size(components_.size());
    for (size_t i=0;i<components_.size();++i) {
        const auto& c=components_[i];
        names_.set_string(i,c.name);
        std::string version=c.name=="system" ? std::string(lv_tr("Packages"))+": "+c.available
                                             : c.current+" / "+c.available;
        descriptions_.set_string(i,version+"\n"+lv_tr(c.note.c_str()));
        enabled_.set_int(i,c.can_update && !server_busy_ ? 1 : 0);
    }
    lv_subject_set_int(&count_,static_cast<int>(components_.size()));
    status(lv_tr(server_busy_ ? "Printer or update manager is busy" :
                 components_.empty() ? "No update components configured" : "Components from Moonraker"));
}
void PrinterMaintenanceOverlay::confirm_reset(bool totals) {
    if (!may_act()) { status(lv_tr("Printer or update manager is busy")); return; }
    ui::ConfirmOptions options; options.owner_token=lifetime_.token(); options.destructive=true;
    const auto printer=printer_id_;
    ui::modal_confirm(lv_tr(totals ? "Reset print statistics?" : "Clear print history?"),
        lv_tr(totals ? "Reset accumulated time, job and filament totals. History entries and printer configuration are kept."
                     : "Delete all print history entries. G-code files, accumulated totals and printer configuration are kept."),
        ModalSeverity::Warning,lv_tr("Reset"),[this,totals,printer] {
            if (!may_act() || printer!=printer_id_) return;
            send(maintenance::reset_request(totals),[this,totals](const json&) {
                if (auto* manager=get_print_history_manager()) {
                    manager->invalidate();
                    manager->ensure_loaded(HistoryScope::RECENT);
                }
                status(lv_tr(totals ? "Print statistics reset" : "Print history cleared"));
            });
        },options);
}
void PrinterMaintenanceOverlay::confirm_update(int index) {
    if (!may_act() || server_busy_ || index<0 || static_cast<size_t>(index)>=components_.size()) return;
    const auto component=components_[index];
    if (!component.can_update) return;
    const auto printer=printer_id_;
    ui::ConfirmOptions options; options.owner_token=lifetime_.token();
    ui::modal_confirm(lv_tr("Update component?"),
        (component.name+"\n"+lv_tr("The component may restart. Wait for the operation to finish.")).c_str(),
        ModalSeverity::Warning,lv_tr("Update"),[this,component,printer] {
            if (!may_act() || server_busy_ || printer!=printer_id_) return;
            // Recheck the server immediately before the confirmed update.
            send({"machine.update.status",json::object()},[this,component](const json& result) {
                publish(result);
                if (!may_act() || server_busy_) return;
                auto found=std::find_if(components_.begin(),components_.end(),[&](const auto& c) { return c.name==component.name; });
                if (found==components_.end() || !found->can_update) return;
                send(maintenance::update_request(component.name),[this](const json&) {
                    status(lv_tr("Update completed. Check component versions."));
                    refresh();
                },component.name=="vorondeck" ? 2100000 : 600000);
            });
        },options);
}
}
