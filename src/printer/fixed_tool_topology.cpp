// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixed_tool_topology.h"

namespace helix::tool_topology {

std::vector<FixedTool> fixed_toolheads(const PrinterDiscovery&) {
    return {};
}

const char* provider_name(const PrinterDiscovery&) {
    return "none";
}

} // namespace helix::tool_topology
