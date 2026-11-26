// SPDX-License-Identifier: GPL-3.0-or-later
#include "enclosure_top.h"

namespace helix::enclosure_top {

bool firmware_reports_enclosure_top(const PrinterDiscovery&) {
    return false;
}

nlohmann::json required_status_objects(const PrinterDiscovery&) {
    return nlohmann::json::object();
}

std::optional<bool> fitted_from_status(const PrinterDiscovery&, const nlohmann::json&) {
    return std::nullopt;
}

} // namespace helix::enclosure_top
