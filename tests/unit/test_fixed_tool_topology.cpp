// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Firmware that ships a fixed set of toolheads declares them in one table
// (helix::tool_topology); ToolState builds its tools from the answer and never
// names the vendor.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "fixed_tool_topology.h"
#include "printer_discovery.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;
namespace topology = helix::tool_topology;

namespace {

PrinterDiscovery discover(const json& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

const json kU1Objects = json::array({"extruder", "extruder1", "extruder2", "extruder3",
                                     "heater_bed", "filament_detect", "toolhead"});

} // namespace

TEST_CASE("fixed_toolheads never infers a proprietary topology", "[tool]") {
    for (const auto& objects :
         {kU1Objects, json::array({"extruder", "heater_bed"}),
          json::array({"toolchanger", "tool T0", "tool T1", "extruder", "extruder1"})}) {
        const auto hw = discover(objects);
        CHECK(topology::fixed_toolheads(hw).empty());
        CHECK(std::string(topology::provider_name(hw)) == "none");
    }
}
