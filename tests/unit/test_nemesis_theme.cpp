// SPDX-License-Identifier: GPL-3.0-or-later
#include "nemesis_theme.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/config_dir_guard.h"
#include "../catch_amalgamated.hpp"
#include "printer_image_manager.h"
#include "theme_manager.h"
#include "wizard_config_paths.h"
#include "helix-xml/src/xml/lv_xml.h"

TEST_CASE_METHOD(LVGLUITestFixture, "NEMESIS image applies its theme and restores Voron without changing printer data",
                 "[nemesis-theme]") {
    auto* config = Config::get_instance();
    const auto original = helix::ConfigTestAccess::data(*config);
    auto& images = helix::PrinterImageManager::instance();
    config->set<std::string>(config->df() + helix::PRINTER_IMAGE, "shipped:voron-v2");
    config->set<bool>("/dark_mode", false);
    config->set<std::string>("/display/theme", helix::kDefaultTheme);
    auto before = helix::ConfigTestAccess::data(*config);
    const auto nemesis = helix::load_theme_from_file(helix::kNemesisTheme);
    REQUIRE(nemesis.is_valid());
    REQUIRE(nemesis.get_mode_support() == helix::ThemeModeSupport::DARK_ONLY);
    CHECK(nemesis.dark.text == "#FFFFFF");
    CHECK(nemesis.dark.primary == "#B8E62E");
    CHECK(helix::PrinterImageManager::format_display_name("nemesis-2-4-awd-cnc-530") == "NEMESIS 530 CNC");
    images.set_active_image(helix::kNemesisImage);
    CHECK(theme_manager_get_active_theme().filename == helix::kNemesisTheme);
    CHECK(theme_manager_is_dark_mode());
    const auto choices = helix::discover_picker_themes(helix::get_themes_directory());
    REQUIRE(choices.size() == 1);
    CHECK(choices[0].filename == helix::kNemesisTheme);
    auto* brand = lv_xml_get_subject(nullptr, "nemesis_theme");
    REQUIRE(brand);
    CHECK(lv_subject_get_int(brand) == 1);
    auto expected = before;
    expected[nlohmann::json::json_pointer(config->df() + helix::PRINTER_IMAGE)] = helix::kNemesisImage;
    CHECK(helix::ConfigTestAccess::data(*config) == expected);
    // A same-image confirmation must not reset the user's configuration.
    images.set_active_image(helix::kNemesisImage);
    CHECK(helix::ConfigTestAccess::data(*config) == expected);
    images.set_active_image("shipped:voron-v2");
    CHECK(theme_manager_get_active_theme().filename == helix::kDefaultTheme);
    CHECK_FALSE(theme_manager_is_dark_mode());
    CHECK(lv_subject_get_int(brand) == 0);
    CHECK(helix::ConfigTestAccess::data(*config) == before);
    // Session initialization follows the newly active profile without writing its data.
    helix::ConfigDirGuard scratch("nemesis_theme");
    config->set<std::string>(config->df() + helix::PRINTER_IMAGE, helix::kNemesisImage);
    images.init(scratch.dir.string());
    CHECK(lv_subject_get_int(brand) == 1);
    auto custom = nemesis;
    custom.filename = "custom-nemesis";
    theme_manager_apply_theme(custom, true);
    CHECK(lv_subject_get_int(brand) == 1);
    config->set<std::string>(config->df() + helix::PRINTER_IMAGE, "shipped:voron-v2");
    images.init(scratch.dir.string());
    CHECK(lv_subject_get_int(brand) == 0);
    CHECK_FALSE(theme_manager_is_dark_mode());
    helix::ConfigTestAccess::data(*config) = original;
}
