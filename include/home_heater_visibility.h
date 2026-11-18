// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_heater_config.h"

#include "home_screen_preferences.h"
#include "printer_temperature_state.h"

namespace helix {

/// Resolve the same stable objects the temperature tile actually controls/reads.
inline bool home_heater_visible(const HomeScreenPreferences& preferences,
                                const PrinterTemperatureState& temperatures, HeaterType heater) {
    using Kind = HomeScreenPreferences::DeviceKind;
    switch (heater) {
    case HeaterType::Nozzle:
        return preferences.is_visible(Kind::Heater, temperatures.active_extruder_name());
    case HeaterType::Bed:
        return preferences.is_visible(Kind::Heater, "heater_bed");
    case HeaterType::Chamber:
        if (!temperatures.chamber_heater_name().empty())
            return preferences.is_visible(Kind::Heater, temperatures.chamber_heater_name());
        return preferences.is_visible(Kind::PassiveSensor,
                                      temperatures.chamber_temperature_source());
    }
    return true;
}

} // namespace helix
