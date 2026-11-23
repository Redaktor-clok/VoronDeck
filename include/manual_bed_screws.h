#pragma once

#include "json_fwd.h"

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
    try {
        const auto& bed = status.at("bed_screws");
        if (status.at("webhooks").at("state") != "ready")
            return false;
        const bool active = bed.at("is_active").get<bool>();
        const auto print = status.at("print_stats").at("state").get<std::string>();
        if (print != "standby" && print != "complete" && print != "cancelled" && print != "error")
            return false;
        if (status.contains("manual_probe") &&
            status.at("manual_probe").at("is_active").get<bool>())
            return false;
        if (action == Action::Abort)
            return active;
        const auto homed = status.at("toolhead").at("homed_axes").get<std::string>();
        if (homed.find('x') == std::string::npos || homed.find('y') == std::string::npos ||
            homed.find('z') == std::string::npos)
            return false;
        const auto idle = status.at("idle_timeout").at("state").get<std::string>();
        if (idle != "Ready" && idle != "Idle")
            return false;
        const auto& config = status.at("configfile").at("settings").at("bed_screws");
        if (!config.is_object() || !config.contains("screw1") || !config.contains("screw2") ||
            !config.contains("screw3"))
            return false;
        if (action == Action::Start)
            return !active;
        return active;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}
} // namespace helix::manual_bed_screws
