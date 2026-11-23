// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <optional>
#include <string>

#include "hv/json.hpp"

namespace helix {
/// The config object and Klipper mux name are distinct for heater_generic.
struct PidHeaterRequest {
    std::string object;
    std::string command_name;
    double min_temp;
    double max_temp;

    bool accepts(double target) const {
        return std::isfinite(target) && target > 0 && target >= min_temp && target <= max_temp;
    }
};

/// Fail closed on absent control/limits. No MPC/watermark migration is implied.
inline std::optional<PidHeaterRequest> pid_heater_request(const nlohmann::json& config,
                                                          const std::string& object) {
    std::string command = object;
    if (object.rfind("heater_generic ", 0) == 0)
        command = object.substr(15);
    else if (object != "heater_bed" &&
             !(object.rfind("extruder", 0) == 0 &&
               object.find_first_not_of("0123456789", 8) == std::string::npos))
        return std::nullopt;
    if (command.empty() ||
        command.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos)
        return std::nullopt;
    const auto section = config.find(object);
    if (section == config.end() || !section->is_object() ||
        section->find("control") == section->end() || (*section)["control"] != "pid")
        return std::nullopt;
    auto number = [&](const char* key) -> std::optional<double> {
        const auto value = section->find(key);
        if (value == section->end())
            return std::nullopt;
        double parsed;
        try {
            if (value->is_number())
                parsed = value->get<double>();
            else if (value->is_string()) {
                size_t used = 0;
                const auto text = value->get<std::string>();
                parsed = std::stod(text, &used);
                if (used != text.size())
                    return std::nullopt;
            } else
                return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
        return std::isfinite(parsed) ? std::optional<double>(parsed) : std::nullopt;
    };
    const auto low = number("min_temp"), high = number("max_temp");
    if (!low || !high || *high <= *low || *high <= 0)
        return std::nullopt;
    return PidHeaterRequest{object, command, *low, *high};
}
} // namespace helix
