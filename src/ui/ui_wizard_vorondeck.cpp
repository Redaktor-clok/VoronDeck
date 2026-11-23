// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_wizard_vorondeck.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_overlay_printer_image.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_input_shaper.h"
#include "ui_settings_appearance.h"
#include "ui_settings_display.h"
#include "ui_text_entry_modal.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "printer_image_manager.h"
#include "printer_name_sync.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "wizard_config_paths.h"

using namespace helix;

WizardAppearanceStep* get_wizard_appearance_step() {
    return &helix::lazy_global<WizardAppearanceStep>("WizardAppearanceStep");
}
WizardChecksStep* get_wizard_checks_step() {
    return &helix::lazy_global<WizardChecksStep>("WizardChecksStep");
}

void WizardAppearanceStep::init_subjects() {
    if (initialized_)
        return;
    UI_MANAGED_SUBJECT_STRING(image_, image_buf_, "", "wizard_appearance_image", subjects_);
    UI_MANAGED_SUBJECT_STRING(name_, name_buf_, "", "wizard_appearance_name", subjects_);
    initialized_ = true;
}
void WizardAppearanceStep::refresh() {
    if (!initialized_)
        return;
    auto& images = PrinterImageManager::instance();
    auto path = images.get_displayed_image_path(lv_display_get_horizontal_resolution(nullptr));
    lv_subject_copy_string(&image_, path.c_str());
    lv_subject_copy_string(&name_, get_printer_display_name().c_str());
}
void WizardAppearanceStep::rename() {
    auto* config = Config::get_instance();
    const auto prefix = config->df();
    ui::TextEntryModal::Config dialog;
    dialog.title = lv_tr("Printer Name");
    dialog.initial = config->get<std::string>(prefix + wizard::PRINTER_NAME, "");
    dialog.max_length = 64;
    dialog.validate = [](const std::string& value) {
        return value.find_first_not_of(" \t\r\n") == std::string::npos
                   ? std::string(lv_tr("Enter a printer name"))
                   : std::string{};
    };
    dialog.on_accept = [prefix](const std::string& value) {
        auto* current = Config::get_instance();
        if (current->df() != prefix)
            return;
        const auto previous = current->get<std::string>(prefix + wizard::PRINTER_NAME, "");
        current->set<std::string>(prefix + wizard::PRINTER_NAME, value);
        if (!current->save()) {
            current->set<std::string>(prefix + wizard::PRINTER_NAME, previous);
            NOTIFY_ERROR(lv_tr("Failed to save configuration"));
            return;
        }
        PrinterNameSync::write_back(get_moonraker_api(), value);
        get_printer_state().set_active_printer_name(value);
        get_wizard_appearance_step()->refresh();
    };
    ui::TextEntryModal::show_owned(std::move(dialog));
}
void WizardAppearanceStep::register_callbacks() {
    register_xml_callbacks({
        {"on_wizard_image",
         [](lv_event_t*) { settings::get_printer_image_overlay().show(lv_screen_active()); }},
        {"on_wizard_theme",
         [](lv_event_t*) { settings::get_appearance_settings_overlay().show(lv_screen_active()); }},
        {"on_wizard_name", [](lv_event_t*) { get_wizard_appearance_step()->rename(); }},
    });
}
lv_obj_t* WizardAppearanceStep::create(lv_obj_t* parent) {
    init_subjects();
    auto& images = PrinterImageManager::instance();
    image_observer_ = ui::observe<int>(
        images.get_image_changed_subject(), this,
        [](WizardAppearanceStep* self, int) { self->refresh(); }, images.get_subjects_lifetime());
    refresh();
    return static_cast<lv_obj_t*>(lv_xml_create(parent, component_name(), nullptr));
}
void WizardAppearanceStep::cleanup() {
    image_observer_.reset();
}

void WizardChecksStep::init_subjects() {
    if (initialized_)
        return;
    static const char* names[] = {"wizard_check_pid_done", "wizard_check_mesh_done",
                                  "wizard_check_zoffset_done", "wizard_check_shaper_done"};
    for (size_t i = 0; i < completed_.size(); ++i) {
        UI_MANAGED_SUBJECT_INT(completed_[i], 0, names[i], subjects_);
    }
    initialized_ = true;
}

bool WizardChecksStep::is_completed(Tool tool) {
    return initialized_ && session_root_ && printer_ == Config::get_instance()->df() &&
           lv_subject_get_int(&completed_[static_cast<size_t>(tool)]) == 1;
}

void WizardChecksStep::clear_results() {
    attempts_.fill(0);
    for (auto& subject : completed_)
        lv_subject_set_int(&subject, 0);
}

void WizardChecksStep::begin_session(lv_obj_t* root, const std::string& printer) {
    init_subjects();
    if (session_root_.get() != root || !root || printer_ != printer) {
        cleanup();
        clear_results();
    }
    session_root_ = root;
    printer_ = printer;
}

