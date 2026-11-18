// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/discovery_of_objects.h"
#include "../test_helpers/sensor_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "home_screen_preferences.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/home_sensor_bank_widget.h"
#include "temperature_sensor_manager.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
class HomeSensorBankFixture : public LVGLUITestFixture {
  protected:
    Config config;
    Config* previous_config = nullptr;
    sensors::TemperatureSensorManager& manager = sensors::TemperatureSensorManager::instance();

    HomeSensorBankFixture() {
        previous_config = ConfigTestAccess::instance_ref();
        ConfigTestAccess::instance_ref() = &config;
        setup_printer_data(config, nlohmann::json::object());
        manager.set_sync_mode(true);
        sensors::SensorManagerTestAccess::reconcile(manager, {});
        manager.deinit_subjects();
        manager.init_subjects();
    }
    ~HomeSensorBankFixture() override {
        ui::UpdateQueueTestAccess::drain_all(ui::UpdateQueue::instance());
        sensors::SensorManagerTestAccess::reconcile(manager, {});
        manager.deinit_subjects();
        ConfigTestAccess::instance_ref() = previous_config;
    }
    int count() {
        auto* subject = lv_xml_get_subject(nullptr, "home_sensor_bank_sensor_count");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    }
    std::string text(const char* name) {
        auto* subject = lv_xml_get_subject(nullptr, name);
        REQUIRE(subject != nullptr);
        return lv_subject_get_string(subject);
    }
    void drain() { ui::UpdateQueueTestAccess::drain_all(ui::UpdateQueue::instance()); }
};
} // namespace

TEST_CASE_METHOD(HomeSensorBankFixture, "Sensor bank filters heater and full-ID home preferences",
                 "[home-sensor-bank]") {
    manager.discover(test::discovery_of({"temperature_sensor mcu", "temperature_sensor host",
                                       "heater_generic chamber", "temperature_fan exhaust"}));
    HomeScreenPreferences(config).set_visible(HomeScreenPreferences::DeviceKind::PassiveSensor,
                                              "temperature_sensor host", false);
    HomeSensorBankWidget bank;
    CHECK(count() == 2);
    REQUIRE(lv_xml_get_subject(nullptr, "home_sensor_bank_sensor_1_name") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "home_sensor_bank_sensor_1_temp") != nullptr);
    HomeScreenPreferences(config).set_visible(HomeScreenPreferences::DeviceKind::PassiveSensor,
                                              "temperature_sensor mcu", false);
    HomeScreenPreferences(config).set_visible(HomeScreenPreferences::DeviceKind::PassiveSensor,
                                              "temperature_fan exhaust", false);
    bank.on_activate();
    CHECK(count() == 0);
}

TEST_CASE_METHOD(HomeSensorBankFixture, "Sensor bank reads live temperatures and equal-count rediscovery",
                 "[home-sensor-bank]") {
    manager.discover(test::discovery_of({"temperature_sensor mcu"}));
    manager.update_from_status({{"temperature_sensor mcu", {{"temperature", 42.0}}}});
    HomeSensorBankWidget bank;
    CHECK(count() == 1);
    CHECK(text("home_sensor_bank_sensor_0_temp") == "42°C");
    bank.attach(nullptr, nullptr);
    manager.update_from_status({{"temperature_sensor mcu", {{"temperature", 51.0}}}});
    drain();
    CHECK(text("home_sensor_bank_sensor_0_temp") == "51°C");
    const auto first_name = text("home_sensor_bank_sensor_0_name");
    manager.discover(test::discovery_of({"temperature_sensor host"}));
    manager.update_from_status({{"temperature_sensor host", {{"temperature", 35.0}}}});
    drain();
    CHECK(count() == 1);
    CHECK(text("home_sensor_bank_sensor_0_name") != first_name);
    CHECK(text("home_sensor_bank_sensor_0_temp") == "35°C");
    // The observer also remains safe when its subject owner dies first.
    manager.deinit_subjects();
    bank.detach();
    drain();
}

TEST_CASE("Sensor bank is an optional widget", "[home-sensor-bank]") {
    auto* definition = find_widget_def("home_sensor_bank");
    REQUIRE(definition != nullptr);
    CHECK_FALSE(definition->default_enabled);
}

TEST_CASE_METHOD(HomeSensorBankFixture, "Sensor bank creates and updates actual XML rows",
                 "[home-sensor-bank][home-design]") {
    manager.discover(test::discovery_of({"temperature_sensor mcu"}));
    manager.update_from_status({{"temperature_sensor mcu", {{"temperature", 42.0}}}});
    HomeSensorBankWidget bank;
    auto* view = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), bank.get_component_name().c_str(), bank.xml_attrs()));
    REQUIRE(view != nullptr);
    REQUIRE(lv_obj_get_child_count(view) == 1);
    CHECK(text("home_sensor_bank_sensor_0_name") == "MCU");
    auto* reading = lv_obj_find_by_name(view, "sensor_temperature");
    REQUIRE(reading != nullptr);
    CHECK(std::string(lv_label_get_text(reading)) == "42°C");
    bank.attach(view, test_screen());
    manager.update_from_status({{"temperature_sensor mcu", {{"temperature", 51.0}}}});
    drain();
    CHECK(std::string(lv_label_get_text(reading)) == "51°C");
    HomeScreenPreferences(config).set_visible(HomeScreenPreferences::DeviceKind::PassiveSensor,
                                              "temperature_sensor mcu", false);
    bank.on_activate();
    drain();
    CHECK(lv_obj_get_child_count(view) == 0);
}
