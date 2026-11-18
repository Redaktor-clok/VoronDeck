// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_carousel.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../test_helpers/thermistor_test_access.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "home_screen_preferences.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/fan_stack_widget.h"
#include "src/ui/panel_widgets/thermistor_widget.h"
#include "temperature_sensor_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using DeviceKind = HomeScreenPreferences::DeviceKind;

namespace {

constexpr const char* kSensorA = "temperature_sensor projection_mcu";
constexpr const char* kSensorB = "temperature_sensor projection_host";
constexpr const char* kHeater = "heater_generic projection_dryer";

/// The same XML attributes and attach path as PanelWidgetManager, with panel
/// identity supplied before creation rather than the size harness's default.
class ProjectionTile {
  public:
    ProjectionTile(PanelWidget& widget, lv_obj_t* screen, const char* panel) : widget_(widget) {
        widget_.set_panel_id(panel);
        root = static_cast<lv_obj_t*>(
            lv_xml_create(screen, widget_.get_component_name().c_str(), widget_.xml_attrs()));
        REQUIRE(root != nullptr);
        widget_.attach_tile(root, screen);
    }
    ~ProjectionTile() {
        widget_.detach_tile();
        lv_obj_delete(root);
    }
    lv_obj_t* root = nullptr;

  private:
    PanelWidget& widget_;
};

class HomeProjectionFixture : public LVGLUITestFixture {
  public:
    HomeScreenPreferences preferences{*Config::get_instance()};

    HomeProjectionFixture() {
        auto& manager = sensors::TemperatureSensorManager::instance();
        manager.init_subjects();
        manager.discover(helix::test::discovery_of({kSensorA, kSensorB, kHeater}));
        state().fan_state().init_fans({"fan", "heater_fan hotend", "fan_generic enclosure"});
        state().fan_state().update_fan_speed("fan", 0.35);
        helix::ui::UpdateQueue::instance().drain();
    }
    ~HomeProjectionFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        sensors::TemperatureSensorManager::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(HomeProjectionFixture,
                 "Home sensor projection filters pages without changing inventory",
                 "[home-preferences][home-projection][thermistor]") {
    preferences.set_visible(DeviceKind::PassiveSensor, kSensorA, false);
    preferences.set_visible(DeviceKind::Heater, kHeater, false);
    ThermistorWidget home("home_projection_sensors");
    home.set_config({{"display_mode", "carousel"}, {"sensors", {kSensorA, kSensorB, kHeater}}});
    ProjectionTile tile(home, test_screen(), "home");
    process_lvgl(10);
    CHECK(ThermistorTestAccess::carousel_page_count(home) == 1);
    CHECK_FALSE(lv_obj_has_flag(tile.root, LV_OBJ_FLAG_HIDDEN));

    preferences.set_visible(DeviceKind::PassiveSensor, kSensorB, false);
    ThermistorTestAccess::bind_carousel(home);
    process_lvgl(10);
    CHECK(ThermistorTestAccess::carousel_page_count(home) == 0);
    CHECK(lv_obj_has_flag(tile.root, LV_OBJ_FLAG_HIDDEN));
    CHECK(sensors::TemperatureSensorManager::instance().get_sensors().size() == 3);

    ThermistorWidget controls("controls_projection_sensors");
    controls.set_config({{"display_mode", "carousel"}, {"sensors", {kSensorA, kSensorB, kHeater}}});
    ProjectionTile controls_tile(controls, test_screen(), "controls");
    process_lvgl(10);
    CHECK(ThermistorTestAccess::carousel_page_count(controls) == 3);
    CHECK_FALSE(lv_obj_has_flag(controls_tile.root, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(HomeProjectionFixture, "Home single sensor choice is hidden only on Home",
                 "[home-preferences][home-projection][thermistor]") {
    preferences.set_visible(DeviceKind::PassiveSensor, kSensorA, false);
    ThermistorWidget home("home_projection_single");
    home.set_config({{"sensor", kSensorA}});
    ProjectionTile tile(home, test_screen(), "home");
    CHECK(lv_obj_has_flag(tile.root, LV_OBJ_FLAG_HIDDEN));

    ThermistorWidget controls("controls_projection_single");
    controls.set_config({{"sensor", kSensorA}});
    ProjectionTile controls_tile(controls, test_screen(), "controls");
    CHECK_FALSE(lv_obj_has_flag(controls_tile.root, LV_OBJ_FLAG_HIDDEN));
    CHECK(sensors::TemperatureSensorManager::instance().get_sensors().size() == 3);
}

TEST_CASE_METHOD(HomeProjectionFixture,
                 "Home fan stack hides individual rows without changing speeds",
                 "[home-preferences][home-projection][fan_stack]") {
    preferences.set_visible(DeviceKind::Fan, "fan", false);
    FanStackWidget home("home_projection_fans", state());
    ProjectionTile tile(home, test_screen(), "home");
    CHECK(
        lv_obj_has_flag(lv_obj_find_by_name(tile.root, "fan_stack_part_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(tile.root, "fan_stack_hotend_row"),
                                LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(tile.root, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(state().fan_state().get_fans().size() == 3);
    for (const auto& fan : state().fan_state().get_fans()) {
        if (fan.object_name == "fan") {
            CHECK(fan.speed_percent == 35);
        }
    }

    FanStackWidget controls("controls_projection_fans", state());
    ProjectionTile controls_tile(controls, test_screen(), "controls");
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(controls_tile.root, "fan_stack_part_row"),
                                LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(HomeProjectionFixture,
                 "Home fan carousel hide-all does not create placeholder devices",
                 "[home-preferences][home-projection][fan_stack]") {
    preferences.set_all_visible(DeviceKind::Fan,
                                {"fan", "heater_fan hotend", "fan_generic enclosure"}, false);
    FanStackWidget home("home_projection_fan_carousel", state());
    home.set_config({{"display_mode", "carousel"}});
    ProjectionTile tile(home, test_screen(), "home");
    auto* carousel = lv_obj_find_by_name(tile.root, "fan_carousel");
    REQUIRE(carousel != nullptr);
    REQUIRE(ui_carousel_get_state(carousel) != nullptr);
    CHECK(ui_carousel_get_state(carousel)->real_tiles.empty());
    CHECK(lv_obj_has_flag(tile.root, LV_OBJ_FLAG_HIDDEN));

    FanStackWidget controls("controls_projection_fan_carousel", state());
    controls.set_config({{"display_mode", "carousel"}});
    ProjectionTile controls_tile(controls, test_screen(), "controls");
    auto* controls_carousel = lv_obj_find_by_name(controls_tile.root, "fan_carousel");
    REQUIRE(controls_carousel != nullptr);
    REQUIRE(ui_carousel_get_state(controls_carousel) != nullptr);
    CHECK(ui_carousel_get_state(controls_carousel)->real_tiles.size() == 3);
    CHECK(state().fan_state().get_fans().size() == 3);
}
