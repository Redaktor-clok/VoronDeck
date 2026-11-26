#include "json_utils.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_icon.h"
#include "ui_modal.h"
#include "ui_next_tick.h"
#include "ui_panel_calibration_pid.h"
#include "ui_text.h"
#include "ui_tile_rung.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "device_display_name.h"
#include "heater_bank_layout.h"
#include "heater_bank_widget.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/ui/shared_font_style.h"
#include "helix/ui/text_metrics.h"
#include "helix/xml/indexed_subject_pool.h"
#include "home_screen_preferences.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace helix {
namespace {
using helix::xml::IndexedSubjectPool;
IndexedSubjectPool names{"heater_bank", IndexedSubjectPool::Type::String, 128, "_name"};
IndexedSubjectPool values{"heater_bank", IndexedSubjectPool::Type::String, 48, "_value"};
IndexedSubjectPool setpoints{"heater_bank", IndexedSubjectPool::Type::String, 24, "_setpoint"};
IndexedSubjectPool statuses{"heater_bank", IndexedSubjectPool::Type::String, 48, "_status"};
IndexedSubjectPool powers{"heater_bank", IndexedSubjectPool::Type::String, 24, "_power"};
IndexedSubjectPool targets{"heater_bank", IndexedSubjectPool::Type::Int, 0, "_target"};
IndexedSubjectPool nozzles{"heater_bank", IndexedSubjectPool::Type::Int, 0, "_nozzle"};
IndexedSubjectPool cfg_names{"heater_bank_cfg", IndexedSubjectPool::Type::String, 128, "_name"};
IndexedSubjectPool cfg_on{"heater_bank_cfg", IndexedSubjectPool::Type::Int, 0, "_on"};
lv_subject_t row_count{}, cfg_count{}, cfg_mode{}, expanded{}, icon_rung{}, value_rung{},
    label_rung{};
WidgetSubjects subjects{"HeaterBankSubjects"};
void init_bank_subjects() {
    subjects.ensure_init(
        [] {
            for (auto* subject : {&icon_rung, &value_rung, &label_rung})
                lv_subject_init_int(subject, 0);
            subjects.publish("heater_bank_icon_rung", &icon_rung);
            subjects.publish("heater_bank_value_rung", &value_rung);
            // SUBJECT_OK: exported sizing diagnostics; checked through the subject registry.
            subjects.publish("heater_bank_label_rung", &label_rung);
            // SUBJECT_OK: exported sizing diagnostics; checked through the subject registry.
            lv_subject_init_int(&expanded, 0);
            subjects.publish("heater_bank_expanded", &expanded);
            lv_subject_init_int(&row_count, 0);
            subjects.publish("heater_bank_count", &row_count);
            lv_subject_init_int(&cfg_mode, 0);
            subjects.publish("heater_bank_cfg_mode", &cfg_mode);
            lv_subject_init_int(&cfg_count, 0);
            subjects.publish("heater_bank_cfg_count", &cfg_count);
        },
        [] {
            names.reclaim();
            values.reclaim();
            setpoints.reclaim();
            statuses.reclaim();
            powers.reclaim();
            targets.reclaim();
            nozzles.reclaim();
            cfg_names.reclaim();
            cfg_on.reclaim();
        });
}
// Heater-specific face steps preserve readable compact multi-device cells independently
// of the display-wide font_xs floor. Every face is an existing registered asset.
struct HeaterFace {
    const lv_font_t* value;
    const lv_font_t* caption;
    const lv_font_t* icon;
};
constexpr const char* heater_faces[][3] = {
    {"noto_sans_12", "noto_sans_light_10", "mdi_icons_16"},
    {"noto_sans_14", "noto_sans_light_10", "mdi_icons_16"},
    {"noto_sans_16", "noto_sans_light_11", "mdi_icons_24"},
    {"noto_sans_20", "noto_sans_light_11", "mdi_icons_32"},
    {"noto_sans_24", "noto_sans_light_12", "mdi_icons_32"},
    {"noto_sans_26", "noto_sans_light_16", "mdi_icons_48"},
    {"noto_sans_32", "noto_sans_light_18", "mdi_icons_48"},
};
HeaterFace heater_face(int rung) {
    const auto& names = heater_faces[std::clamp(rung, 0, 6)];
    return {lv_xml_get_font_silent(nullptr, names[0]), lv_xml_get_font_silent(nullptr, names[1]),
            lv_xml_get_font_silent(nullptr, names[2])};
}
HeaterBankWidget* active_bank = nullptr;
class HeaterSelectionModal : public Modal {
  public:
    HeaterSelectionModal(std::vector<std::string> names, nlohmann::json values, bool list,
                         std::function<bool(const nlohmann::json&, bool)> save)
        : names_(std::move(names)), values_(std::move(values)), save_(std::move(save)) {
        init_bank_subjects();
        lv_subject_set_int(&cfg_mode, list ? 1 : 0);
        cfg_names.ensure_size(names_.size());
        cfg_on.ensure_size(names_.size());
        for (size_t i = 0; i < names_.size(); ++i) {
            cfg_names.set_string(i, names_[i]);
            cfg_on.set_int(i, values_.value(names_[i], true));
        }
        lv_subject_set_int(&cfg_count, static_cast<int>(names_.size()));
    }
    const char* get_name() const override {
        return "Configure Heaters";
    }
    const char* component_name() const override {
        return "heater_bank_config_modal";
    }

