// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "printer_temperature_state.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "Home heater tiles show reported duty only with a target",
                 "[vorondeck][home][heater_power]") {
    auto& temperatures = state().temperature_state();
    struct Heater {
        const char* component;
        lv_subject_t* power;
        lv_subject_t* target;
        lv_subject_t* mode = nullptr;
    };
    const Heater heaters[] = {
        {"panel_widget_temperature", temperatures.get_extruder_power_subject(),
         temperatures.get_active_extruder_target_subject()},
        {"panel_widget_bed_temperature", temperatures.get_bed_power_subject(),
         temperatures.get_bed_target_subject()},
        {"panel_widget_chamber_temperature", temperatures.get_chamber_power_subject(),
         temperatures.get_chamber_effective_target_subject(),
         temperatures.get_chamber_mode_subject()},
    };

    for (const auto& heater : heaters) {
        DYNAMIC_SECTION(heater.component) {
            lv_subject_set_int(heater.power, -1);
            lv_subject_set_int(heater.target, 0);
            if (heater.mode)
                lv_subject_set_int(heater.mode, helix::ChamberMode::Off);
            auto* tile =
                static_cast<lv_obj_t*>(lv_xml_create(test_screen(), heater.component, nullptr));
            REQUIRE(tile != nullptr);
            auto* label = lv_obj_find_by_name(tile, "heater_power");
            REQUIRE(label != nullptr);
            CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));

            auto settle = [&](int power, int target) {
                lv_subject_set_int(heater.power, power);
                lv_subject_set_int(heater.target, target);
                if (heater.mode)
                    lv_subject_set_int(heater.mode, target > 0 ? helix::ChamberMode::Heating
                                                               : helix::ChamberMode::Off);
                helix::ui::UpdateQueue::instance().drain();
            };

            settle(78, 2200);
            CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
            CHECK(std::string(lv_label_get_text(label)) == "78%");

            // A maintained target may need zero duty. Zero is known data.
            settle(0, 2200);
            CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
            CHECK(std::string(lv_label_get_text(label)) == "0%");

            // Clearing the target hides old power immediately, even while hot.
            settle(78, 0);
            CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));

            // An unreported field never turns into a fabricated zero percent.
            settle(-1, 2200);
            CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
            if (heater.mode) {
                // A chamber maintained by a temperature fan is not heater power.
                settle(78, 350);
                lv_subject_set_int(heater.mode, helix::ChamberMode::Maintaining);
                helix::ui::UpdateQueue::instance().drain();
                CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
            }
        }
    }
}
