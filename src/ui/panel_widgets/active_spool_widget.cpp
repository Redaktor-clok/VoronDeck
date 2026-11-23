// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "active_spool_widget.h"

#include "ui_ams_edit_follow_up.h"
#include "ui_ams_edit_overlay.h"
#include "ui_color_picker.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_external_spool_overview.h"
#include "ui_fonts.h"
#include "ui_spool_canvas.h"
#include "ui_tile_rung.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "active_material_provider.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_display_name.h"
#include "home_spool_summary.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

namespace helix {

void register_active_spool_widget() {
    register_widget_factory("active_spool", [](const std::string&) {
        auto* api = PanelWidgetManager::instance().shared_resource<IMoonrakerAPI>();
        return std::make_unique<ActiveSpoolWidget>(api);
    });
}

// The compact form: a spool one line of the rung's icon face square, over one
// caption line whose text is either "Active Spool" or "No Spool". The caption is
// budgeted whatever show_widget_labels says, because "No Spool" draws regardless.
ActiveSpoolWidget::ActiveSpoolWidget(IMoonrakerAPI* api)
    : TiledPanelWidget("active_spool", TileSizing::Content{"", "", "Active Spool", false, "",
                                                           /*label_always_drawn=*/true,
                                                           TileSizing::IconBox::Square}),
      api_(api) {
    // Registered before the manager parses the component, which drops a
    // binding whose subject is missing at parse time.
    UI_MANAGED_SUBJECT_INT(wide_subject_, 0, "active_spool_wide", subjects_);
    UI_MANAGED_SUBJECT_INT(loaded_subject_, 0, "active_spool_loaded", subjects_);
    UI_MANAGED_SUBJECT_STRING(summary_subject_, summary_buf_, "", "active_spool_summary",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(has_summary_subject_, 0, "active_spool_has_summary", subjects_);
    UI_MANAGED_SUBJECT_STRING(percent_subject_, percent_buf_, "", "active_spool_percent",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(has_percent_subject_, 0, "active_spool_has_percent", subjects_);
    UI_MANAGED_SUBJECT_STRING(badge_subject_, badge_buf_, "1", "active_spool_badge", subjects_);
    display_picker_.set_action_callback([this](int action, int) {
        if (action < 0)
            return;
        config_["simplified"] = action == 1;
        save_widget_config(config_);
        PanelWidgetManager::instance().notify_config_changed(panel_id());
    });
}

void ActiveSpoolWidget::set_config(const nlohmann::json& config) {
    config_ = config.is_object() ? config : nlohmann::json::object();
    simplified_ = config.contains("simplified") && config["simplified"].is_boolean() &&
                  config["simplified"].get<bool>();
    if (simplified_)
        is_wide_ = true;
}

std::string ActiveSpoolWidget::get_component_name() const {
    return simplified_ ? "panel_widget_active_spool_simple" : "panel_widget_active_spool";
}

bool ActiveSpoolWidget::on_edit_configure() {
    if (widget_obj_ && parent_screen_ && !display_picker_.is_visible())
        display_picker_.show_below_widget(parent_screen_, widget_obj_);
    return false;
}

ActiveSpoolWidget::~ActiveSpoolWidget() {
    detach();
}

void ActiveSpoolWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (!widget_obj_)
        return;

    // Register click handler via per-callback user_data
    auto* btn = lv_obj_find_by_name(widget_obj_, "spoolman_btn");
    if (btn) {
        lv_obj_add_event_cb(btn, clicked_cb, LV_EVENT_CLICKED, this);
    }

    // Cache element pointers
    spool_compact_ = lv_obj_find_by_name(widget_obj_, "spool_compact");
    spool_wide_ = lv_obj_find_by_name(widget_obj_, "spool_wide");
    material_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_material");
    brand_color_label_ = lv_obj_find_by_name(widget_obj_, "spoolman_brand_color");

    // Observe spool changes from all sources
    auto token = lifetime_.token();

    // External spool changes
    spool_color_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_external_spool_color_subject(), this,
        [token](ActiveSpoolWidget* self, int /*color*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // AMS backend active slot changes
    current_slot_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_current_slot_subject(), this,
        [token](ActiveSpoolWidget* self, int /*slot*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    // AMS slot info changes (material/color edits)
    slots_version_observer_ = helix::ui::observe<int>(
        AmsState::instance().get_slots_version_subject(), this,
        [token](ActiveSpoolWidget* self, int /*version*/) {
            if (token.expired())
                return;
            self->update_spool_display();
        },
        AmsState::instance().get_subjects_lifetime());

    lv_subject_set_int(&wide_subject_, is_wide_ ? 1 : 0);
    resize_spool_canvases();

    // Initial display update
    update_spool_display();

    spdlog::debug("[ActiveSpoolWidget] Attached");
}

void ActiveSpoolWidget::detach() {
    display_picker_.hide();
    lifetime_.invalidate();

    spool_color_observer_.reset();
    current_slot_observer_.reset();
    slots_version_observer_.reset();

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    spool_compact_ = nullptr;
    spool_wide_ = nullptr;
    material_label_ = nullptr;
    brand_color_label_ = nullptr;

    spdlog::debug("[ActiveSpoolWidget] Detached");
}

void ActiveSpoolWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                        int height_px) {
    granted_width_ = width_px;
    granted_height_ = height_px;
    sizing_.measure_and_publish(width_px, height_px);
    const bool wide = simplified_ || is_wide_at(width_px, height_px);
    const bool mode_changed = wide != is_wide_;
    is_wide_ = wide;
    lv_subject_set_int(&wide_subject_, wide ? 1 : 0);

    if (!widget_obj_)
        return;

    resize_spool_canvases();
    if (mode_changed || is_wide_) {
        // Refresh display for the now-visible elements
        update_spool_display();
    }
    spdlog::debug("[ActiveSpoolWidget] on_size_changed {}x{} -> {}", width_px, height_px,
                  wide ? "wide" : "compact");
}