void WizardChecksStep::receive(Tool tool, const helix::CalibrationReceipt::Event& event,
                               const std::string& current_printer) {
    if (!session_root_)
        return;
    if (printer_ != current_printer) {
        clear_results();
        printer_ = current_printer;
    }
    if (event.ticket.printer != printer_)
        return;
    const auto index = static_cast<size_t>(tool);
    if (event.phase == helix::CalibrationReceipt::Phase::Running) {
        attempts_[index] = event.ticket.attempt;
        lv_subject_set_int(&completed_[index], 0);
    } else if (attempts_[index] != 0 && attempts_[index] == event.ticket.attempt) {
        lv_subject_set_int(&completed_[index],
                           event.phase == helix::CalibrationReceipt::Phase::Succeeded ? 1 : 0);
        attempts_[index] = 0;
    }
}

void WizardChecksStep::watch(Tool tool, const helix::CalibrationReceipt& receipt) {
    const auto index = static_cast<size_t>(tool);
    attempts_[index] = 0;
    // Observe invokes once on attachment: cached/running results from before this
    // opening are not receipts for a new attempt in this wizard.
    receipt_observers_[index] = receipt.observe(
        this, [tool, initial = true](WizardChecksStep* self,
                                     const helix::CalibrationReceipt::Event& event) mutable {
            if (initial) {
                initial = false;
                return;
            }
            self->receive(tool, event, Config::get_instance()->df());
        });
}

void WizardChecksStep::cleanup() {
    for (auto& observer : receipt_observers_)
        observer.reset();
    attempts_.fill(0);
}

void WizardChecksStep::register_callbacks() {
#if !defined(HELIX_PLATFORM_ESP32)
    register_xml_callbacks({
        {"on_wizard_pid",
         [](lv_event_t*) {
             if (!lv_subject_get_int(
                     get_printer_state().network_state().get_nav_buttons_enabled_subject()))
                 return;
             auto* api = get_moonraker_api();
             if (!api || api->hardware().heaters().empty())
                 return;
             auto& panel = get_global_pid_cal_panel();
             panel.set_api(api);
             get_wizard_checks_step()->watch(Tool::Pid, panel.calibration_receipt());
             panel.show(lv_screen_active());
         }},
        {"on_wizard_mesh",
         [](lv_event_t*) {
             if (!lv_subject_get_int(
                     get_printer_state().network_state().get_nav_buttons_enabled_subject()))
                 return;
             auto* api = get_moonraker_api();
             if (api && api->hardware().has_bed_mesh()) {
                 get_wizard_checks_step()->watch(Tool::Mesh,
                                                 get_global_bed_mesh_panel().calibration_receipt());
                 ui::open_bed_mesh_panel(lv_screen_active());
             }
         }},
        {"on_wizard_zoffset",
         [](lv_event_t*) {
             if (!lv_subject_get_int(
                     get_printer_state().network_state().get_nav_buttons_enabled_subject()))
                 return;
             auto* api = get_moonraker_api();
             if (!api || !api->hardware().has_z_offset_calibration())
                 return;
             auto& panel = get_global_zoffset_cal_panel();
             panel.set_api(api);
             get_wizard_checks_step()->watch(Tool::ZOffset, panel.calibration_receipt());
             panel.show(lv_screen_active());
         }},
        {"on_wizard_shaper",
         [](lv_event_t*) {
             if (!lv_subject_get_int(
                     get_printer_state().network_state().get_nav_buttons_enabled_subject()))
                 return;
             auto* api = get_moonraker_api();
             if (!api || !api->hardware().has_accelerometer())
                 return;
             auto& panel = get_global_input_shaper_panel();
             if (!panel.get_root())
                 panel.set_api(get_moonraker_client(), api);
             get_wizard_checks_step()->watch(Tool::Shaper, panel.calibration_receipt());
             panel.show(lv_screen_active());
         }},
    });
#endif
}
lv_obj_t* WizardChecksStep::create(lv_obj_t* parent) {
    auto* root = parent;
    while (root && !ui_wizard_is_container(root))
        root = lv_obj_get_parent(root);
    begin_session(root, Config::get_instance()->df());
    auto* api = get_moonraker_api();
#if defined(HELIX_PLATFORM_ESP32)
    // These calibration panels are excluded from the embedded Core+AMS build.
    api = nullptr;
#endif
    const char* attrs[] = {
#if defined(HELIX_PLATFORM_ESP32)
        "tools_supported",
        "0",
#endif
        "has_heaters",
        api && !api->hardware().heaters().empty() ? "1" : "0",
        "has_mesh",
        api && api->hardware().has_bed_mesh() ? "1" : "0",
        "has_probe",
        api && api->hardware().has_probe() ? "1" : "0",
        "has_shaper",
        api && api->hardware().has_accelerometer() ? "1" : "0",
        nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, component_name(), attrs));
}
