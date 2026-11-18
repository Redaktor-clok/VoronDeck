// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/material_settings_manager_test_access.h"
#include "config.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "material_settings_manager.h"

#include <filesystem>
#include <fstream>
#include <cstdint>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace filament;

// Fixture that resets MaterialSettingsManager singleton and its overlay between tests
// Reset on entry as well as exit: any earlier case, in any file, can leave the
// singleton initialized, and then this case's init() never reads its own data.
struct MaterialSettingsFixture : LVGLTestFixture {
    MaterialSettingsFixture() {
        clean();
    }
    ~MaterialSettingsFixture() override {
        clean();
    }

    static void clean() {
        TestAccess::reset(MaterialSettingsManager::instance());
        std::error_code ec;
        std::filesystem::remove(
            helix::printer::detail::user_overlay_dir_ref() + "/user_filaments.json", ec);
        filament::reload_materials();
    }
};

// ============================================================================
// MaterialSettingsManager Tests
// ============================================================================

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager init with no config",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // No overrides should exist with fresh config
    CHECK_FALSE(MaterialSettingsManager::instance().has_override("PLA"));
    CHECK(MaterialSettingsManager::instance().get_override("PLA") == nullptr);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager set/get round trip",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.nozzle_min = 195;
    ovr.nozzle_max = 215;
    ovr.bed_temp = 55;

    MaterialSettingsManager::instance().set_override("PLA", ovr);

    REQUIRE(MaterialSettingsManager::instance().has_override("PLA"));
    const auto* result = MaterialSettingsManager::instance().get_override("PLA");
    REQUIRE(result != nullptr);
    REQUIRE(result->nozzle_min.has_value());
    CHECK(*result->nozzle_min == 195);
    REQUIRE(result->nozzle_max.has_value());
    CHECK(*result->nozzle_max == 215);
    REQUIRE(result->bed_temp.has_value());
    CHECK(*result->bed_temp == 55);

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager sparse override",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Only override bed temp
    MaterialOverride ovr;
    ovr.bed_temp = 110;

    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    CHECK_FALSE(result->nozzle_min.has_value());
    CHECK_FALSE(result->nozzle_max.has_value());
    REQUIRE(result->bed_temp.has_value());
    CHECK(*result->bed_temp == 110);

    // Clean up
    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager clear_override",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.nozzle_min = 200;
    MaterialSettingsManager::instance().set_override("PETG", ovr);
    REQUIRE(MaterialSettingsManager::instance().has_override("PETG"));

    MaterialSettingsManager::instance().clear_override("PETG");
    CHECK_FALSE(MaterialSettingsManager::instance().has_override("PETG"));
    CHECK(MaterialSettingsManager::instance().get_override("PETG") == nullptr);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager clear nonexistent is safe",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Should not crash
    MaterialSettingsManager::instance().clear_override("NonExistent");
    CHECK_FALSE(MaterialSettingsManager::instance().has_override("NonExistent"));
}

