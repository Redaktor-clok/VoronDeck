// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "json_fwd.h"
#include <string>
#include <vector>

namespace helix::maintenance {
struct Component {
    std::string name, current, available, note;
    bool can_update = false;
};
std::vector<Component> parse_components(const json& result);
struct Request { std::string method; json params; };
Request reset_request(bool totals);
Request update_request(const std::string& name);
} // namespace helix::maintenance
