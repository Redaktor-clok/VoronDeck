// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_macro_harness.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "material_settings_manager.h"
#include "filament_catalog.h"
#include "../test_helpers/material_settings_manager_test_access.h"
#include <filesystem>
#include <unistd.h>
#include "helix-xml/src/xml/lv_xml.h"
#include "preset_materials.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "temperature_controller.h"
#include "theme_manager.h"
#include "ui_settings_material_temps.h"
#include "ui_update_queue.h"
#include "ui_spool_canvas.h"
#include "ui_external_spool_overview.h"
#include "ui/ui_widget_helpers.h"

#include <memory>
#include <string>
#include "../catch_amalgamated.hpp"

namespace {
struct MaterialSelectorHarness {
    LVGLUITestFixture& fixture;
    helix::test::FilamentPanelMacroHarness panel;
    helix_test::ScopedSharedResource<helix::TemperatureController> controller;
    lv_obj_t* root = nullptr;
    int original_portrait = 0;
    bool original_strict = helix::ui::strict_ui_checks();

    explicit MaterialSelectorHarness(LVGLUITestFixture& f)
        : fixture(f),
          controller(std::make_shared<helix::TemperatureController>(panel.state, &panel.api)) {
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::set_strict_ui_checks(true);
        helix::presets::init_subjects();
        auto* orientation = lv_xml_get_subject(nullptr, "ui_is_portrait");
        REQUIRE(orientation != nullptr);
        original_portrait = lv_subject_get_int(orientation);
        root = static_cast<lv_obj_t*>(lv_xml_create(f.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel.panel->setup(root, f.test_screen());
        f.process_lvgl(20);
        helix::ui::UpdateQueue::instance().drain();
        panel.client.clear_gcode_script_history();
    }
    ~MaterialSelectorHarness() {
        helix::settings::get_material_temps_overlay().close();
        helix::ui::UpdateQueue::instance().drain();
        if (root) {
            lv_obj_delete(root);
        }
        panel.panel.reset();
        StaticPanelRegistry::instance().destroy_all();
        lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), original_portrait);
        fixture.process_lvgl(10);
        helix::ui::set_strict_ui_checks(original_strict);
    }
    lv_obj_t* find(const char* name) {
        auto* object = lv_obj_find_by_name(root, name);
        REQUIRE(object != nullptr);
        return object;
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "External overview displays the canonical material and independent local spool data",
                 "[external-spool-overview][filament-material-selector][filament]") {
    MaterialSelectorHarness h(*this);
    helix::ui::ExternalSpoolOverview overview;
    overview.init_subjects();
    overview.register_callbacks();
    auto* root = overview.create(test_screen());
    REQUIRE(root);
    auto* material = lv_obj_find_by_name(root, "external_overview_material_selected");
    REQUIRE(material);
    CHECK(lv_obj_find_by_name(root, "filament_material_selector") == nullptr);
    const auto selected_profile = lv_dropdown_get_selected(h.find("filament_material_selector"));

    helix::SlotInfo slot;
    slot.material = "PETG";
    slot.color_rgb = 0xBB4050;
    slot.total_weight_g = 1000;
    slot.spoolman_id = 0;
    auto* canvas = lv_obj_find_by_name(root, "external_overview_ring");
    REQUIRE(canvas);
    for (float grams : {850.0f, 0.0f, -1.0f}) {
        slot.remaining_weight_g = grams;
        helix::AmsState::instance().set_external_spool_info_in_memory(slot);
        overview.refresh_spool();
        process_lvgl(10);
        CHECK(ui_spool_canvas_get_fill_level(canvas) == Catch::Approx(grams < 0 ? 0 : grams / 1000));
        CHECK(lv_color_eq(ui_spool_canvas_get_color(canvas), lv_color_hex(slot.color_rgb)));
        auto* amount = lv_obj_find_by_name(root, "external_overview_remaining");
        REQUIRE(amount);
        const std::string shown = lv_label_get_text(amount);
        if (grams == 850) {
            CHECK(shown.find("850") != std::string::npos);
            CHECK(shown.find("1000") != std::string::npos);
            CHECK(shown.find("85%") != std::string::npos);
        } else if (grams < 0) {
            CHECK(shown.find('%') == std::string::npos);
        }
    }
    for (const char* type : {"PETG", "ABS"}) {
        slot.material = type;
        helix::AmsState::instance().set_external_spool_info_in_memory(slot);
        overview.refresh_spool();
        CHECK(std::string(lv_label_get_text(material)) == type);
    }
    CHECK(lv_dropdown_get_selected(h.find("filament_material_selector")) == selected_profile);
    CHECK(h.panel.client.gcode_script_history().empty());
    helix::AmsState::instance().clear_external_spool_info();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament external spool is a ring with real live remaining and no invented full amount",
                 "[filament-material-selector][filament][remaining-rings]") {
    MaterialSelectorHarness h(*this);
    auto* container = h.find("external_spool_container");
    REQUIRE(lv_obj_get_child_count(container) == 1);
    auto* canvas = lv_obj_get_child(container, 0);
    REQUIRE(canvas);
    CHECK(helix::ui::spool_canvas_get_view(canvas) == helix::ui::SpoolView::Ring);
    helix::SlotInfo slot;
    slot.material = "PLA";
    slot.color_rgb = 0x27B5BD;
    slot.total_weight_g = 1000;
    for (float grams : {0.0f, 500.0f, 1000.0f, -1.0f}) {
        slot.remaining_weight_g = grams;
        helix::AmsState::instance().set_external_spool_info_in_memory(slot);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        CHECK(ui_spool_canvas_get_fill_level(canvas) == Catch::Approx(grams < 0 ? 0 : grams / 1000));
        CHECK(lv_color_eq(ui_spool_canvas_get_color(canvas), lv_color_hex(slot.color_rgb)));
    }
    CHECK(h.panel.client.gcode_script_history().empty());
    helix::AmsState::instance().clear_external_spool_info();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament material dropdown selects native presets without heating in both orientations",
                 "[filament-material-selector][filament]") {
    MaterialSelectorHarness h(*this);
    const int nozzle_before = lv_subject_get_int(
        h.panel.state.temperature_state().get_active_extruder_target_subject());
    const int bed_before = lv_subject_get_int(
        h.panel.state.temperature_state().get_bed_target_subject());
    for (const bool portrait : {false, true, false}) {
        CAPTURE(portrait);
        lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), portrait ? 1 : 0);
        lv_obj_set_size(h.root, portrait ? 480 : 800, portrait ? 736 : 416);
        process_lvgl(20);
        lv_obj_update_layout(h.root);
        auto* dropdown = h.find("filament_material_selector");
        CHECK(lv_dropdown_get_option_count(dropdown) >= helix::presets::kPresetCount + 1);
        for (const auto& material : *filament::materials())
            CHECK(std::string(lv_dropdown_get_options(dropdown)).find(material.name) != std::string::npos);
        const std::string options = lv_dropdown_get_options(dropdown);
        for (int slot = 0; slot < helix::presets::kPresetCount; ++slot) {
            CHECK(options.find(helix::presets::display_label(slot)) != std::string::npos);
        }
        // The five quick heat shortcuts and their reserved grid are removed.
        for (const char* name : {"preset_pla", "preset_petg", "preset_abs", "preset_tpu",
                                 "preset_spool", "preset_grid", "spool_preset_cell"})
            CHECK(lv_obj_find_by_name(h.root, name) == nullptr);
        for (const char* name : {"filament_heat_selected", "btn_cooldown", "btn_load", "btn_unload"})
            h.find(name);
        auto* actions = h.find("filament_heat_actions");
        CHECK(lv_obj_get_height(actions) == theme_manager_get_spacing("button_height"));
        lv_area_t heat_box{}, cool_box{};
        lv_obj_get_coords(h.find("filament_heat_selected"), &heat_box);
        lv_obj_get_coords(h.find("btn_cooldown"), &cool_box);
        CHECK(heat_box.y1 == cool_box.y1);
        CHECK(heat_box.y2 == cool_box.y2);
        CHECK(heat_box.x2 < cool_box.x1);
        lv_obj_t* profiles = h.find("filament_material_profiles");
        lv_area_t left{}, right{}, row{};
        lv_obj_get_coords(dropdown, &left);
        lv_obj_get_coords(profiles, &right);
        lv_obj_get_coords(h.find("filament_material_selector_row"), &row);
        CHECK(left.x2 < right.x1);
        CHECK(left.x1 >= row.x1);
        CHECK(right.x2 <= row.x2);
        CHECK(left.y2 <= row.y2);
        CHECK(right.y2 <= row.y2);
        lv_dropdown_set_selected(dropdown, 3); // ABS-position, whatever material the user assigned.
        lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(h.panel.panel->get_material() == 2);
        CHECK(lv_dropdown_get_selected(dropdown) == 3);
        const std::string hardware_target_before = lv_subject_get_string(
            lv_xml_get_subject(nullptr, "filament_nozzle_target"));
        // A current-only Klipper report must not turn the recommendation into
        // an unsent live target or a false Heating status.
        h.panel.state.update_from_status({{"extruder", {{"temperature", 26.0}}}});
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
        int shown_current = 0, shown_target = -1;
        h.panel.panel->get_temp(&shown_current, &shown_target);
        CHECK(shown_target == nozzle_before);
        CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "filament_nozzle_target"))) ==
              hardware_target_before);
        CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "filament_status"))) ==
              lv_tr("Tap Heat to start preheating"));
        CHECK(h.panel.panel->get_material() == 2);
        CHECK(h.panel.client.gcode_script_history().empty());
        CHECK(lv_subject_get_int(h.panel.state.temperature_state().get_active_extruder_target_subject()) ==
              nozzle_before);
        CHECK(lv_subject_get_int(h.panel.state.temperature_state().get_bed_target_subject()) == bed_before);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament Materials and Profiles opens the native material editor without commands",
                 "[filament-material-selector][filament]") {
    MaterialSelectorHarness h(*this);
    lv_obj_send_event(h.find("filament_material_profiles"), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    auto& editor = helix::settings::get_material_temps_overlay();
    REQUIRE(editor.get_root() != nullptr);
    CHECK(editor.is_visible());
    CHECK(lv_obj_find_by_name(editor.get_root(), "material_list_view") != nullptr);
    CHECK(h.panel.client.gcode_script_history().empty());
    editor.close();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Short landscape filament column scrolls every native action into view",
                 "[filament-material-selector][filament][layout]") {
    MaterialSelectorHarness h(*this);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), 0);
    lv_obj_set_size(h.root, 800, 320);
    process_lvgl(20);
    lv_obj_update_layout(h.root);
    auto* column = h.find("right_column");
    CHECK(lv_obj_has_flag(column, LV_OBJ_FLAG_SCROLLABLE));
    CHECK(lv_obj_get_scroll_dir(column) == LV_DIR_VER);
    for (const char* name : {"btn_load", "btn_unload", "btn_purge", "btn_extrude", "btn_retract",
                             "filament_material_selector", "filament_material_profiles",
                             "filament_heat_selected", "btn_cooldown"}) {
        CAPTURE(name);
        auto* action = h.find(name);
        lv_obj_scroll_to_view_recursive(action, LV_ANIM_OFF);
        lv_obj_update_layout(h.root);
        lv_area_t action_box{}, column_box{};
        lv_obj_get_coords(action, &action_box);
        lv_obj_get_coords(column, &column_box);
        CHECK(lv_obj_get_height(action) >= theme_manager_get_spacing("button_height"));
        CHECK(action_box.y1 >= column_box.y1);
        CHECK(action_box.y2 <= column_box.y2);
        CHECK(action_box.x1 >= column_box.x1);
        CHECK(action_box.x2 <= column_box.x2);
    }
    CHECK(h.panel.client.gcode_script_history().empty());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Returning from material profiles refreshes saved and reset recommendations without heating",
                 "[filament-material-selector][filament]") {
    MaterialSelectorHarness h(*this);
    int slot = -1;
    for (int i = 0; i < helix::presets::kPresetCount; ++i) {
        const auto branded = MaterialSettingsManager::instance().get_preset_filament(i);
        if (!branded || !branded->is_branded()) {
            slot = i;
            break;
        }
    }
    REQUIRE(slot >= 0);
    const auto name = helix::presets::name(slot);
    struct RestoreOverride {
        std::string name;
        std::optional<filament::MaterialOverride> original;
        ~RestoreOverride() {
            auto& manager = MaterialSettingsManager::instance();
            if (original) {
                manager.set_override(name, *original);
            } else {
                manager.clear_override(name);
            }
            helix::presets::refresh_subjects();
        }
    } restore{name, std::nullopt};
    if (const auto* original = MaterialSettingsManager::instance().get_override(name)) {
        restore.original = *original;
    }
    auto* dropdown = h.find("filament_material_selector");
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(slot + 1));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    const auto before = helix::presets::slot_temps(slot);
    REQUIRE(before.has_value());
    filament::MaterialOverride changed;
    changed.nozzle_min = before->nozzle + 7;
    changed.nozzle_max = before->nozzle + 7;
    changed.bed_temp = before->bed + 7;
    MaterialSettingsManager::instance().set_override(name, changed);
    // This is the same write the native editor performs; its close reactivates
    // FilamentPanel through NavigationManager's ordinary lifecycle.
    h.panel.panel->on_activate();
    auto assert_refreshed = [&]() {
        const auto updated = helix::presets::slot_temps(slot);
        REQUIRE(updated.has_value());
        const std::string temps_subject = "preset_material_" + std::to_string(slot) + "_temps";
        CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, temps_subject.c_str()))) ==
              helix::presets::temp_label(slot));
        const std::string preview = lv_subject_get_string(
            lv_xml_get_subject(nullptr, "filament_material_nozzle_temp"));
        CHECK(preview.find(std::to_string(updated->nozzle)) != std::string::npos);
        CHECK(lv_dropdown_get_selected(dropdown) == static_cast<uint32_t>(slot + 1));
        CHECK(h.panel.client.gcode_script_history().empty());
        CHECK(lv_subject_get_int(h.panel.state.temperature_state().get_active_extruder_target_subject()) == 0);
    };
    assert_refreshed();
    CHECK(helix::presets::slot_temps(slot)->nozzle == before->nozzle + 7);
    MaterialSettingsManager::instance().clear_override(name);
    h.panel.panel->on_activate();
    assert_refreshed();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Explicit Heat uses only selected native material and refuses blocked or missing selection",
                 "[filament-material-selector][filament][selected-heat]") {
    const bool portrait = GENERATE(false, true);
    const int slot = GENERATE(1, 2);
    MaterialSelectorHarness h(*this);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "ui_is_portrait"), portrait ? 1 : 0);
    lv_obj_set_size(h.root, portrait ? 480 : 800, portrait ? 736 : 416);
    auto* navigation = lv_xml_get_subject(nullptr, "nav_buttons_enabled");
    REQUIRE(navigation);
    lv_subject_set_int(navigation, 1);
    process_lvgl(20);
    auto* heat = h.find("filament_heat_selected");
    auto* dropdown = h.find("filament_material_selector");
    CHECK(lv_obj_has_state(heat, LV_STATE_DISABLED));
    lv_obj_send_event(heat, LV_EVENT_CLICKED, nullptr);
    CHECK(h.panel.client.gcode_script_history().empty());
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(slot + 1));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    process_lvgl(10);
    CHECK(h.panel.panel->get_material() == slot);
    CHECK(h.panel.client.gcode_script_history().empty());
    CHECK_FALSE(lv_obj_has_state(heat, LV_STATE_DISABLED));
    auto* blocked = helix::AmsState::instance().get_nozzle_heat_blocked_subject();
    lv_subject_set_int(blocked, 1);
    process_lvgl(10);
    CHECK(lv_obj_has_state(heat, LV_STATE_DISABLED));
    lv_obj_send_event(heat, LV_EVENT_CLICKED, nullptr);
    CHECK(h.panel.client.gcode_script_history().empty());
    lv_subject_set_int(blocked, 0);
    lv_subject_set_int(navigation, 0);
    process_lvgl(10);
    lv_obj_send_event(heat, LV_EVENT_CLICKED, nullptr);
    CHECK(h.panel.client.gcode_script_history().empty());
    lv_subject_set_int(navigation, 1);
    process_lvgl(10);
    const auto temperatures = helix::presets::slot_temps(slot);
    REQUIRE(temperatures);
    lv_obj_send_event(heat, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    const auto scripts = h.panel.client.gcode_script_history();
    REQUIRE_FALSE(scripts.empty());
    bool selected_nozzle_sent = false;
    for (const auto& script : scripts) {
        if (script.find("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=") != std::string::npos) {
            CHECK(script.find("TARGET=" + std::to_string(temperatures->nozzle)) != std::string::npos);
            selected_nozzle_sent = true;
        }
        CHECK(script.find("LOAD_FILAMENT") == std::string::npos);
        CHECK(script.find("UNLOAD_FILAMENT") == std::string::npos);
    }
    CHECK(selected_nozzle_sent);
}

