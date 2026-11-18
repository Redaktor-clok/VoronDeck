// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_external_spool_overview.h"

#include "ui_callback_helpers.h"
#include "ui_external_spool_menu.h"
#include "ui_panel_filament.h"
#include "ui_settings_material_temps.h"
#include "ui_spool_canvas.h"

#include "ams_state.h"
#include "app_globals.h"
#include "home_spool_summary.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "theme_manager.h"

namespace helix::ui {

ExternalSpoolOverview::~ExternalSpoolOverview() {
    deinit_subjects();
}

void ExternalSpoolOverview::init_subjects() {
    if (!init_subjects_guarded([this] {
            UI_MANAGED_SUBJECT_STRING(name_, name_buf_, "", "external_overview_name", subjects_);
            UI_MANAGED_SUBJECT_STRING(material_, material_buf_, "", "external_overview_material", subjects_);
            UI_MANAGED_SUBJECT_STRING(amount_, amount_buf_, "", "external_overview_amount", subjects_);
            UI_MANAGED_SUBJECT_STRING(accounting_, accounting_buf_, "", "external_overview_accounting", subjects_);
        })) {
        return;
    }
    StaticSubjectRegistry::instance().register_deinit("ExternalSpoolOverview", [] {
        if (auto* overlay = lazy_global_if_exists<ExternalSpoolOverview>())
            overlay->deinit_subjects();
    });
}

void ExternalSpoolOverview::deinit_subjects() {
    spool_observer_.reset();
    deinit_subjects_base(subjects_);
}

void ExternalSpoolOverview::register_callbacks() {
    register_xml_callbacks({
        {"on_external_overview_back", [](lv_event_t*) { get_external_spool_overview().close(); }},
        {"on_external_overview_edit", [](lv_event_t* e) {
             open_external_spool_editor(lv_obj_get_screen(lv_event_get_current_target_obj(e)));
         }},
        {"on_external_overview_profiles", [](lv_event_t* e) {
             settings::get_material_temps_overlay().show(
                 lv_obj_get_screen(lv_event_get_current_target_obj(e)));
         }},
    });
}

void ExternalSpoolOverview::on_activate() {
    OverlayBase::on_activate();
    auto& panel = get_global_filament_panel();
    panel.on_activate();
    refresh_spool();
    spool_observer_.reset();
    auto& ams = AmsState::instance();
    spool_observer_ = observe<int>(ams.get_external_spool_color_subject(), this,
        [](ExternalSpoolOverview* self, int) { self->refresh_spool(); },
        ams.get_subjects_lifetime());
}

void ExternalSpoolOverview::on_deactivating(DeactivateReason) {
    spool_observer_.reset();
}

void ExternalSpoolOverview::refresh_spool() {
    if (!get_root())
        return;
    auto* canvas = lv_obj_find_by_name(get_root(), "external_overview_ring");
    const auto slot = AmsState::instance().get_external_spool_info();
    if (slot) {
        if (canvas) {
            spool_canvas_set_filament(canvas, *slot);
            const auto fill = home_spool_fill_level({slot->remaining_weight_g, slot->total_weight_g});
            ui_spool_canvas_set_fill_level(canvas, fill < 0 ? 0.0f : fill);
        }
        HomeSpoolAmount amount;
        amount.remaining_g = slot->remaining_weight_g;
        amount.total_g = slot->total_weight_g;
        lv_subject_copy_string(&name_, slot->spool_name.empty() ? lv_tr("Spool 1") : slot->spool_name.c_str());
        lv_subject_copy_string(&material_, slot->material.empty() ? lv_tr("Unknown") : slot->material.c_str());
        const auto summary = home_spool_summary(amount, {true, true, false});
        lv_subject_copy_string(&amount_, summary.empty() ? "—" : summary.c_str());
        lv_subject_copy_string(&accounting_, slot->spoolman_id > 0
            ? lv_tr("Linked to Spoolman")
            : lv_tr("Local filament accounting · spool weight excluded."));
    } else {
        if (canvas) {
            ui_spool_canvas_set_color(canvas, theme_manager_get_color("text_muted"));
            ui_spool_canvas_set_fill_level(canvas, 0.0f);
        }
        lv_subject_copy_string(&name_, lv_tr("Spool 1"));
        lv_subject_copy_string(&material_, lv_tr("No spool assigned"));
        lv_subject_copy_string(&amount_, "—");
        lv_subject_copy_string(&accounting_, lv_tr("Set the material and filament weight to start local accounting."));
    }
}

} // namespace helix::ui
