// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_utils.h"

#include <string>

namespace helix::manual_bed_screws {
enum class Action { Start, Accept, Adjusted, Abort };

inline const char* command(Action action) {
    switch (action) {
    case Action::Start:
        return "BED_SCREWS_ADJUST";
    case Action::Accept:
        return "ACCEPT";
    case Action::Adjusted:
        return "ADJUSTED";
    case Action::Abort:
        return "ABORT";
    }
    return "";
}

// Shared ACCEPT/ABORT commands must never be sent to a different Klipper helper.
// Re-check a fresh status snapshot immediately before every command.
inline bool allowed(Action action, const nlohmann::json& status) {
    using namespace helix::json_util;
    const auto bed = safe_object(status, "bed_screws");
    const auto* active_value = find_member(bed, "is_active");
    if (safe_string(safe_object(status, "webhooks"), "state") != "ready" || !active_value ||
        !active_value->is_boolean())
        return false;
    const bool active = active_value->get<bool>();
    const auto print = safe_string(safe_object(status, "print_stats"), "state");
    if (print != "standby" && print != "complete" && print != "cancelled" && print != "error")
        return false;
    if (const auto* probe = find_member(status, "manual_probe")) {
        const auto* probing = find_member(*probe, "is_active");
        if (!probing || !probing->is_boolean() || probing->get<bool>())
            return false;
    }
    if (action == Action::Abort)
        return active;
    const auto homed = safe_string(safe_object(status, "toolhead"), "homed_axes");
    if (homed.find('x') == std::string::npos || homed.find('y') == std::string::npos ||
        homed.find('z') == std::string::npos)
        return false;
    const auto idle = safe_string(safe_object(status, "idle_timeout"), "state");
    if (idle != "Ready" && idle != "Idle")
        return false;
    const auto config =
        safe_object(safe_object(safe_object(status, "configfile"), "settings"), "bed_screws");
    if (!config.contains("screw1") || !config.contains("screw2") || !config.contains("screw3"))
        return false;
    return action == Action::Start ? !active : active;
}
} // namespace helix::manual_bed_screws
