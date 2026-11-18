// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/gcode_recording_api.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_client_mock.h"
#include "panel_widget_registry.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/cooldown_widget.h"
#include "temperature_sensor_manager.h"
#include "translation_loader.h"
#include "ui_icon_codepoints.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "../catch_amalgamated.hpp"

namespace {
class CooldownFixture : public LVGLUITestFixture {
  public:
    CooldownFixture() : api_(client_, state()) {
        saved_api_ = get_moonraker_api();
        set_moonraker_api(&api_);
        saved_config_ = helix::ConfigTestAccess::instance_ref();
        helix::setup_printer_data(config_, nlohmann::json::object());
        helix::ConfigTestAccess::instance_ref() = &config_;
        helix::register_cooldown_widget();
        lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"), 2);
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json::array({"extruder", "extruder1", "heater_bed",
                                               "heater_generic aux4", "temperature_sensor mcu"}));
        helix::sensors::TemperatureSensorManager::instance().discover(hw);
        state().temperature_state().init_extruders({"extruder", "extruder1"});
    }
    ~CooldownFixture() override {
        helix::ui::set_test_notification_error_hook(nullptr);
        helix::ui::set_test_notification_success_hook(nullptr);
        set_moonraker_api(saved_api_);
        helix::ConfigTestAccess::instance_ref() = saved_config_;
    }
    lv_obj_t* button() {
        auto* root = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "panel_widget_cooldown", nullptr));
        REQUIRE(root != nullptr);
        auto* icon = lv_obj_find_by_name(root, "cooldown_icon");
        REQUIRE(icon != nullptr);
        CHECK(std::string(lv_label_get_text(icon)) == helix::ui::icon::lookup_codepoint("cooldown"));
        auto* button = lv_obj_find_by_name(root, "cooldown_button");
        REQUIRE(button != nullptr);
        return button;
    }
    MoonrakerClientMock client_;
    helix::test::GcodeRecordingApi api_;
    helix::Config config_;
    helix::IMoonrakerAPI* saved_api_ = nullptr;
    helix::Config* saved_config_ = nullptr;
};
} // namespace

TEST_CASE_METHOD(CooldownFixture, "Cooldown tile click disables every discovered heater",
                 "[cooldown_widget]") {
    auto* def = helix::find_widget_def("cooldown");
    REQUIRE(def != nullptr);
    CHECK(def->category == helix::WidgetCategory::Temperature);
    CHECK(std::string(def->icon) == "cooldown");
    CHECK_FALSE(def->default_enabled);
    CHECK(def->effective_min_colspan() <= 2);
    CHECK(def->effective_min_rowspan() <= 2);
    auto* btn = button();
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    REQUIRE(api_.sent.size() == 1);
    CHECK(api_.contains("HEATER=extruder TARGET=0"));
    CHECK(api_.contains("HEATER=extruder1 TARGET=0"));
    CHECK(api_.contains("HEATER=heater_bed TARGET=0"));
    CHECK(api_.contains("HEATER=aux4 TARGET=0"));
    CHECK_FALSE(api_.contains("mcu"));
    CHECK_FALSE(api_.contains("TARGET=1"));
    CHECK_FALSE(api_.contains("M104"));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"), 0);
    CHECK(lv_obj_has_state(btn, LV_STATE_DISABLED));
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    CHECK(api_.sent.size() == 1);
}