// ============================================================================
// find_material override integration tests
// ============================================================================

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material returns overridden nozzle temps",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Set override for PLA
    MaterialOverride ovr;
    ovr.nozzle_min = 195;
    ovr.nozzle_max = 215;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    auto result = find_material("PLA");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 195);
    CHECK(result->nozzle_max == 215);
    CHECK(result->bed_temp == 60); // Not overridden, should be database default

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material returns overridden bed temp only",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    auto result = find_material("PLA");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 190); // Database default
    CHECK(result->nozzle_max == 220); // Database default
    CHECK(result->bed_temp == 55);    // Overridden

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material returns defaults after clear_override",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.nozzle_min = 200;
    ovr.bed_temp = 70;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    MaterialSettingsManager::instance().clear_override("PLA");

    auto result = find_material("PLA");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 190);
    CHECK(result->nozzle_max == 220);
    CHECK(result->bed_temp == 60);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material with no override returns database values",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Ensure no overrides for PETG
    MaterialSettingsManager::instance().clear_override("PETG");

    auto result = find_material("PETG");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 230);
    CHECK(result->nozzle_max == 260);
    CHECK(result->bed_temp == 80);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material override preserves non-temp fields",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    auto result = find_material("PLA");
    REQUIRE(result.has_value());

    // Non-temperature fields should be unchanged
    CHECK(std::string_view(result->name) == "PLA");
    CHECK(std::string_view(result->category) == "Standard");
    CHECK(result->dry_temp_c == 45);
    CHECK(result->density_g_cm3 == Catch::Approx(1.24f));
    CHECK(std::string_view(result->compat_group) == "PLA");

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "Multiple material overrides coexist",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride pla_ovr;
    pla_ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", pla_ovr);

    MaterialOverride abs_ovr;
    abs_ovr.nozzle_min = 245;
    abs_ovr.bed_temp = 110;
    MaterialSettingsManager::instance().set_override("ABS", abs_ovr);

    auto pla = find_material("PLA");
    auto abs = find_material("ABS");
    REQUIRE(pla.has_value());
    REQUIRE(abs.has_value());

    CHECK(pla->bed_temp == 55);
    CHECK(abs->nozzle_min == 245);
    CHECK(abs->bed_temp == 110);

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager preheat_macro round trip",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "PREHEAT_ABS";
    ovr.macro_handles_heating = true;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    REQUIRE(result->preheat_macro.has_value());
    CHECK(*result->preheat_macro == "PREHEAT_ABS");
    REQUIRE(result->macro_handles_heating.has_value());
    CHECK(*result->macro_handles_heating == true);

    CHECK_FALSE(result->nozzle_min.has_value());
    CHECK_FALSE(result->nozzle_max.has_value());
    CHECK_FALSE(result->bed_temp.has_value());

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager macro with temp overrides",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.bed_temp = 110;
    ovr.preheat_macro = "HEAT_SOAK_ABS";
    ovr.macro_handles_heating = false;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    CHECK(*result->bed_temp == 110);
    CHECK(*result->preheat_macro == "HEAT_SOAK_ABS");
    CHECK(*result->macro_handles_heating == false);

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "MaterialSettingsManager absent macro_handles_heating defaults true",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "MY_MACRO";
    MaterialSettingsManager::instance().set_override("TPU", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("TPU");
    REQUIRE(result != nullptr);
    REQUIRE(result->preheat_macro.has_value());
    CHECK_FALSE(result->macro_handles_heating.has_value());

    MaterialSettingsManager::instance().clear_override("TPU");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "Preheat macro override: macro_handles_heating true skips temps",
                 "[material_settings][preheat]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "PREHEAT_ABS";
    ovr.macro_handles_heating = true;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    REQUIRE(result->preheat_macro.has_value());

    bool handles_heating = result->macro_handles_heating.value_or(true);
    CHECK(handles_heating == true);
    CHECK(*result->preheat_macro == "PREHEAT_ABS");

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "Preheat macro override: macro_handles_heating false runs both",
                 "[material_settings][preheat]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "ENABLE_BED_FANS";
    ovr.macro_handles_heating = false;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);

    bool handles_heating = result->macro_handles_heating.value_or(true);
    CHECK(handles_heating == false);

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "get_all_overrides returns all set overrides",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Clear any stale overrides
    MaterialSettingsManager::instance().clear_override("PLA");
    MaterialSettingsManager::instance().clear_override("ABS");

    MaterialOverride ovr;
    ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    ovr.bed_temp = 110;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto& all = MaterialSettingsManager::instance().get_all_overrides();
    CHECK(all.count("PLA") == 1);
    CHECK(all.count("ABS") == 1);

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
    MaterialSettingsManager::instance().clear_override("ABS");
}

