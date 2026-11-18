// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "home_screen_preferences.h"

#include <string>

namespace helix {

struct HomeSpoolAmount {
    double remaining_g = -1;
    double total_g = -1;
    double reported_metres = -1;
    double density_g_cm3 = 0;
    double diameter_mm = 0;
};

/// Formats only known data. Estimated length is marked with an approximation sign.
std::string home_spool_summary(const HomeSpoolAmount& amount,
                               HomeScreenPreferences::SpoolUnits units, bool show_total = true);

/// Remaining fraction 0-1 from known finite weights; -1 when unavailable.
double home_spool_fill_level(const HomeSpoolAmount& amount);

} // namespace helix
