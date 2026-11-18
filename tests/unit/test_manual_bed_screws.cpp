#include "manual_bed_screws.h"
#include "../catch_amalgamated.hpp"

using namespace helix::manual_bed_screws;

TEST_CASE("Manual screw commands require fresh matching helper state", "[manual-bed-screws]") {
    nlohmann::json status = {
        {"webhooks", {{"state", "ready"}}},
        {"bed_screws", {{"is_active", false}}},
        {"print_stats", {{"state", "standby"}}},
        {"toolhead", {{"homed_axes", "xyz"}}},
        {"idle_timeout", {{"state", "Ready"}}},
        {"configfile", {{"settings", {{"bed_screws", {
            {"screw1", "20,20"}, {"screw2", "100,20"}, {"screw3", "60,100"}}}}}}}};
    CHECK(allowed(Action::Start, status));
    CHECK_FALSE(allowed(Action::Accept, status));
    CHECK_FALSE(allowed(Action::Adjusted, status));
    CHECK_FALSE(allowed(Action::Abort, status));
    status["bed_screws"]["is_active"] = true;
    CHECK_FALSE(allowed(Action::Start, status));
    CHECK(allowed(Action::Accept, status));
    CHECK(allowed(Action::Adjusted, status));
    CHECK(allowed(Action::Abort, status));
    status["manual_probe"] = {{"is_active", true}};
    CHECK_FALSE(allowed(Action::Abort, status));
    CHECK_FALSE(allowed(Action::Accept, status));
    status["manual_probe"]["is_active"] = false;
    status["idle_timeout"]["state"] = "unknown";
    CHECK_FALSE(allowed(Action::Accept, status));
    status["idle_timeout"]["state"] = nullptr;
    CHECK_FALSE(allowed(Action::Adjusted, status));
    status["idle_timeout"]["state"] = "Ready";
    auto missing_config = status;
    missing_config.erase("configfile");
    missing_config["bed_screws"]["is_active"] = false;
    CHECK_FALSE(allowed(Action::Start, missing_config));
    status["toolhead"]["homed_axes"] = "xy";
    CHECK_FALSE(allowed(Action::Accept, status));
    CHECK(allowed(Action::Abort, status));
    status["toolhead"]["homed_axes"] = "xyz";
    status["idle_timeout"]["state"] = "Printing";
    CHECK_FALSE(allowed(Action::Adjusted, status));
    CHECK(allowed(Action::Abort, status));
    for (auto state : {"printing", "paused", "unknown"}) {
        status["print_stats"]["state"] = state;
        CHECK_FALSE(allowed(Action::Abort, status));
        CHECK_FALSE(allowed(Action::Accept, status));
    }
    CHECK_FALSE(allowed(Action::Start, nlohmann::json::object()));
    status.erase("bed_screws");
    CHECK_FALSE(allowed(Action::Abort, status));
    CHECK(std::string(command(Action::Start)) == "BED_SCREWS_ADJUST");
    CHECK(std::string(command(Action::Adjusted)) == "ADJUSTED");
}
