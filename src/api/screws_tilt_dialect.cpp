// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screws_tilt_dialect.h"

#include "printer_discovery.h"

namespace helix::screws_tilt {

void reconcile_on_connect(IMoonrakerClient&, const PrinterDiscovery&, const nlohmann::json&) {}

void request_exit(IMoonrakerClient&, const PrinterDiscovery&) {}

bool start_dialect_sequence(IMoonrakerClient&, IMoonrakerAPI&, const PrinterDiscovery&,
                            ScrewTiltCallback, helix::SharedCallback<void(const MoonrakerError&)>) {
    return false;
}

const std::vector<const char*>& config_section_names() {
    static const std::vector<const char*> names{"screws_tilt_adjust"};
    return names;
}

} // namespace helix::screws_tilt