namespace {
struct SelectedProfileStore {
    std::string previous = helix::printer::detail::user_overlay_dir_ref();
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("helix_selected_profile_" + std::to_string(::getpid()));
    SelectedProfileStore() {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        helix::printer::detail::user_overlay_dir_ref() = path.string();
        helix::TestAccess::reset(helix::MaterialSettingsManager::instance());
        filament::reload_materials();
        helix::MaterialSettingsManager::instance().init();
    }
    ~SelectedProfileStore() {
        helix::printer::detail::user_overlay_dir_ref() = previous;
        helix::TestAccess::reset(helix::MaterialSettingsManager::instance());
        filament::reload_materials();
        helix::MaterialSettingsManager::instance().init();
        std::filesystem::remove_all(path);
    }
};
int option_index(lv_obj_t* dropdown, const std::string& wanted) {
    const std::string options = lv_dropdown_get_options(dropdown);
    size_t start = 0;
    int index = 0;
    while (start <= options.size()) {
        const auto end = options.find('\n', start);
        if (options.substr(start, end == std::string::npos ? end : end - start) == wanted)
            return index;
        if (end == std::string::npos) break;
        start = end + 1;
        ++index;
    }
    return -1;
}
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Custom profiles survive dropdown refresh and heat only through native explicit action",
                 "[filament-material-selector][selected-heat][custom-profile]") {
    const int route = GENERATE(0, 1, 2, 3); // direct, declared, unknown, declared with limit
    SelectedProfileStore store;
    auto& settings = helix::MaterialSettingsManager::instance();
    filament::MaterialOverride definition;
    definition.nozzle_min = 235;
    definition.nozzle_max = 245;
    definition.bed_temp = 85;
    definition.chamber_temp = route == 3 ? 100 : 50;
    if (route != 0) {
        definition.preheat_macro = "HEAT_PROFILE";
        definition.macro_handles_heating = true;
    }
    REQUIRE(settings.save_user_profile("My Profile", definition));
    REQUIRE(filament::find_material("My Profile")->user_defined);
    MaterialSelectorHarness h(*this);
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json{"extruder", "heater_bed", "gcode_macro HEAT_PROFILE"});
    h.panel.state.set_hardware(discovery);
    if (route == 1 || route == 3)
        h.panel.cache_macros({{"HEAT_PROFILE", "M118 {params.NOZZLE_TEMP} {params.BED_TEMP} {params.CHAMBER_TEMP}"}});
    else
        helix::MacroParamCache::instance().clear();
    lv_subject_set_int(lv_xml_get_subject(nullptr, "nav_buttons_enabled"), 1);
    auto* dropdown = h.find("filament_material_selector");
    int index = option_index(dropdown, "My Profile");
    REQUIRE(index > helix::presets::kPresetCount);
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    process_lvgl(10);
    auto* summary = lv_xml_get_subject(nullptr, "filament_material_profile_summary");
    REQUIRE(summary);
    const std::string preview = lv_subject_get_string(summary);
    CHECK(preview.find("My Profile") != std::string::npos);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "filament_status"))) ==
          "Tap Heat to start preheating");
    CHECK(preview.find("240") != std::string::npos);
    CHECK(preview.find("85") != std::string::npos);
    CHECK(preview.find(std::to_string(*definition.chamber_temp)) != std::string::npos);
    CHECK(h.panel.client.gcode_script_history().empty());
    // A new database entry and an edit refresh the list, not the stable selected key.
    REQUIRE(settings.save_user_profile("AAA Other", definition));
    definition.nozzle_min = 245;
    definition.nozzle_max = 245;
    REQUIRE(settings.save_user_profile("My Profile", definition, "My Profile"));
    h.panel.panel->on_activate();
    process_lvgl(20);
    dropdown = h.find("filament_material_selector");
    CHECK(lv_dropdown_get_selected(dropdown) == option_index(dropdown, "My Profile"));
    CHECK(std::string(lv_subject_get_string(summary)).find("245") != std::string::npos);
    CHECK(h.panel.client.gcode_script_history().empty());
    CHECK(lv_subject_get_int(h.panel.state.temperature_state().get_active_extruder_target_subject()) == 0);
    h.panel.client.clear_gcode_script_history();
    lv_obj_send_event(h.find("filament_heat_selected"), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    const auto scripts = h.panel.client.gcode_script_history();
    REQUIRE_FALSE(scripts.empty());
    if (route == 0) {
        bool nozzle = false, bed = false;
        for (const auto& script : scripts) {
            nozzle |= script.find("HEATER=extruder TARGET=245") != std::string::npos;
            bed |= script.find("HEATER=heater_bed TARGET=85") != std::string::npos;
            CHECK(script.find("chamber") == std::string::npos);
        }
        CHECK(nozzle); CHECK(bed);
    } else if (route == 1 || route == 3) {
        REQUIRE(scripts.size() == 1);
        CHECK(scripts[0].find("HEAT_PROFILE") == 0);
        CHECK(scripts[0].find("NOZZLE_TEMP=245") != std::string::npos);
        CHECK(scripts[0].find("BED_TEMP=85") != std::string::npos);
        CHECK(scripts[0].find(route == 3 ? "CHAMBER_TEMP=80" : "CHAMBER_TEMP=50") != std::string::npos);
    } else {
        CHECK(scripts == std::vector<std::string>{"HEAT_PROFILE"});
    }
    CHECK(filament::find_material("My Profile")->chamber_temp_c == *definition.chamber_temp);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Selected profile chamber recommendation uses native M141 or heater only on Heat",
                 "[filament-material-selector][selected-heat][custom-profile][chamber][m141]") {
    const int route = GENERATE(0, 1, 2); // M141, direct heater, absent chamber
    CAPTURE(route);
    SelectedProfileStore store;
    filament::MaterialOverride definition;
    definition.nozzle_min = 240;
    definition.nozzle_max = 240;
    definition.bed_temp = 85;
    definition.chamber_temp = 50;
    REQUIRE(helix::MaterialSettingsManager::instance().save_user_profile("Chamber Profile", definition));
    auto& settings = helix::SettingsManager::instance();
    struct RestoreAssignment {
        helix::SettingsManager& settings;
        std::string previous;
        ~RestoreAssignment() { settings.set_chamber_heater_assignment(previous); }
    } restore{settings, settings.get_chamber_heater_assignment()};
    settings.set_chamber_heater_assignment("auto");
    MaterialSelectorHarness h(*this);
    nlohmann::json objects = {"extruder", "heater_bed"};
    if (route != 2) objects.push_back("heater_generic chamber_heater");
    if (route == 0) objects.push_back("gcode_macro M141");
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(objects);
    h.panel.state.set_hardware(discovery);
    if (route == 0) {
        h.panel.cache_macros({{"M141",
            "SET_HEATER_TEMPERATURE HEATER=chamber_heater TARGET={params.S|default(0)}"}});
        REQUIRE(helix::MacroParamCache::instance().has_macro("m141"));
    }
    REQUIRE(h.controller.get().resolved_name(helix::HeaterType::Chamber) ==
            (route == 2 ? "" : "heater_generic chamber_heater"));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "nav_buttons_enabled"), 1);
    auto* dropdown = h.find("filament_material_selector");
    const int index = option_index(dropdown, "Chamber Profile");
    REQUIRE(index > helix::presets::kPresetCount);
    h.panel.client.clear_gcode_script_history();
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    process_lvgl(10);
    CHECK(h.panel.client.gcode_script_history().empty());
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr,
        "filament_material_profile_summary"))).find("50") != std::string::npos);
    lv_obj_send_event(h.find("filament_heat_selected"), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    auto expected = std::vector<std::string>{
        "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=240",
        "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=85"};
    if (route == 0) expected.push_back("M141 S50");
    if (route == 1)
        expected.push_back("SET_HEATER_TEMPERATURE HEATER=chamber_heater TARGET=50");
    CHECK(h.panel.client.gcode_script_history() == expected);
    const auto stored = filament::find_material("Chamber Profile");
    REQUIRE(stored);
    CHECK(stored->chamber_temp_c == 50);
}
