// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolchanger_addon.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;
using namespace helix;

TEST_CASE("AFC lane spools are read from their AFC_lane objects", "[toolchanger][afc]") {
    const auto spools =
        toolchanger_addon::read_afc_lane_spools(json{{"AFC_lane E0", {{"spool_id", 19}}},
                                                     {"AFC_lane E3", {{"spool_id", nullptr}}},
                                                     {"AFC_lane E1", {{"map", "T1"}}}});
    REQUIRE(spools.has_value());
    REQUIRE(spools->size() == 2);
    for (const auto& spool : *spools) {
        if (spool.lane == "E0") {
            CHECK(spool.spool_id == 19);
        } else {
            CHECK(spool.lane == "E3");
            CHECK(spool.spool_id == 0); // null: the lane has no spool
        }
    }
    // A frame naming no lane's spool_id is not a clearing.
    CHECK_FALSE(toolchanger_addon::read_afc_lane_spools(json{{"AFC_lane E1", {{"map", "T1"}}}})
                    .has_value());
}
