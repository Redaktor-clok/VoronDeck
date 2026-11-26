// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "printer_motion_state.h"
#include "z_offset_utils.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterMotionState;
using helix::zoffset::displayed_z_offset_microns;
using nlohmann::json;

namespace {

class PersistedZOffsetFixture : public HelixTestFixture {
  public:
    PersistedZOffsetFixture() {
        state.init_subjects(false); // no XML registration in test
    }

    PrinterMotionState state;

    int persisted() {
        return lv_subject_get_int(state.get_persisted_z_offset_subject());
    }
    bool valid() {
        return lv_subject_get_int(state.get_persisted_z_offset_valid_subject()) != 0;
    }
    int live() {
        return lv_subject_get_int(state.get_gcode_z_offset_subject());
    }
    int displayed(bool print_active) {
        std::optional<int> p;
        if (valid()) {
            p = persisted();
        }
        return displayed_z_offset_microns(live(), p, print_active);
    }

    static json homing_origin(double z) {
        return json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, z, 0.0}}}}};
    }

    static json save_variables(const json& variables) {
        return json{{"save_variables", json{{"variables", variables}}}};
    }
    static json gcode_offsets(const json& offsets) {
        return save_variables(json{{"zoffset", offsets}});
    }
};

} // namespace

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset starts invalid",
                 "[motion][zoffset][persistence]") {
    REQUIRE_FALSE(valid());
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset parses zoffset.z",
                 "[motion][zoffset][persistence]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));

    REQUIRE(valid());
    REQUIRE(persisted() == -150);
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset survives frames that omit it",
                 "[motion][zoffset][persistence]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    REQUIRE(valid());

    state.update_from_status(json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, 0.0, 0.0}}}}});

    REQUIRE(valid());
    REQUIRE(persisted() == -150);
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset tracks later changes",
                 "[motion][zoffset][persistence]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(gcode_offsets(json{{"z", -0.075}}));

    REQUIRE(valid());
    REQUIRE(persisted() == -75);
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset accepts a stored zero",
                 "[motion][zoffset][persistence]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(gcode_offsets(json{{"z", 0.0}}));

    REQUIRE(valid());
    REQUIRE(persisted() == 0);
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Persisted z-offset stays invalid for a printer without gcode_offsets",
                 "[motion][zoffset][persistence]") {
    state.update_from_status(save_variables(json{{"bambufy_colors", json::array()}}));

    REQUIRE_FALSE(valid());
}

TEST_CASE_METHOD(PersistedZOffsetFixture, "Persisted z-offset ignores a null placeholder",
                 "[motion][zoffset][persistence]") {
    state.update_from_status(gcode_offsets(json{{"z", nullptr}}));

    REQUIRE_FALSE(valid());
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Persisted z-offset does not disturb the live gcode offset",
                 "[motion][zoffset][persistence]") {
    state.update_from_status(json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, -0.2, 0.0}}}}});
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));

    REQUIRE(lv_subject_get_int(state.get_gcode_z_offset_subject()) == -200);
    REQUIRE(persisted() == -150);
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Regression: persistent-offset z-offset stays truthful across a whole print cycle",
                 "[motion][zoffset][persistence][regression]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(homing_origin(0.0));

    REQUIRE(live() == 0); // what the old code displayed: a phantom 0.000
    CHECK(displayed(/*print_active=*/false) == -150);

    state.update_from_status(homing_origin(-0.15));

    CHECK(displayed(/*print_active=*/true) == -150);

    state.update_from_status(homing_origin(-0.16));
    CHECK(displayed(/*print_active=*/true) == -160);
    state.update_from_status(gcode_offsets(json{{"z", -0.16}}));
    CHECK(displayed(/*print_active=*/true) == -160);

    state.update_from_status(homing_origin(0.0));

    REQUIRE(live() == 0);
    CHECK(displayed(/*print_active=*/false) == -160); // NOT 0.000
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Regression: a non-persistent-offset printer is unaffected in every print state",
                 "[motion][zoffset][regression]") {
    state.update_from_status(homing_origin(-0.2));

    REQUIRE_FALSE(valid());
    CHECK(displayed(/*print_active=*/false) == -200);
    CHECK(displayed(/*print_active=*/true) == -200);

    state.update_from_status(homing_origin(0.0));
    CHECK(displayed(/*print_active=*/false) == 0);
}

TEST_CASE_METHOD(PersistedZOffsetFixture,
                 "Regression: idle baby-step adjusts from the stored offset, not zero",
                 "[motion][zoffset][persistence][regression]") {
    state.update_from_status(gcode_offsets(json{{"z", -0.15}}));
    state.update_from_status(homing_origin(0.0));

    const int base = displayed(/*print_active=*/false);
    REQUIRE(base == -150);

    CHECK(helix::zoffset::build_z_adjust_gcode(base, live(), -10, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z=-0.160 MOVE=1");

    state.update_from_status(homing_origin(-0.15));
    const int print_base = displayed(/*print_active=*/true);
    REQUIRE(print_base == live());
    CHECK(helix::zoffset::build_z_adjust_gcode(print_base, live(), -10, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}
