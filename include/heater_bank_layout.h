// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "grid_layout.h"

#include <algorithm>
#include <string>
namespace helix {
struct HeaterBankShape {
    int columns;
    int rows;
};
inline HeaterBankShape heater_bank_shape(size_t count, bool portrait) {
    const int n = std::max(1, static_cast<int>(count));
    return portrait ? HeaterBankShape{(n + 2) / 3, std::min(3, n)}
                    : HeaterBankShape{std::min(3, n), (n + 2) / 3};
}
inline bool controllable_heater_id(const std::string& name) {
    if (name == "heater_bed" || name == "extruder")
        return true;
    if (name.rfind("heater_generic ", 0) == 0)
        return name.size() > 15;
    return name.rfind("extruder", 0) == 0 && name.size() > 8 &&
           name.find_first_not_of("0123456789", 8) == std::string::npos;
}
// Transactional native-grid placement: never moves another tile or mutates on failure.
inline bool fit_heater_bank(GridLayout& grid, GridPlacement& tile, size_t count, bool portrait) {
    auto shape = heater_bank_shape(count, portrait);
    GridLayout candidate = grid;
    candidate.remove(tile.widget_id);
    auto next = tile;
    next.colspan = shape.columns * 3;
    next.rowspan = shape.rows * GridLayout::TRACKS_PER_CELL + 1;
    if (!candidate.can_place(next.col, next.row, next.colspan, next.rowspan)) {
        auto position = candidate.find_available(next.colspan, next.rowspan, 1, 1);
        if (!position)
            return false;
        next.col = position->first;
        next.row = position->second;
    }
    if (!candidate.place(next))
        return false;
    grid = std::move(candidate);
    tile = next;
    return true;
}
} // namespace helix
