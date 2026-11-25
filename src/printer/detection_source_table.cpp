// SPDX-License-Identifier: GPL-3.0-or-later
#include "detection_source_table.h"

namespace helix::detection {

const std::vector<SourceTableEntry>& builtin_sources() {
    static const std::vector<SourceTableEntry> sources;
    return sources;
}

void register_builtin_sources(DetectionManager& dm, helix::PrinterState* state) {
    for (const auto& entry : builtin_sources()) {
        auto src = entry.build(state);
        if (!src) {
            continue;
        }
        const std::string id = src->id();
        dm.register_source(std::move(src));
        if (const auto p = entry.stored_policy()) {
            dm.set_policy(id, *p);
        }
    }
}

} // namespace helix::detection
