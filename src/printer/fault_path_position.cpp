// SPDX-License-Identifier: GPL-3.0-or-later

#include "fault_path_position.h"

#include "afc_fault_position.h"

#include <cctype>

namespace helix::faultpath {

namespace {

struct Provider {
    std::optional<PathSegment> (*position)(std::string_view message);
    std::string (*strip)(std::string_view message);
};

const Provider kProviders[] = {
    {&afc::afc_fault_position, &afc::afc_strip_position_diagram},
};

const Provider* recognising_provider(std::string_view message) {
    for (const Provider& p : kProviders) {
        if (p.position(message)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

std::optional<PathSegment> fault_position(std::string_view message) {
    if (is_prep_runout(message))
        return PathSegment::PREP;
    const Provider* p = recognising_provider(message);
    return p ? p->position(message) : std::nullopt;
}

bool is_prep_runout(std::string_view message) {
    // Match the fault sentence, never a filename or a later diagnostic paragraph.
    message = message.substr(0, message.find('\n'));
    while (!message.empty() && std::isspace(static_cast<unsigned char>(message.front())))
        message.remove_prefix(1);
    if (message.substr(0, 3) == "!! ")
        message.remove_prefix(3);
    std::string line(message);
    for (char& c : line)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (line.rfind("runout triggered for lane ", 0) == 0)
        return true;
    if (line.rfind("runout detected on ", 0) != 0)
        return false;
    for (const std::string_view sensor : {"mmu_entry", "mmu_pre_gate"}) {
        const size_t at = line.find(sensor);
        if (at == std::string::npos || at == 0 || line[at - 1] != ' ')
            continue;
        size_t end = at + sensor.size();
        if (end < line.size() && line[end] == '_') {
            const size_t first_digit = ++end;
            while (end < line.size() && line[end] >= '0' && line[end] <= '9')
                ++end;
            if (end == first_digit)
                continue;
        }
        if (end == line.size() || std::isspace(static_cast<unsigned char>(line[end])))
            return true;
    }
    return false;
}

std::string strip_position_diagram(std::string_view message) {
    const Provider* p = recognising_provider(message);
    return p ? p->strip(message) : std::string(message);
}

} // namespace helix::faultpath