  protected:
    void on_show() override {
        wire_ok_button("btn_primary");
        wire_cancel_button("btn_secondary");
    }
    void on_ok() override {
        for (size_t i = 0; i < names_.size(); ++i)
            values_[names_[i]] = lv_subject_get_int(cfg_on.at(i)) != 0;
        if (save_ && save_(values_, lv_subject_get_int(&cfg_mode) != 0))
            hide();
    }

  private:
    std::vector<std::string> names_;
    nlohmann::json values_;
    std::function<bool(const nlohmann::json&, bool)> save_;
};
// The keypad is singleton; pin the full Klipper ID so active-tool changes cannot retarget it.
std::string keypad_heater;
void heater_keypad_done(float value, void*) {
    auto* controller = get_temperature_controller();
    if (controller && !keypad_heater.empty())
        controller->set_target(keypad_heater, static_cast<double>(value), {.toast = true});
}
} // namespace

void HeaterBankWidget::init_subjects() {
    init_bank_subjects();
}

void register_heater_bank_widget() {
    register_widget_subjects("heater_bank", init_bank_subjects);
    lv_xml_register_event_cb(nullptr, "heater_bank_device_cb", HeaterBankWidget::device_clicked);
    lv_xml_register_event_cb(nullptr, "heater_bank_switch_cb", [](lv_event_t* e) {
        const char* index = static_cast<const char*>(lv_event_get_user_data(e));
        auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        if (index && sw) {
            const size_t i = std::strtoul(index, nullptr, 10);
            if (i < static_cast<size_t>(lv_subject_get_int(&cfg_count)))
                cfg_on.set_int(i, lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
        }
    });
    lv_xml_register_event_cb(nullptr, "heater_bank_mode_cb", [](lv_event_t* e) {
        const char* mode = static_cast<const char*>(lv_event_get_user_data(e));
        lv_subject_set_int(&cfg_mode, mode && std::string(mode) == "list" ? 1 : 0);
    });
    register_widget_factory("heater_bank", [](const std::string&) {
        return std::make_unique<HeaterBankWidget>(get_printer_state());
    });
}

std::vector<std::string> HeaterBankWidget::inventory() const {
    std::vector<std::string> result;
    for (const auto& name : state_.get_discovery().heaters())
        if (controllable_heater_id(name))
            result.push_back(name);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool HeaterBankWidget::selected(const std::string& name) const {
    bool fallback = true;
    if (config_.contains("heaters") && config_["heaters"].is_object()) {
        auto it = config_["heaters"].find(name);
        if (it != config_["heaters"].end() && it->is_boolean())
            fallback = it->get<bool>();
    }
    auto* cfg = Config::get_instance();
    return cfg ? HomeScreenPreferences(*cfg).is_visible(HomeScreenPreferences::DeviceKind::Heater,
                                                        name, fallback)
               : fallback;
}

bool HeaterBankWidget::portrait() const {
    auto* display = lv_display_get_default();
    return display && lv_display_get_vertical_resolution(display) >
                          lv_display_get_horizontal_resolution(display);
}

void HeaterBankWidget::Row::refresh() {
    if (!object)
        return;
    targets.set_int(index, target);
    animator.update(current, target);
    values.set_string(index, fmt::format("{}\u00B0", (current + 5) / 10));
    setpoints.set_string(index, target > 0 ? fmt::format("/ {}\u00B0", (target + 5) / 10) : "");
    powers.set_string(index, target <= 0 || power < 0 ? "" : fmt::format("{}%", power));
    statuses.set_string(index,
                        target > 0 ? fmt::format("/ {}\u00B0", (target + 5) / 10) : lv_tr("Off"));
}

void HeaterBankWidget::attach(lv_obj_t* widget, lv_obj_t* screen) {
    widget_ = widget;
    screen_ = screen;
    cells_ = lv_obj_find_by_name(widget, "heater_bank_cells");
    active_bank = this;
    auto& temps = state_.temperature_state();
    extruder_observer_ = ui::observe<int>(
        temps.get_extruder_version_subject(), this,
        [](HeaterBankWidget* self, int) { self->schedule_rebuild(); },
        temps.get_subjects_lifetime());
    auto& sensors = sensors::TemperatureSensorManager::instance();
    if (sensors.subjects_initialized())
        inventory_observer_ = ui::observe<int>(
            sensors.revision_subject(), this,
            [](HeaterBankWidget* self, int) { self->schedule_rebuild(); },
            sensors.subjects_lifetime());
    rebuild_rows();
    schedule_rebuild();
}

void HeaterBankWidget::on_activate() {
    schedule_rebuild();
}

void HeaterBankWidget::schedule_rebuild() {
    if (!widget_ || rebuild_pending_)
        return;
    rebuild_pending_ = true;
    ui::run_next_tick(lifetime_.token(), [this] {
        rebuild_pending_ = false;
        const auto devices = inventory();
        std::vector<std::string> selection;
        for (const auto& name : devices)
            if (selected(name))
                selection.push_back(name);
        // Home settings can change while this retained tile is off-screen.
        // Discovery is unchanged in that case, but its visible rows are stale.
        if (devices != applied_inventory_ || selection != applied_selection_)
            rebuild_rows();
    });
}

void HeaterBankWidget::rebuild_rows() {
    rows_.clear();
    if (!cells_)
        return;
    lv_subject_set_int(&row_count, 0);
    auto devices = inventory();
    applied_inventory_ = devices;
    applied_selection_.clear();
    names.ensure_size(devices.size());
    values.ensure_size(devices.size());
    statuses.ensure_size(devices.size());
    setpoints.ensure_size(devices.size());
    powers.ensure_size(devices.size());
    targets.ensure_size(devices.size());
    nozzles.ensure_size(devices.size());
    auto& temps = state_.temperature_state();
    auto& sensors = sensors::TemperatureSensorManager::instance();
    for (const auto& name : devices) {
        if (!selected(name))
            continue;
        applied_selection_.push_back(name);
        SubjectLifetime current_life, target_life;
        lv_subject_t* current = nullptr;
        lv_subject_t* target = nullptr;
        if (name == "heater_bed") {
            current = temps.get_bed_temp_subject(current_life);
            target = temps.get_bed_target_subject(target_life);
        } else if (name.rfind("extruder", 0) == 0) {
            current = temps.get_extruder_temp_subject(name, current_life);
            target = temps.get_extruder_target_subject(name, target_life);
        } else {
            current = sensors.get_temp_subject(name, current_life);
            target = sensors.get_target_subject(name, target_life);
        }
        if (!current || !target)
            continue;
        auto row = std::make_unique<Row>();
        row->owner = this;
        row->name = name;
        row->index = rows_.size();
        std::string display = name;
        if (name == "heater_bed")
            display = lv_tr("Bed");
        else if (auto it = temps.extruders().find(name); it != temps.extruders().end())
            display = it->second.display_name;
        else if (name.rfind("heater_generic ", 0) == 0)
            display = extract_device_suffix(name);
        names.set_string(row->index, display);
        nozzles.set_int(row->index,
                        name.rfind("extruder", 0) == 0 ? 1 : (name == "heater_bed" ? 2 : 0));
        row->current = lv_subject_get_int(current);
        row->target = lv_subject_get_int(target);
        SubjectLifetime power_life;
        lv_subject_t* power = nullptr;
        if (name == "heater_bed") {
            power = temps.get_bed_power_subject();
            power_life = temps.get_subjects_lifetime();
        } else if (name.rfind("extruder", 0) == 0)
            power = temps.get_extruder_power_subject(name, power_life);
        else
            power = sensors.get_power_subject(name, power_life);
        if (power) {
            row->power = lv_subject_get_int(power);
            row->power_observer = ui::observe<int>(
                power, row.get(),
                [](Row* r, int value) {
                    r->power = value;
                    r->refresh();
                },
                power_life);
        }
        row->current_observer = ui::observe<int>(
            current, row.get(),
            [](Row* r, int value) {
                r->current = value;
                r->refresh();
            },
            current_life);
        row->target_observer = ui::observe<int>(
            target, row.get(),
            [](Row* r, int value) {
                r->target = value;
                r->refresh();
            },
            target_life);
        rows_.push_back(std::move(row));
    }
    lv_subject_set_int(&row_count, static_cast<int>(rows_.size()));
    for (auto& row : rows_) {
        row->object = lv_obj_find_by_name(
            cells_, ("heater_bank_device_" + std::to_string(row->index)).c_str());
        if (!row->object)
            continue;
        row->icon = lv_obj_find_by_name(
            row->object,
            row->name.rfind("extruder", 0) == 0
                ? "nozzle_icon_glyph"
                : (row->name == "heater_bed" ? "bed_icon_glyph" : "chamber_icon_glyph"));
        if (row->icon)
            row->animator.attach(row->icon);
        row->refresh();
    }
    relayout_for_granted_size();
}

HeaterBankWidget::LayoutFit HeaterBankWidget::choose_size(size_t count, int width, int height,
                                                          bool list) const {
    const auto shape = heater_bank_shape(count, portrait());
    const int gap = cells_ ? lv_obj_get_style_pad_column(cells_, LV_PART_MAIN) : 4;
    const int overhead_w = widget_ ? lv_obj_get_style_pad_left(widget_, LV_PART_MAIN) +
                                         lv_obj_get_style_pad_right(widget_, LV_PART_MAIN)
                                   : 8;
    const int overhead_h = widget_ ? lv_obj_get_style_pad_top(widget_, LV_PART_MAIN) +
                                         lv_obj_get_style_pad_bottom(widget_, LV_PART_MAIN)
                                   : 8;
    if (list) {
        const int available_width = width - overhead_w;
        const int visible_rows = std::clamp(static_cast<int>(count), 1, 3);
        const int available_height = std::max(0, height - overhead_h);
        const int row_height =
            std::max(32, (available_height - gap * (visible_rows - 1)) / visible_rows);
        for (int rung = portrait() ? 3 : 6; rung >= 0; --rung) {
            const auto face = heater_face(rung);
            if (!face.value || !face.caption || !face.icon)
                continue;
            const auto icon = ui::tile_glyph_box({face.icon, LV_SCALE_NONE});
            // Reserve every native field in both idle and heating states.
            const int status_width = std::max(ui::text_width("/ 888\u00B0", face.caption),
                                              ui::text_width(lv_tr("Off"), face.caption));
            const int required_width = icon.w + ui::text_width("888\u00B0", face.value) +
                                       status_width + ui::text_width("100%", face.caption) +
                                       ui::text_width("Hea", face.value) + 20;
            if (required_width <= available_width &&
                std::max<int>(icon.h, face.value->line_height) + 2 <= row_height &&
                available_height >= 32)
                return {rung, rung};
        }
        return {};
    }
    const int cell_width = (width - overhead_w - gap * (shape.columns - 1)) / shape.columns;
    const int cell_height = (height - overhead_h - gap * (shape.rows - 1)) / shape.rows;
    auto* row = rows_.empty() ? nullptr : rows_.front()->object;
    const int row_pad_w = row ? lv_obj_get_style_pad_left(row, LV_PART_MAIN) +
                                    lv_obj_get_style_pad_right(row, LV_PART_MAIN)
                              : 4;
    const int row_pad_h = row ? lv_obj_get_style_pad_top(row, LV_PART_MAIN) +
                                    lv_obj_get_style_pad_bottom(row, LV_PART_MAIN)
                              : 4;
    const int icon_gap = row ? lv_obj_get_style_pad_column(row, LV_PART_MAIN) : 4;
    const int largest = count > 3 ? std::max(0, choose_size(3, width, height).value - 1) : 6;
    for (int rung = largest; rung >= 0; --rung) {
        const auto faces = heater_face(rung);
        if (!faces.value || !faces.caption || !faces.icon)
            continue;
        for (bool compact : {false, true}) {
            if (compact && count <= 3)
                continue;
            const auto* caption =
                compact ? lv_xml_get_font_silent(nullptr, "noto_sans_light_8") : faces.caption;
            if (!caption)
                continue;
            // Both columns reserve their metadata line even while idle. Live values
            // never participate in fitting, so heating cannot resize any device.
            const int text_width = std::max(ui::text_width("888\u00B0", faces.value),
                                            ui::text_width("/ 888\u00B0", caption));
            const int power_width = ui::text_width("100%", caption);
            const int text_height = faces.value->line_height + 2 * caption->line_height;
            auto fits = [&](const lv_font_t* icon_font) {
                const auto icon = ui::tile_glyph_box({icon_font, LV_SCALE_NONE});
                return std::max(icon.w, power_width) + icon_gap + text_width + row_pad_w <=
                           cell_width &&
                       std::max<int>(icon.h + caption->line_height, text_height) + row_pad_h <=
                           cell_height;
            };
            if (!fits(faces.icon))
                continue;
            int icon_rung = rung;
            // Preserve the reference icon size when a shorter text face fits the three lines.
            if (count <= 3) {
                const auto normal_icon = heater_face(3);
                if (normal_icon.icon && fits(normal_icon.icon))
                    icon_rung = std::max(icon_rung, 3);
            }
            if (count <= 3 && portrait()) {
                const auto larger = heater_face(5);
                if (larger.icon && fits(larger.icon))
                    icon_rung = std::max(icon_rung, 5);
            }
            return {rung, icon_rung, compact};
        }
    }
    return {};
}

bool HeaterBankWidget::fits_at(int width, int height) const {
    return choose_size(list_mode() ? 1 : rows_.size(), width, height, list_mode()).value >= 0;
}

void HeaterBankWidget::on_size_changed(int, int, int, int) {
    if (!cells_)
        return;
    const bool list = list_mode();
    lv_subject_set_int(&expanded, portrait() && rows_.size() <= 3 && !list ? 1 : 0);
    const auto shape = list ? HeaterBankShape{1, std::max(1, static_cast<int>(rows_.size()))}
                            : heater_bank_shape(rows_.size(), portrait());
    lv_obj_update_layout(cells_);
    const auto sizing = choose_size(rows_.size(), granted_width_px(), granted_height_px(), list);
    const int chosen = std::max(0, sizing.value);
    const int chosen_icon = list ? (portrait() ? std::min(chosen, 2) : chosen) : sizing.icon;
    lv_subject_set_int(&icon_rung, chosen_icon);
    lv_subject_set_int(&value_rung, chosen);
    lv_subject_set_int(&label_rung, chosen);
    const int visible_rows = std::clamp(static_cast<int>(rows_.size()), 1, 3);
    // Native placement notifies us before the old XML geometry necessarily settles.
    // Fixed scroll tracks must use the authoritative grant, not the previous canvas height.
    const int list_content_height = granted_height_px() -
                                    lv_obj_get_style_pad_top(widget_, LV_PART_MAIN) -
                                    lv_obj_get_style_pad_bottom(widget_, LV_PART_MAIN);
    const int list_height = std::max<int32_t>(
        32, (list_content_height -
             lv_obj_get_style_pad_row(cells_, LV_PART_MAIN) * (visible_rows - 1)) /
                visible_rows);
    // DECLARATIVE_OK: measured tracks keep readable list cells independent of inventory count.
    lv_obj_set_scroll_dir(cells_, list ? LV_DIR_VER : LV_DIR_NONE);
    columns_.assign(shape.columns, LV_GRID_FR(1));
    columns_.push_back(LV_GRID_TEMPLATE_LAST);
    tracks_.assign(shape.rows, list ? list_height : LV_GRID_FR(1));
    tracks_.push_back(LV_GRID_TEMPLATE_LAST);
    lv_obj_set_grid_dsc_array(cells_, columns_.data(), tracks_.data());
    for (size_t i = 0; i < rows_.size(); ++i) {
        const int col = list ? 0 : (portrait() ? static_cast<int>(i) / 3 : static_cast<int>(i) % 3);
        const int row = list ? static_cast<int>(i)
                             : (portrait() ? static_cast<int>(i) % 3 : static_cast<int>(i) / 3);
        if (!rows_[i]->object)
            continue;
        auto faces = heater_face(chosen);
        if (!list && sizing.compact_caption)
            faces.caption = lv_xml_get_font_silent(nullptr, "noto_sans_light_8");
        auto apply_face = [&](const char* name, const lv_font_t* font, bool one_line) {
            if (auto* obj = lv_obj_find_by_name(rows_[i]->object, name); obj && font) {
                // DECLARATIVE_OK: exact registered face selected by measured cell geometry.
                ui::apply_font_style(obj, font);
                if (one_line)
                    ui::label_hold_one_line(obj, ui::LineHold::AnyMode);
            }
        };
        apply_face("heater_bank_value", faces.value, true);
        for (const char* name : {"heater_bank_setpoint", "heater_bank_status", "heater_bank_power"})
            apply_face(name, faces.caption, true);
        const auto* name_font =
            list ? (portrait() ? heater_face(std::max(0, chosen - 1)).value : faces.value)
                 : faces.caption;
        apply_face("heater_bank_name", name_font, true);
        if (!list) {
            if (auto* column = lv_obj_find_by_name(rows_[i]->object, "heater_bank_icon_column")) {
                // DECLARATIVE_OK: reserve measured 100% width while the cold label is empty.
                const auto icon =
                    ui::tile_glyph_box({heater_face(chosen_icon).icon, LV_SCALE_NONE});
                lv_obj_set_style_min_width(
                    column, std::max<int32_t>(icon.w, ui::text_width("100%", faces.caption)),
                    LV_PART_MAIN);
            }
        }
        for (const char* name : {"home_nozzle_cold_glyph", "nozzle_icon_glyph", "bed_icon_glyph",
                                 "chamber_icon_glyph"})
            apply_face(name, heater_face(chosen_icon).icon, false);
        // DECLARATIVE_OK: runtime orientation and discovered device count determine measured grid
        // cells.
        lv_obj_set_grid_cell(rows_[i]->object, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH,
                             row, 1);
    }
}

void HeaterBankWidget::device_clicked(lv_event_t* e) {
    auto* indev = lv_indev_active();
    if (indev && lv_indev_get_scroll_obj(indev))
        return;
    const char* index = static_cast<const char*>(lv_event_get_user_data(e));
    if (!active_bank || !index)
        return;
    const size_t i = std::strtoul(index, nullptr, 10);
    if (i < active_bank->rows_.size())
        active_bank->open_target(*active_bank->rows_[i]);
}

void HeaterBankWidget::open_target(Row& row) {
    auto* controller = get_temperature_controller();
    if (!controller)
        return;
    keypad_heater = row.name;
    ui_keypad_config_t cfg = {
        .initial_value = row.target / 10.0f,
        .min_value = 0.0f,
        .max_value = controller->keypad_max_for(
            row.name, row.name.rfind("extruder", 0) == 0 ? 300.0f : 120.0f),
        .title_label = keypad_heater.c_str(),
        .unit_label = "\u00B0C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = heater_keypad_done,
        .user_data = nullptr,
    };
    ui_keypad_show(&cfg);
#if !defined(HELIX_PLATFORM_ESP32)
    ui_keypad_offer_pid(row.name, screen_);
#endif
}

bool HeaterBankWidget::on_edit_configure() {
    if (PanelWidgetManager::refuse_layout_edit())
        return false;
    auto names = inventory();
    auto values = config_.is_object() ? helix::json_util::safe_object(config_, "heaters")
                                      : nlohmann::json::object();
    if (!values.is_object())
        values = nlohmann::json::object();
    for (const auto& name : names)
        values[name] = selected(name);
    auto token = lifetime_.token();
    Modal::show_owned(std::make_unique<HeaterSelectionModal>(
                          names, values, list_mode(),
                          [this, token](const nlohmann::json& selection, bool list) {
                              return !token.expired() && apply_selection(selection, list);
                          }),
                      screen_);
    return false; // Modal OK owns the transaction; opening/cancelling never rebuilds.
}

bool HeaterBankWidget::apply_selection(const nlohmann::json& selection, bool list) {
    size_t count = 0;
    for (const auto& name : inventory()) {
        auto it = selection.find(name);
        if (it == selection.end() || !it->is_boolean() || it->get<bool>())
            ++count;
    }
    const int width =
        granted_width_px() > 0 ? granted_width_px() : (widget_ ? lv_obj_get_width(widget_) : 0);
    const int height =
        granted_height_px() > 0 ? granted_height_px() : (widget_ ? lv_obj_get_height(widget_) : 0);
    // Removing devices cannot demand more room than the already accepted tile.
    if ((!list && (count > rows_.size() || list_mode()) &&
         choose_size(count, width, height).value < 0) ||
        (list && choose_size(1, width, height, true).value < 0)) {
        NOTIFY_WARNING("{}",
                       lv_tr("Not enough room for the selected heaters. Enlarge this tile first."));
        return false;
    }
    auto& layout = PanelWidgetManager::instance().get_widget_config(panel_id());
    layout.load();
    for (size_t page = 0; page < layout.page_count(); ++page) {
        auto& entries = layout.page_entries_mut(page);
        auto found = std::find_if(entries.begin(), entries.end(),
                                  [this](const auto& e) { return e.id == id() && e.is_placed(); });
        if (found == entries.end())
            continue;
        auto next = config_.is_object() ? config_ : nlohmann::json::object();
        next["heaters"] = selection;
        if (list)
            next["display_mode"] = "list";
        else
            next.erase("display_mode");
        // The tile editor and Home settings edit the same effective selection.
        // Keep legacy tile values for compatibility, but do not let them mask
        // a later explicit Home preference (or vice versa).
        HomeScreenPreferences preferences(*Config::get_instance());
        for (const auto& name : inventory()) {
            const auto it = selection.find(name);
            preferences.set_visible(HomeScreenPreferences::DeviceKind::Heater, name,
                                    it == selection.end() || !it->is_boolean() || it->get<bool>());
        }
        // The user's granted area is authoritative, including when the selected count changes.
        found->config = next;
        config_ = next;
        layout.save();
        const auto panel = panel_id();
        lifetime_.defer("HeaterBank::saved",
                        [panel] { PanelWidgetManager::instance().notify_config_changed(panel); });
        return true;
    }
    return false;
}

void HeaterBankWidget::detach() {
    lifetime_.invalidate();
    rebuild_pending_ = false;
    inventory_observer_.reset();
    extruder_observer_.reset();
    rows_.clear();
    if (active_bank == this)
        active_bank = nullptr;
    widget_ = nullptr;
    cells_ = nullptr;
    screen_ = nullptr;
}
} // namespace helix
