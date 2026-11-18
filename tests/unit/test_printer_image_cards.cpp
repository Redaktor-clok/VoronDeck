// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../catch_amalgamated.hpp"
#include "ui_overlay_printer_image.h"
#include "ui_update_queue.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "wizard_config_paths.h"
#include "helix-xml/src/xml/lv_xml.h"

TEST_CASE_METHOD(LVGLUITestFixture, "Image cards preserve complete inventory and only change render selection",
                 "[printer_image][printer-image-cards]") {
    const bool portrait = GENERATE(false, true);
    ScopedResolution resolution(lv_display_get_default(), portrait ? 480 : 800, portrait ? 800 : 480);
    auto* orientation = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(orientation);
    const int old_orientation = lv_subject_get_int(orientation);
    lv_subject_set_int(orientation, portrait ? 1 : 0);
    auto* config = Config::get_instance();
    config->set<std::string>(config->df() + helix::wizard::PRINTER_NAME, "My existing printer");
    config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "Voron 0.2");
    auto& manager = helix::PrinterImageManager::instance();
    auto images = manager.get_shipped_images(printer_images::current_screen_width());
    REQUIRE(images.size() >= 4);
    auto& overlay = helix::settings::get_printer_image_overlay();
    REQUIRE(overlay.show(test_screen()));
    process_lvgl(30);
    auto* root = lv_obj_find_by_name(test_screen(), "printer_image_overlay");
    REQUIRE(root);
    auto* count = lv_xml_get_subject(nullptr, "printer_image_shipped_count");
    REQUIRE(count);
    auto* list = lv_obj_find_by_name(root, "shipped_images_list");
    REQUIRE(list);
    CHECK(lv_obj_get_height(list) > 0);
    CHECK(lv_subject_get_int(count) == static_cast<int>(images.size()));
    auto* title = lv_obj_find_by_name(root, "printer_image_current_name");
    REQUIRE(title);
    CHECK(std::string(lv_label_get_text(title)) == "My existing printer");
    const auto before = helix::ConfigTestAccess::data(*config);
    for (size_t i = 0; i < images.size(); ++i) {
        auto* card = lv_obj_find_by_name(root, ("printer_image_shipped_" + std::to_string(i)).c_str());
        REQUIRE(card);
        // A generic card_height token previously resolved to a global 80%,
        // creating a percent-height child inside its content-height list.
        const auto declared_height = lv_obj_get_style_height(card, LV_PART_MAIN);
        CHECK_FALSE(LV_COORD_IS_SPEC(declared_height));
        CHECK(declared_height >= 150);
        CHECK(declared_height <= 200);
        CHECK(lv_obj_get_height(card) == declared_height);
        auto* image = lv_obj_find_by_name(card, "printer_card_render");
        REQUIRE(image);
        REQUIRE(lv_image_get_src(image));
        CHECK(std::string(static_cast<const char*>(lv_image_get_src(image))) == images[i].preview_path);
        auto* name_subject = lv_xml_get_subject(nullptr,
            ("printer_image_shipped_" + std::to_string(i) + "_name").c_str());
        REQUIRE(name_subject);
        CHECK(std::string(lv_subject_get_string(name_subject)) == images[i].display_name);
        auto* label = lv_obj_find_by_name(card, "printer_card_name");
        REQUIRE(label);
        CHECK_FALSE(std::string(lv_label_get_text(label)).empty());
        lv_area_t card_bounds{}, label_bounds{};
        lv_obj_get_coords(card, &card_bounds);
        lv_obj_get_coords(label, &label_bounds);
        CHECK(label_bounds.x1 >= card_bounds.x1);
        CHECK(label_bounds.x2 <= card_bounds.x2);
        CHECK(label_bounds.y1 >= card_bounds.y1);
        CHECK(label_bounds.y2 <= card_bounds.y2);
    }
    auto* first = lv_obj_find_by_name(root, "printer_image_shipped_0");
    auto* second = lv_obj_find_by_name(root, "printer_image_shipped_1");
    auto* next = lv_obj_find_by_name(root, portrait ? "printer_image_shipped_2" : "printer_image_shipped_3");
    REQUIRE(first); REQUIRE(second); REQUIRE(next);
    lv_area_t a{}, b{}, c{};
    lv_obj_get_coords(first, &a); lv_obj_get_coords(second, &b); lv_obj_get_coords(next, &c);
    CHECK(b.y1 == a.y1); CHECK(b.x1 > a.x2); CHECK(c.y1 > a.y2);
    lv_obj_send_event(second, LV_EVENT_CLICKED, nullptr);
    process_lvgl(10);
    CHECK(manager.get_active_image_id() == images[1].id);
    CHECK(lv_obj_has_state(second, LV_STATE_CHECKED));
    CHECK_FALSE(lv_obj_has_state(first, LV_STATE_CHECKED));
    auto after = helix::ConfigTestAccess::data(*config);
    auto expected = before;
    expected[nlohmann::json::json_pointer(config->df() + helix::PRINTER_IMAGE)] = images[1].id;
    CHECK(after == expected);
    overlay.handle_auto_detect();
    CHECK(manager.get_active_image_id().empty());
    CHECK_FALSE(lv_obj_has_state(second, LV_STATE_CHECKED));
    CHECK(lv_subject_get_int(count) == static_cast<int>(images.size()));
    overlay.close();
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_set_int(orientation, old_orientation);
}