// ============================================================================
// Preset materials tests
// ============================================================================

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager preset defaults when unset",
                 "[material_settings][presets]") {
    Config::get_instance()->get_json("/preset_materials") =
        nlohmann::json(); // null → no valid presets
    MaterialSettingsManager::instance().init();
    auto p = MaterialSettingsManager::instance().get_preset_materials();
    CHECK(p[0] == "PLA");
    CHECK(p[1] == "PETG");
    CHECK(p[2] == "ABS");
    CHECK(p[3] == "TPU");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager preset set/get and persistence",
                 "[material_settings][presets]") {
    Config::get_instance()->get_json("/preset_materials") = nlohmann::json(); // start clean
    MaterialSettingsManager::instance().init();

    MaterialSettingsManager::instance().set_preset_material(1, "PC");
    CHECK(MaterialSettingsManager::instance().get_preset_materials()[1] == "PC");

    // out-of-range and empty are no-ops
    MaterialSettingsManager::instance().set_preset_material(99, "PA");
    MaterialSettingsManager::instance().set_preset_material(0, "");
    CHECK(MaterialSettingsManager::instance().get_preset_materials()[0] == "PLA");

    // Simulate a fresh process: reset the singleton, reload from persisted config
    TestAccess::reset(MaterialSettingsManager::instance());
    MaterialSettingsManager::instance().init();
    CHECK(MaterialSettingsManager::instance().get_preset_materials()[1] == "PC");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager preset reset restores defaults",
                 "[material_settings][presets]") {
    Config::get_instance()->get_json("/preset_materials") = nlohmann::json();
    MaterialSettingsManager::instance().init();
    MaterialSettingsManager::instance().set_preset_material(0, "PA");
    MaterialSettingsManager::instance().reset_preset_materials();
    auto p = MaterialSettingsManager::instance().get_preset_materials();
    CHECK(p[0] == "PLA");
    CHECK(p[3] == "TPU");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "MaterialSettingsManager preset malformed config falls back to defaults",
                 "[material_settings][presets]") {
    // Non-string / wrong-size array must not throw and must yield defaults.
    Config::get_instance()->get_json("/preset_materials") = nlohmann::json::array({1, 2});
    TestAccess::reset(MaterialSettingsManager::instance());
    MaterialSettingsManager::instance().init();
    auto p = MaterialSettingsManager::instance().get_preset_materials();
    CHECK(p[0] == "PLA");
    CHECK(p[1] == "PETG");
    CHECK(p[2] == "ABS");
    CHECK(p[3] == "TPU");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "MaterialSettingsManager preset non-string element skipped, default retained",
                 "[material_settings][presets]") {
    // Correctly-sized 4-element array with a non-string element must not throw; the
    // non-string slot keeps its default while valid string slots are applied.
    Config::get_instance()->get_json("/preset_materials") =
        nlohmann::json::array({"PC", 5, "ABS", "TPU"});
    TestAccess::reset(MaterialSettingsManager::instance());
    MaterialSettingsManager::instance().init();
    auto p = MaterialSettingsManager::instance().get_preset_materials();
    CHECK(p[0] == "PC");
    CHECK(p[1] == "PETG"); // non-string element skipped → default retained
    CHECK(p[2] == "ABS");
    CHECK(p[3] == "TPU");
}

// ============================================================================
// Overlay persistence and the settings.json migration
// ============================================================================