int ActiveSpoolWidget::wide_spool_edge() {
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_xl");
    return icon_font ? lv_font_get_line_height(icon_font) : 48;
}

bool ActiveSpoolWidget::is_wide_at(int width_px, int height_px) {
    return width_px >= widget_size::w_normal() && height_px >= wide_spool_edge();
}

void ActiveSpoolWidget::resize_spool_canvases() {
    // The wide row keeps the tier's largest icon edge; the compact spool is
    // one line of the icon face the tile's rung names, the square TileSizing
    // measured.
    if (spool_wide_) {
        const int target = simplified_ ? 58 : wide_spool_edge();
        const int width = granted_width_ > 0 ? granted_width_ : lv_obj_get_width(widget_obj_);
        const int height = lv_obj_get_content_height(lv_obj_get_parent(spool_wide_));
        const int edge = std::min({target, std::max(24, width / 3), height > 0 ? height : target});
        ui_spool_canvas_set_size(spool_wide_, edge);
    }
    if (spool_compact_) {
        const ui::TileFace face = ui::tile_rung_face(ui::TileLadder::Icon, sizing_.icon_rung());
        if (face.font)
            ui_spool_canvas_set_size(spool_compact_, face.px(lv_font_get_line_height(face.font)));
    }
}

void ActiveSpoolWidget::update_spool_display() {
    // Use ActiveMaterialProvider which checks AMS backend first, then external spool
    auto active = helix::get_active_material();
    bool has_spool = active.has_value();

    // Also try to get weight and naming info from the source SlotInfo
    float remaining_weight = -1;
    float total_weight = -1;
    std::optional<SlotInfo> source_slot;

    if (has_spool) {
        // Get the source slot (AMS or external)
        auto& ams = AmsState::instance();
        AmsBackend* backend = ams.get_backend();
        if (backend && backend->is_filament_loaded()) {
            int current = backend->get_current_slot();
            if (current >= 0) {
                source_slot = backend->get_slot_info(current);
            }
        } else {
            source_slot = ams.get_external_spool_info();
        }
        if (source_slot) {
            remaining_weight = source_slot->remaining_weight_g;
            total_weight = source_slot->total_weight_g;
        }
    }

    // Compute fill level and color
    lv_color_t spool_color = lv_color_hex(0x808080); // Gray for no-spool
    float fill_level = 0.0f;

    if (has_spool) {
        spool_color = lv_color_hex(active->color_rgb);
        HomeSpoolAmount amount;
        amount.remaining_g = remaining_weight;
        amount.total_g = total_weight;
        const double fill = home_spool_fill_level(amount);
        // Unknown weights leave the ring's neutral track, with no invented 100%.
        fill_level = fill >= 0 ? static_cast<float>(fill) : 0.0f;
    }

    // Determine which spool canvas is active based on current mode
    lv_obj_t* active_spool = is_wide_ ? spool_wide_ : spool_compact_;

    // Update the active spool canvas
    if (active_spool) {
        if (has_spool && source_slot)
            helix::ui::spool_canvas_set_filament(active_spool, active->color_rgb,
                                                 source_slot->multi_color_hexes,
                                                 helix::ui::swatch_finish_of(*source_slot));
        else
            ui_spool_canvas_set_color(active_spool, spool_color);
        ui_spool_canvas_set_fill_level(active_spool, fill_level);
        // Semi-transparent when no spool assigned
        lv_obj_set_style_opa(active_spool, has_spool ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    // Update text labels (wide mode)
    if (material_label_) {
        if (has_spool) {
            lv_label_set_text(material_label_, active->material_name.c_str());
            lv_obj_set_style_text_align(material_label_, LV_TEXT_ALIGN_LEFT, 0);
        } else {
            // A tag, not lv_tr() text: the label then re-translates itself.
            if (is_wide_) {
                lv_label_set_translation_tag(material_label_, "No Spool");
            } else {
                lv_label_set_text(material_label_, "");
            }
            lv_obj_set_style_text_align(material_label_, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    if (brand_color_label_) {
        std::string brand_color_str;
        if (has_spool && source_slot) {
            // Same precedence as the AMS "currently loaded" card. Spoolman keeps
            // vendor and filament name out of SlotInfo on purpose (see
            // filament_display_name.h), so a Spoolman-linked lane on a backend
            // that reports no brand of its own -- AFC -- can only be named
            // through the identity cache.
            const auto identity = SpoolmanManager::find_identity(source_slot->spoolman_id);
            const auto parts =
                resolve_filament_label_parts(*source_slot, identity ? &*identity : nullptr,
                                             get_color_name_from_hex(source_slot->color_rgb));
            // The material has its own row above this one, so it is joined out.
            brand_color_str = compose_filament_label(parts.brand, parts.name, "");
        }
        lv_label_set_text(brand_color_label_, brand_color_str.c_str());
        if (brand_color_str.empty()) {
            lv_obj_add_flag(brand_color_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(brand_color_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    std::string summary;
    std::string percent;
    if (has_spool && source_slot) {
        HomeSpoolAmount amount;
        amount.remaining_g = remaining_weight;
        amount.total_g = total_weight;
        if (!source_slot->remaining_length_display().empty())
            amount.reported_metres = source_slot->remaining_length_m;
        if (auto material = filament::find_material(source_slot->material))
            amount.density_g_cm3 = material->density_g_cm3;
        amount.diameter_mm = get_printer_state().get_discovery().filament_diameter_mm();
        auto* config = Config::get_instance();
        const auto units = config ? HomeScreenPreferences(*config).spool_units()
                                  : HomeScreenPreferences::SpoolUnits{};
        auto* number_label =
            widget_obj_ ? lv_obj_find_by_name(widget_obj_, "spoolman_weight") : nullptr;
        const int text_width = number_label ? lv_obj_get_width(number_label) : 0;
        const lv_font_t* number_font = theme_manager_get_font("font_xs");
        const auto measured_width = [](const std::string& text, const lv_font_t* font) {
            lv_point_t size;
            lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            return static_cast<int>(size.x);
        };
        const std::string full_summary = home_spool_summary(amount, units);
        const bool condensed =
            simplified_ ||
            (is_wide_ && text_width > 0 && measured_width(full_summary, number_font) > text_width);
        if (condensed) {
            percent = home_spool_summary(amount, {units.percent, false, false}, false);
            summary = home_spool_summary(amount, {false, units.grams, units.metres}, false);
            // Keep selected numbers still and complete: compact units first,
            // then two short amount lines when the existing text column is narrow.
            if (text_width > 0 && measured_width(summary, number_font) > text_width) {
                const std::string separator = " \xC2\xB7 ";
                if (auto pos = summary.find(separator); pos != std::string::npos)
                    summary.replace(pos, separator.size(), "\n");
                summary.erase(std::remove(summary.begin(), summary.end(), ' '), summary.end());
            }
            const double fill = home_spool_fill_level(amount);
            if (active_spool)
                ui_spool_canvas_set_fill_level(active_spool,
                                               fill >= 0 ? static_cast<float>(fill) : 0.0f);
        } else {
            summary = full_summary;
        }
        if (number_label && is_wide_ && text_width > 0) {
            const lv_font_t* candidates[] = {number_font, &noto_sans_12, &noto_sans_11,
                                             &noto_sans_10};
            for (const auto* font : candidates) {
                number_font = font;
                if (measured_width(summary, font) <= text_width)
                    break;
            }
            lv_obj_set_style_text_font(number_label, number_font, 0);
            if (auto* percent_label = lv_obj_find_by_name(widget_obj_, "spool_simple_percent"))
                lv_obj_set_style_text_font(percent_label, number_font, 0);
        }
    }
    lv_subject_copy_string(&percent_subject_, percent.c_str());
    lv_subject_set_int(&has_percent_subject_, percent.empty() ? 0 : 1);
    auto& ams = AmsState::instance();
    auto* backend = ams.get_backend();
    const int slot = backend && backend->is_filament_loaded() ? backend->get_current_slot() : -1;
    const std::string number = std::to_string(slot >= 0 ? backend->spool_display_number(slot) : 1);
    lv_subject_copy_string(&badge_subject_, number.c_str());
    lv_subject_copy_string(&summary_subject_, summary.c_str());
    lv_subject_set_int(&has_summary_subject_, summary.empty() ? 0 : 1);

    lv_subject_set_int(&loaded_subject_, has_spool ? 1 : 0);
}

void ActiveSpoolWidget::handle_clicked() {
    spdlog::info("[ActiveSpoolWidget] Clicked");

    auto& ams = AmsState::instance();

    // Check if AMS backend has active material
    AmsBackend* backend = ams.get_backend();
    if (backend && backend->is_filament_loaded()) {
        int current = backend->get_current_slot();
        if (current >= 0) {
            spdlog::info("[ActiveSpoolWidget] Opening AMS slot edit for slot {}", current);
            SlotInfo slot = backend->get_slot_info(current);
            helix::ui::get_ams_edit_overlay().show_for_slot(
                parent_screen_, current, slot, api_,
                [current](const helix::ui::AmsEditOverlay::EditResult& result) {
                    if (!result.saved) {
                        return;
                    }
                    AmsBackend* be = AmsState::instance().get_backend();
                    if (!be) {
                        return;
                    }
                    // Capture the pre-edit slot BEFORE the commit — its unlink
                    // arm (clear the server active spool) needs the old link.
                    SlotInfo original = be->get_slot_info(current);
                    AmsError err =
                        AmsState::instance().commit_slot_edit(current, original, result.slot_info);
                    if (err.success() || err.partially_applied) {
                        helix::ui::offer_edit_follow_up(current, original, result.slot_info);
                    }
                    if (!err.success()) {
                        helix::ui::notify_ams_error(err);
                    }
                });
            return;
        }
    }

    // External spool or no spool -- open external spool edit modal
    open_external_spool_edit();
}

void ActiveSpoolWidget::open_external_spool_edit() {
    spdlog::info("[ActiveSpoolWidget] Opening external spool editor");

    if (lv_subject_get_int(AmsState::instance().get_ams_type_subject()) == 0 &&
        !ToolState::instance().is_multi_tool()) {
        helix::ui::get_external_spool_overview().show(parent_screen_);
        return;
    }

    auto ext = AmsState::instance().get_external_spool_info();
    SlotInfo initial_info = ext.value_or(SlotInfo{});
    initial_info.slot_index = -2;
    initial_info.global_index = -2;

    helix::ui::get_ams_edit_overlay().show_for_slot(
        parent_screen_, -2, initial_info, api_,
        [](const helix::ui::AmsEditOverlay::EditResult& result) {
            if (result.saved) {
                // Owns S1 (server active-spool sync) + the S5 emptiness
                // predicate + settings persist/erase.
                AmsState::instance().commit_external_spool_edit(result.slot_info);
            }
        });
}

void ActiveSpoolWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ActiveSpoolWidget] clicked_cb");
    auto* self = static_cast<ActiveSpoolWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
