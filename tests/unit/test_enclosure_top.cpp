// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "enclosure_top.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "test_helpers/scoped_env.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
using namespace helix::enclosure_top;

namespace {

PrinterDiscovery hardware_with_objects(const std::vector<std::string>& names) {
    PrinterDiscovery hw;
    json objects = json::array();
    for (const auto& n : names) {
        objects.push_back(n);
    }
    hw.parse_objects(objects);
    return hw;
}

/// The object set a real U1 capture lists around its purifier.
PrinterDiscovery u1_hardware() {
    return hardware_with_objects({"purifier", "fm175xx_reader", "tool", "extruder"});
}

} // namespace

TEST_CASE("enclosure top: retired markers never select a provider", "[enclosure_top]") {
    for (const auto& hw : {u1_hardware(), hardware_with_objects({"purifier", "extruder"}),
                           hardware_with_objects({"extruder", "heater_bed"})}) {
        CHECK_FALSE(firmware_reports_enclosure_top(hw));
        CHECK(required_status_objects(hw).empty());
        CHECK_FALSE(
            fitted_from_status(hw, json{{"purifier", {{"power_detected", true}}}}).has_value());
        CHECK_FALSE(
            fitted_from_status(hw, json{{"purifier", {{"power_detected", false}}}}).has_value());
    }
}
