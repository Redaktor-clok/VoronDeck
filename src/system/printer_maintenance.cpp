// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_maintenance.h"

namespace helix::maintenance {
namespace {
std::string string_field(const json& value, const char* key) {
    auto i = value.find(key);
    return i != value.end() && i->is_string() ? i->get<std::string>() : "";
}
bool flag(const json& value, const char* key, bool fallback = false) {
    auto i = value.find(key);
    return i != value.end() && i->is_boolean() ? i->get<bool>() : fallback;
}
}
std::vector<Component> parse_components(const json& result) {
    std::vector<Component> rows;
    if (!result.is_object()) return rows;
    auto versions = result.find("version_info");
    if (versions == result.end() || !versions->is_object()) return rows;
    for (const auto& [name, data] : versions->items()) {
        if (!data.is_object()) continue;
        Component row{name, string_field(data,"version"), string_field(data,"remote_version"), "", false};
        bool available = !row.available.empty() && row.current != row.available;
        if (auto commits=data.find("commits_behind"); commits!=data.end() && commits->is_array())
            available = available || !commits->empty();
        if (name == "system") {
            auto count=data.find("package_count");
            if (count!=data.end() && count->is_number_integer()) {
                auto n=count->get<int>();
                row.available=std::to_string(n);
                available=n>0;
            }
        }
        bool safe=flag(data,"is_valid",true) && !flag(data,"is_dirty") && !flag(data,"corrupt");
        if (name=="vorondeck_recovery") {
            available=flag(data,"update_available");
            safe=safe && flag(data,"vorondeck_recovery");
        }
        if ((name=="vorondeck" && !flag(data,"vorondeck_package")) || name=="helixscreen") {
            row.note="This source checkout does not update the installed VoronDeck package";
            safe=false;
        } else if (!safe) row.note="Component needs attention";
        else row.note=available ? "Update available" : "Up to date";
        if (name=="vorondeck_recovery" && flag(data,"restart_required"))
            row.note="Recovery updated. Restart Klipper to activate it.";
        row.can_update=safe && available;
        rows.push_back(std::move(row));
    }
    return rows;
}
Request reset_request(bool totals) {
    return totals ? Request{"server.history.reset_totals",json::object()}
                  : Request{"server.history.delete_job",{{"all",true}}};
}
Request update_request(const std::string& name) {
    // These endpoints also work on pre-1.5 Moonraker installations.
    if (name=="klipper" || name=="moonraker" || name=="system")
        return {"machine.update."+name,json::object()};
    return {"machine.update.client",{{"name",name}}};
}
}
