// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_heater_icon_binder.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"
#include "ui_temperature_utils.h"
#include "../test_helpers/scoped_animations_enabled.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "subject_managed_panel.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

bool hidden(lv_obj_t* root, const char* name) {
    lv_obj_t* object = lv_obj_find_by_name(root, name);
    REQUIRE(object != nullptr);
    return lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Home nozzle removes heat waves when target is off",
                 "[vorondeck][home][heater-icons]") {
    auto& temperatures = state().temperature_state();
    struct Heater {
        const char* component;
        const char* cold;
        const char* active;
        lv_subject_t* current;
        lv_subject_t* target;
        helix::HeaterType type;
    };
    const Heater heaters[] = {
        {"home_nozzle_icon", "home_nozzle_cold_glyph", "nozzle_icon_glyph",
         temperatures.get_active_extruder_temp_subject(),
         temperatures.get_active_extruder_target_subject(), helix::HeaterType::Nozzle},
    };
    for (const auto& heater : heaters) {
        DYNAMIC_SECTION(heater.component) {
            // A cooling hot heater still has no heating waves once disabled.
            lv_subject_set_int(heater.current, 1000);
            lv_subject_set_int(heater.target, 0);
            const char* attrs[] = {"badge_subject", "", nullptr};
            auto* root = static_cast<lv_obj_t*>(
                lv_xml_create(test_screen(), heater.component, attrs));
            REQUIRE(root != nullptr);
            CHECK_FALSE(hidden(root, heater.cold));
            CHECK(hidden(root, heater.active));

            // The normal native animator must still find the active glyph.
            helix::ui::HeaterIconBinder binder;
            REQUIRE(binder.bind(root, state(), heater.type));
            lv_subject_set_int(heater.target, 2000);
            helix::ui::UpdateQueue::instance().drain();
            CHECK(hidden(root, heater.cold));
            CHECK_FALSE(hidden(root, heater.active));
            CHECK(binder.is_bound());

            lv_subject_set_int(heater.target, 0);
            helix::ui::UpdateQueue::instance().drain();
            CHECK_FALSE(hidden(root, heater.cold));
            CHECK(hidden(root, heater.active));
            binder.unbind();
        }
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Home nozzle follows its supplied tool target rather than another tool",
                 "[vorondeck][home][heater-icons]") {
    SubjectManager subjects;
    lv_subject_t picked_target{};
    UI_MANAGED_SUBJECT_INT(picked_target, 0, "home_icon_picked_target", subjects);
    const char* attrs[] = {"target_subject", "home_icon_picked_target", "badge_subject", "", nullptr};
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "home_nozzle_icon", attrs));
    REQUIRE(root != nullptr);
    auto* cold = lv_obj_find_by_name(root, "home_nozzle_cold_glyph");
    auto* active = lv_obj_find_by_name(root, "nozzle_icon_glyph");
    REQUIRE(cold != nullptr);
    REQUIRE(active != nullptr);
    REQUIRE(helix::ui::icon::lookup_codepoint("vorondeck_nozzle") != nullptr);
    REQUIRE(helix::ui::icon::lookup_codepoint("vorondeck_nozzle_heat") != nullptr);
    CHECK(std::string(lv_label_get_text(cold)) == helix::ui::icon::lookup_codepoint("vorondeck_nozzle"));
    CHECK(std::string(lv_label_get_text(active)) == helix::ui::icon::lookup_codepoint("vorondeck_nozzle_heat"));

    lv_subject_set_int(state().temperature_state().get_active_extruder_target_subject(), 2100);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(hidden(root, "home_nozzle_cold_glyph"));
    CHECK(hidden(root, "nozzle_icon_glyph"));

    lv_subject_set_int(&picked_target, 2100);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(hidden(root, "home_nozzle_cold_glyph"));
    CHECK_FALSE(hidden(root, "nozzle_icon_glyph"));
    lv_obj_delete(root);
    subjects.deinit_all();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Home heater glyphs retain gray idle, pulsing heat and green reached",
                 "[vorondeck][home][heater-icons]") {
    helix::ui::ScopedAnimationsEnabled animations;
    REQUIRE(animations.available());
    animations.set(true);
    using helix::ui::temperature::HeatState;
    using helix::ui::temperature::get_heating_state_color;
    auto& temperatures = state().temperature_state();
    for (auto type : {helix::HeaterType::Nozzle, helix::HeaterType::Bed}) {
        const bool nozzle = type == helix::HeaterType::Nozzle;
        DYNAMIC_SECTION((nozzle ? "nozzle" : "bed")) {
            auto* current = nozzle ? temperatures.get_active_extruder_temp_subject()
                                   : temperatures.get_bed_temp_subject();
            auto* target = nozzle ? temperatures.get_active_extruder_target_subject()
                                  : temperatures.get_bed_target_subject();
            lv_subject_set_int(current, 1000);
            lv_subject_set_int(target, 0);
            const char* attrs[] = {"badge_subject", "", nullptr};
            auto* root = static_cast<lv_obj_t*>(lv_xml_create(
                test_screen(), nozzle ? "home_nozzle_icon" : "home_bed_icon", attrs));
            REQUIRE(root != nullptr);
            auto* glyph =
                lv_obj_find_by_name(root, nozzle ? "nozzle_icon_glyph" : "bed_icon_glyph");
            REQUIRE(glyph != nullptr);
            const std::string expected = helix::ui::icon::lookup_codepoint(
                nozzle ? "vorondeck_nozzle_heat" : "radiator");
            CHECK(std::string(lv_label_get_text(glyph)) == expected);
            if (!nozzle) {
                CHECK_FALSE(hidden(root, "bed_icon_glyph"));
                CHECK(lv_obj_find_by_name(root, "home_bed_cold_platform") == nullptr);
            }
            const auto idle_animations = lv_anim_count_running();
            helix::ui::HeaterIconBinder binder;
            REQUIRE(binder.bind(root, state(), type));
            CHECK(lv_color_eq(lv_obj_get_style_text_color(glyph, LV_PART_MAIN),
                              get_heating_state_color(HeatState::Off)));

            lv_subject_set_int(target, 2000);
            helix::ui::UpdateQueue::instance().drain();
            CHECK_FALSE(lv_obj_has_flag(glyph, LV_OBJ_FLAG_HIDDEN));
            CHECK(lv_color_eq(lv_obj_get_style_text_color(glyph, LV_PART_MAIN),
                              get_heating_state_color(HeatState::Heating)));
            CHECK(lv_anim_count_running() > idle_animations);

            lv_subject_set_int(current, 2000);
            helix::ui::UpdateQueue::instance().drain();
            CHECK(lv_color_eq(lv_obj_get_style_text_color(glyph, LV_PART_MAIN),
                              get_heating_state_color(HeatState::AtTemp)));
            CHECK(lv_anim_count_running() == idle_animations);
            CHECK(std::string(lv_label_get_text(glyph)) == expected);

            lv_subject_set_int(target, 0);
            helix::ui::UpdateQueue::instance().drain();
            CHECK(lv_color_eq(lv_obj_get_style_text_color(glyph, LV_PART_MAIN),
                              get_heating_state_color(HeatState::Off)));
            if (!nozzle)
                CHECK_FALSE(hidden(root, "bed_icon_glyph"));
            binder.unbind();
            lv_obj_delete(root);
        }
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "Home chamber distinguishes a heater from a passive probe and cooling",
                 "[vorondeck][home][heater-icons]") {
    auto& temperatures = state().temperature_state();
    auto* has_heater = lv_xml_get_subject(nullptr, "printer_has_chamber_heater");
    REQUIRE(has_heater != nullptr);
    lv_subject_set_int(has_heater, 1);
    lv_subject_set_int(temperatures.get_chamber_mode_subject(), helix::ChamberMode::Heating);
    lv_subject_set_int(temperatures.get_chamber_effective_target_subject(), 0);
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "home_chamber_icon", nullptr));
    REQUIRE(root != nullptr);
    CHECK_FALSE(hidden(root, "home_chamber_cold_glyph"));
    CHECK(hidden(root, "chamber_icon_glyph"));
    CHECK(hidden(root, "home_chamber_sensor_glyph"));

    helix::ui::HeaterIconBinder binder;
    REQUIRE(binder.bind(root, state(), helix::HeaterType::Chamber));
    lv_subject_set_int(temperatures.get_chamber_effective_target_subject(), 550);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(hidden(root, "home_chamber_cold_glyph"));
    CHECK_FALSE(hidden(root, "chamber_icon_glyph"));
    auto* flame = lv_obj_find_by_name(root, "chamber_icon_glyph");
    CHECK(std::string(lv_label_get_text(flame)) == helix::ui::icon::lookup_codepoint("fire"));

    lv_subject_set_int(temperatures.get_chamber_mode_subject(), helix::ChamberMode::Maintaining);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(hidden(root, "home_chamber_cold_glyph"));
    CHECK(hidden(root, "chamber_icon_glyph"));

    lv_subject_set_int(has_heater, 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(hidden(root, "home_chamber_cold_glyph"));
    CHECK(hidden(root, "chamber_icon_glyph"));
    CHECK_FALSE(hidden(root, "home_chamber_sensor_glyph"));
    binder.unbind();
}