TEST_CASE_METHOD(CooldownFixture, "Cooldown preserves custom macro and reports command errors",
                 "[cooldown_widget]") {
    helix::ConfigTestAccess::data(config_)["printers"]["default"]["default_macros"]["cooldown"] =
        "MY_COOLDOWN";
    api_.fail.insert("MY_COOLDOWN");
    std::vector<std::string> errors;
    std::vector<std::string> successes;
    helix::ui::set_test_notification_error_hook(
        [&errors](const std::string& text) { errors.push_back(text); });
    helix::ui::set_test_notification_success_hook(
        [&successes](const std::string& text) { successes.push_back(text); });
    lv_obj_send_event(button(), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(api_.sent.size() == 1);
    CHECK(api_.sent.front() == "MY_COOLDOWN");
    CHECK(errors.size() == 1);
    CHECK(successes.empty());
    helix::ui::set_test_notification_error_hook(nullptr);
    helix::ui::set_test_notification_success_hook(nullptr);
}

TEST_CASE_METHOD(CooldownFixture, "Cooldown rejects malformed auxiliary heater identifiers",
                 "[cooldown_widget]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"heater_generic aux4\nM104 S250"}));
    helix::sensors::TemperatureSensorManager::instance().discover(hw);
    lv_obj_send_event(button(), LV_EVENT_CLICKED, nullptr);
    REQUIRE(api_.sent.size() == 1);
    CHECK_FALSE(api_.contains("M104"));
    CHECK_FALSE(api_.contains("S250"));
}

TEST_CASE_METHOD(CooldownFixture, "Cooldown content follows the granted shape without moving its box",
                 "[cooldown_widget][layout]") {
    struct RestoreLanguage {
        ~RestoreLanguage() { lv_translation_set_language(helix::ui::kIdentityLocale); }
    } restore_language;
    helix::ui::ensure_translation_loaded("ru");
    lv_translation_set_language("ru");
    REQUIRE(std::string(lv_tr("Cool Down")) == "Охладить");
    helix::CooldownWidget widget;
    lv_subject_set_int(lv_xml_get_subject(nullptr, "show_widget_labels"), 1);
    auto* root = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), widget.get_component_name().c_str(), widget.xml_attrs()));
    REQUIRE(root != nullptr);
    widget.tile_sizing()->set_content_root(root);
    widget.attach(root, test_screen());
    auto* button = lv_obj_find_by_name(root, "cooldown_button");
    auto* icon = lv_obj_find_by_name(root, "cooldown_icon");
    REQUIRE(button != nullptr);
    REQUIRE(icon != nullptr);
    lv_obj_t* label = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        auto* child = lv_obj_get_child(button, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class) &&
            std::string(lv_label_get_text(child)) == lv_tr("Cool Down")) {
            label = child;
        }
    }
    REQUIRE(label != nullptr);
    auto* direction = lv_xml_get_subject(nullptr, "cooldown_layout_direction");
    REQUIRE(direction != nullptr);
    struct Shape { int width; int height; int direction; };
    for (const auto shape : {Shape{115, 103, 0}, Shape{100, 200, 0},
                             Shape{200, 100, 1}, Shape{100, 200, 0}}) {
        CAPTURE(shape.width, shape.height);
        lv_obj_set_pos(root, 17, 23);
        lv_obj_set_size(root, shape.width, shape.height);
        widget.notify_size_changed(shape.direction ? 4 : 2, shape.direction ? 2 : 4,
                                   shape.width, shape.height);
        process_lvgl(1);
        lv_obj_update_layout(root);
        CHECK(lv_subject_get_int(direction) == shape.direction);
        CHECK(lv_obj_get_style_flex_flow(button, LV_PART_MAIN) ==
              (shape.direction ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN));
        CHECK(lv_obj_get_x(root) == 17);
        CHECK(lv_obj_get_y(root) == 23);
        CHECK(lv_obj_get_width(root) == shape.width);
        CHECK(lv_obj_get_height(root) == shape.height);
        CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
        lv_area_t icon_box{}, label_box{}, button_box{};
        lv_obj_get_coords(icon, &icon_box);
        lv_obj_get_coords(label, &label_box);
        lv_obj_get_coords(button, &button_box);
        INFO("icon bounds: " << icon_box.x1 << "," << icon_box.y1 << " to " << icon_box.x2
                            << "," << icon_box.y2);
        INFO("label bounds: " << label_box.x1 << "," << label_box.y1 << " to " << label_box.x2
                             << "," << label_box.y2);
        INFO("button bounds: " << button_box.x1 << "," << button_box.y1 << " to " << button_box.x2
                              << "," << button_box.y2);
        if (shape.direction) {
            CHECK(icon_box.x2 < label_box.x1);
        } else {
            CHECK(icon_box.y2 < label_box.y1);
        }
        CHECK(icon_box.x1 >= button_box.x1);
        CHECK(icon_box.y1 >= button_box.y1);
        CHECK(icon_box.x2 <= button_box.x2);
        CHECK(icon_box.y2 <= button_box.y2);
        CHECK(label_box.x1 >= button_box.x1);
        CHECK(label_box.y1 >= button_box.y1);
        CHECK(label_box.x2 <= button_box.x2);
        CHECK(label_box.y2 <= button_box.y2);
    }
    widget.detach();
    lv_obj_delete(root);
}