namespace {

std::string overlay_path() {
    return helix::printer::detail::user_overlay_dir_ref() + "/user_filaments.json";
}

void write_overlay(const std::string& body) {
    std::ofstream(overlay_path()) << body;
    filament::reload_materials();
}

nlohmann::json read_overlay() {
    std::ifstream f(overlay_path());
    return f.is_open() ? nlohmann::json::parse(f) : nlohmann::json();
}

/// The overlay's `types` entry for @p name, or null.
nlohmann::json overlay_type(const std::string& name) {
    auto doc = read_overlay();
    if (doc.is_object() && doc.contains("types")) {
        for (const auto& t : doc["types"])
            if (t.value("name", "") == name)
                return t;
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(MaterialSettingsFixture, "settings material_overrides move into the overlay",
                 "[material_settings][migration]") {
    Config::get_instance()->get_json("/material_overrides") = nlohmann::json::parse(
        R"({"PLA": {"nozzle_min": 205, "bed_temp": 65, "chamber_temp": 0,
                    "preheat_macro": "WARM_PLA", "macro_handles_heating": false}})");

    MaterialSettingsManager::instance().init();

    auto pla = overlay_type("PLA");
    REQUIRE(pla.is_object());
    CHECK(pla["nozzle_min"] == 205);
    CHECK(pla["bed"] == 65);
    CHECK(pla["chamber"] == 0);
    CHECK(pla["preheat_macro"] == "WARM_PLA");
    CHECK(pla["macro_handles_heating"] == false);
    CHECK_FALSE(pla.contains("bed_temp"));
    CHECK_FALSE(Config::get_instance()->exists("/material_overrides"));

    CHECK(find_material("PLA")->bed_temp == 65);
    const auto* ovr = MaterialSettingsManager::instance().get_override("PLA");
    REQUIRE(ovr != nullptr);
    CHECK(ovr->bed_temp == 65);
    CHECK(ovr->preheat_macro == "WARM_PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "the migration is idempotent",
                 "[material_settings][migration]") {
    Config::get_instance()->get_json("/material_overrides") =
        nlohmann::json::parse(R"({"ABS": {"bed_temp": 110}})");
    MaterialSettingsManager::instance().init();
    const auto first = read_overlay();

    // A settings restore from the rolling backup brings the key back.
    Config::get_instance()->get_json("/material_overrides") =
        nlohmann::json::parse(R"({"ABS": {"bed_temp": 110}})");
    TestAccess::reset(MaterialSettingsManager::instance());
    MaterialSettingsManager::instance().init();

    CHECK(read_overlay() == first);
    CHECK(read_overlay()["types"].size() == 1);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "a field already in the overlay wins the migration",
                 "[material_settings][migration]") {
    write_overlay(R"({"types": [{"name": "PLA", "bed": 70}]})");
    Config::get_instance()->get_json("/material_overrides") =
        nlohmann::json::parse(R"({"pla": {"bed_temp": 65, "nozzle_min": 205}})");

    MaterialSettingsManager::instance().init();

    auto pla = overlay_type("PLA");
    CHECK(pla["bed"] == 70);
    CHECK(pla["nozzle_min"] == 205);
    CHECK(read_overlay()["types"].size() == 1);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "a failed overlay write leaves settings intact",
                 "[material_settings][migration]") {
    // A regular file where the overlay's directory should be: nothing can be
    // created under it, so the save fails.
    const std::string blocker = helix::printer::detail::user_overlay_dir_ref() + "/blocker";
    std::ofstream(blocker) << "x";
    const std::string saved_dir = helix::printer::detail::user_overlay_dir_ref();
    helix::printer::detail::user_overlay_dir_ref() = blocker;

    Config::get_instance()->get_json("/material_overrides") =
        nlohmann::json::parse(R"({"PLA": {"bed_temp": 65}})");
    MaterialSettingsManager::instance().init();
    helix::printer::detail::user_overlay_dir_ref() = saved_dir;
    std::filesystem::remove(blocker);

    REQUIRE(Config::get_instance()->exists("/material_overrides"));
    CHECK(Config::get_instance()->get_json("/material_overrides")["PLA"]["bed_temp"] == 65);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "a malformed material_overrides is dropped",
                 "[material_settings][migration]") {
    Config::get_instance()->get_json("/material_overrides") = "junk";
    MaterialSettingsManager::instance().init();
    CHECK_FALSE(Config::get_instance()->exists("/material_overrides"));
    CHECK_FALSE(MaterialSettingsManager::instance().has_override("PLA"));
}

TEST_CASE_METHOD(MaterialSettingsFixture, "set_override writes the overlay, not settings.json",
                 "[material_settings]") {
    MaterialSettingsManager::instance().init();
    MaterialOverride ovr;
    ovr.bed_temp = 112;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    CHECK(overlay_type("ABS")["bed"] == 112);
    CHECK_FALSE(Config::get_instance()->exists("/material_overrides"));
}

TEST_CASE_METHOD(MaterialSettingsFixture, "overrides keep the hand-authored fields of an entry",
                 "[material_settings]") {
    write_overlay(R"({"types": [{"name": "PLA", "density": 1.3}]})");
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.bed_temp = 66;
    MaterialSettingsManager::instance().set_override("PLA", ovr);
    CHECK(overlay_type("PLA")["density"] == 1.3);
    CHECK(overlay_type("PLA")["bed"] == 66);

    MaterialSettingsManager::instance().clear_override("PLA");
    auto pla = overlay_type("PLA");
    REQUIRE(pla.is_object());
    CHECK(pla["density"] == 1.3);
    CHECK_FALSE(pla.contains("bed"));
    CHECK(find_material("PLA")->bed_temp == 60);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "clearing a user-defined type keeps its definition",
                 "[material_settings]") {
    write_overlay(R"({"types": [{"name": "PEKK", "nozzle_min": 330, "nozzle_max": 360}]})");
    MaterialSettingsManager::instance().init();
    REQUIRE(MaterialSettingsManager::instance().has_override("PEKK"));

    MaterialSettingsManager::instance().clear_override("PEKK");
    CHECK(overlay_type("PEKK")["nozzle_min"] == 330);
    CHECK(find_material("PEKK")->nozzle_min == 330);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "a hand-written 205.0 counts as an override",
                 "[material_settings]") {
    write_overlay(R"({"types": [{"name": "PLA", "nozzle_min": 205.0}]})");
    MaterialSettingsManager::instance().init();
    REQUIRE(MaterialSettingsManager::instance().has_override("PLA"));
    CHECK(MaterialSettingsManager::instance().get_override("PLA")->nozzle_min == 205);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "an unparseable overlay blocks the migration",
                 "[material_settings][migration]") {
    // Saving over it would replace the user's hand edits with the migrated
    // entries alone; settings keep the only copy until the file is fixed.
    std::ofstream(overlay_path()) << R"({"types": [ {"name": "PLA", )";
    Config::get_instance()->get_json("/material_overrides") =
        nlohmann::json::parse(R"({"PLA": {"bed_temp": 65}})");

    MaterialSettingsManager::instance().init();

    REQUIRE(Config::get_instance()->exists("/material_overrides"));
    CHECK(Config::get_instance()->get_json("/material_overrides")["PLA"]["bed_temp"] == 65);
    std::ifstream f(overlay_path());
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(body == R"({"types": [ {"name": "PLA", )");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "an empty overlay file does not block the migration",
                 "[material_settings][migration]") {
    // A zero-byte or whitespace-only file is an overlay nobody has written yet.
    const char* body = GENERATE("", " \n\t\r\n");
    std::ofstream(overlay_path()) << body;
    Config::get_instance()->get_json("/material_overrides") =
        nlohmann::json::parse(R"({"PLA": {"bed_temp": 65}})");

    MaterialSettingsManager::instance().init();

    CHECK_FALSE(Config::get_instance()->exists("/material_overrides"));
    CHECK(overlay_type("PLA")["bed"] == 65);
    CHECK(find_material("PLA")->bed_temp == 65);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "Custom profiles create edit rename and reload native types",
                 "[material_settings][material-profiles]") {
    auto& manager=MaterialSettingsManager::instance();manager.init();
    MaterialOverride profile;
    profile.nozzle_min=235;profile.nozzle_max=255;profile.bed_temp=100;profile.chamber_temp=55;
    REQUIRE(manager.save_user_profile(" Workshop ABS ",profile));
    auto material=find_material("Workshop ABS");REQUIRE(material);
    CHECK(material->user_defined);CHECK(material->chamber_temp_c==55);
    CHECK(overlay_type("Workshop ABS")["nozzle_min"]==235);
    profile.bed_temp=105;profile.chamber_temp=65;
    REQUIRE(manager.save_user_profile("Workshop ABS",profile,"Workshop ABS"));
    TestAccess::reset(manager);filament::reload_materials();manager.init();
    material=find_material("Workshop ABS");REQUIRE(material);
    CHECK(material->bed_temp==105);CHECK(material->chamber_temp_c==65);
    manager.set_preset_material(0,"Workshop ABS");
    REQUIRE(manager.save_user_profile("Workshop ASA",profile,"Workshop ABS"));
    CHECK_FALSE(find_material("Workshop ABS"));
    REQUIRE(find_material("Workshop ASA"));
    CHECK(manager.get_preset_materials()[0]=="Workshop ASA");
    CHECK(read_overlay()["types"].size()==1);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "Custom profile validation rejects empty duplicate alias and control names",
                 "[material_settings][material-profiles]") {
    auto& manager=MaterialSettingsManager::instance();manager.init();
    MaterialOverride profile;
    profile.nozzle_min=200;profile.nozzle_max=220;profile.bed_temp=60;profile.chamber_temp=0;
    for(const std::string name : {"", "   ", "PLA", "pla", "Bad\nName"}) {
        CHECK_FALSE(manager.validate_user_profile_name(name).empty());
        CHECK_FALSE(manager.save_user_profile(name,profile));
    }
    REQUIRE(manager.save_user_profile("My PLA",profile));
    CHECK_FALSE(manager.save_user_profile("my pla",profile));
    CHECK(manager.validate_user_profile_name("My PLA","My PLA").empty());
    CHECK_FALSE(manager.save_user_profile("PLA",profile,"My PLA"));
    CHECK_FALSE(manager.save_user_profile("Renamed builtin",profile,"PLA"));
    REQUIRE(find_material("My PLA"));
}

TEST_CASE_METHOD(MaterialSettingsFixture, "Custom profile write failure reports failure without replacing the overlay",
                 "[material_settings][material-profiles]") {
    struct RestoreDirectory {
        std::string directory=helix::printer::detail::user_overlay_dir_ref();
        std::string blocker=(directory.empty()?std::filesystem::temp_directory_path().string():directory)+
            "/profile-write-blocker-"+std::to_string(reinterpret_cast<std::uintptr_t>(this));
        ~RestoreDirectory() {
            helix::printer::detail::user_overlay_dir_ref()=directory;
            std::error_code ec;std::filesystem::remove(blocker,ec);
            filament::reload_materials();
        }
    } restore;
    std::ofstream(restore.blocker)<<"A file cannot be an overlay directory";
    REQUIRE(std::filesystem::is_regular_file(restore.blocker));
    helix::printer::detail::user_overlay_dir_ref()=restore.blocker;
    filament::reload_materials();
    MaterialOverride profile;
    profile.nozzle_min=200;profile.nozzle_max=220;profile.bed_temp=60;profile.chamber_temp=50;
    CHECK_FALSE(MaterialSettingsManager::instance().save_user_profile("Unwritten profile",profile));
    CHECK_FALSE(find_material("Unwritten profile"));
    std::ifstream file(restore.blocker);
    const std::string contents((std::istreambuf_iterator<char>(file)),{});
    CHECK(contents=="A file cannot be an overlay directory");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "Custom profile API rejects invalid complete numeric definitions",
                 "[material_settings][material-profiles]") {
    auto& manager=MaterialSettingsManager::instance();manager.init();
    MaterialOverride valid;
    valid.nozzle_min=200;valid.nozzle_max=220;valid.bed_temp=60;valid.chamber_temp=0;
    for(int invalid=0;invalid<8;++invalid) {
        auto profile=valid;
        switch(invalid) {
            case 0:profile.nozzle_min=0;break;
            case 1:profile.nozzle_min=230;break;
            case 2:profile.nozzle_max=501;break;
            case 3:profile.bed_temp=-1;break;
            case 4:profile.bed_temp=201;break;
            case 5:profile.chamber_temp=-1;break;
            case 6:profile.chamber_temp=121;break;
            case 7:profile.chamber_temp.reset();break;
        }
        CAPTURE(invalid);
        CHECK_FALSE(manager.save_user_profile("Invalid numeric profile",profile));
        CHECK_FALSE(find_material("Invalid numeric profile"));
    }
    valid.nozzle_min=100;valid.nozzle_max=500;valid.bed_temp=200;valid.chamber_temp=120;
    REQUIRE(manager.save_user_profile("Portable field boundaries",valid));
    REQUIRE(find_material("Portable field boundaries"));
}

TEST_CASE_METHOD(MaterialSettingsFixture, "A branded filament reference blocks rename but allows temperature editing",
                 "[material_settings][material-profiles]") {
    auto& manager=MaterialSettingsManager::instance();manager.init();
    MaterialOverride profile;
    profile.nozzle_min=200;profile.nozzle_max=220;profile.bed_temp=60;profile.chamber_temp=50;
    REQUIRE(manager.save_user_profile("Referenced profile",profile));
    auto document=read_overlay();
    document["filaments"]=nlohmann::json::array({
        {{"id","test.product"},{"brand","Test"},{"name","Profile filament"},{"type","Referenced profile"}}
    });
    std::ofstream(overlay_path())<<document.dump();
    filament::reload_materials();TestAccess::reset(manager);manager.init();
    CHECK(manager.validate_user_profile_name("Different profile","Referenced profile")==
          "This profile is used by a filament and cannot be renamed");
    CHECK_FALSE(manager.save_user_profile("Different profile",profile,"Referenced profile"));
    REQUIRE(find_material("Referenced profile"));CHECK_FALSE(find_material("Different profile"));
    profile.bed_temp=75;
    REQUIRE(manager.save_user_profile("Referenced profile",profile,"Referenced profile"));
    CHECK(find_material("Referenced profile")->bed_temp==75);
    CHECK(read_overlay()["filaments"][0]["type"]=="Referenced profile");
}
