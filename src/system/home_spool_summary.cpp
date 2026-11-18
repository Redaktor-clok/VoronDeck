// SPDX-License-Identifier: GPL-3.0-or-later
#include "home_spool_summary.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace helix {

double home_spool_fill_level(const HomeSpoolAmount& amount) {
    if (!std::isfinite(amount.remaining_g) || amount.remaining_g < 0 ||
        !std::isfinite(amount.total_g) || amount.total_g <= 0)
        return -1;
    return std::clamp(amount.remaining_g / amount.total_g, 0.0, 1.0);
}

std::string home_spool_summary(const HomeSpoolAmount& amount,
                               HomeScreenPreferences::SpoolUnits units, bool show_total) {
    const auto known = [](double value) { return std::isfinite(value) && value >= 0; };
    const bool has_weight = known(amount.remaining_g);
    const bool has_total = std::isfinite(amount.total_g) && amount.total_g > 0;
    std::string summary;
    const auto append = [&](const std::string& part) {
        if (!summary.empty())
            summary += " \xC2\xB7 ";
        summary += part;
    };
    const double fill = home_spool_fill_level(amount);
    if (units.percent && fill >= 0) {
        append(fmt::format("{:.0f}%", fill * 100));
    }
    if (units.grams && has_weight) {
        append(has_total && show_total ? fmt::format("{:.0f} / {:.0f} g", amount.remaining_g, amount.total_g)
                         : fmt::format("{:.0f} g", amount.remaining_g));
    }
    if (units.metres) {
        if (known(amount.reported_metres)) {
            append(fmt::format("{:.0f} m", amount.reported_metres));
        } else if (has_weight && std::isfinite(amount.density_g_cm3) && amount.density_g_cm3 > 0 &&
                   std::isfinite(amount.diameter_mm) && amount.diameter_mm > 0) {
            // g / (g/cm3) / mm2 equals metres: the cm3->mm3 and mm->m factors cancel.
            constexpr double pi = 3.14159265358979323846;
            const double radius = amount.diameter_mm / 2;
            const double metres =
                amount.remaining_g / amount.density_g_cm3 / (pi * radius * radius);
            if (std::isfinite(metres))
                append(fmt::format("~ {:.0f} m", metres));
        }
    }
    return summary;
}

} // namespace helix
