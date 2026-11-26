// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_temperature_source.h"

#include "http_request_epoch.h"
#include "printer_discovery.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#include "hv/json.hpp"

namespace helix::filament_temps {
namespace {

bool ends_with(const std::string& s, const char* suffix) {
    const size_t slen = std::strlen(suffix);
    return s.size() >= slen && s.compare(s.size() - slen, slen, suffix) == 0;
}

/// Read one leaf temperature: an integer the firmware reports, bounded like a
/// temperature. Anything else is absence, never a guess.
std::optional<int> scalar_temp(const nlohmann::json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    const int64_t v = value.get<int64_t>();
    // 0 is the table's unset placeholder: as a load/unload target it would
    // preheat the nozzle to 0C and shadow the DB rung below it.
    if (v <= 0 || v > 999) {
        return std::nullopt;
    }
    return static_cast<int>(v);
}

std::mutex g_table_mutex;
std::map<FilamentKey, FilamentTemperatures> g_table;
/// The REST epoch the table was read under: a lookup made after REST moved to
/// another printer must not answer with this one's temperatures.
uint64_t g_table_epoch = 0;

void store_table(const std::map<FilamentKey, FilamentTemperatures>& table, uint64_t epoch) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_table = table;
    g_table_epoch = epoch;
}

} // namespace

bool firmware_publishes_filament_temperatures(const PrinterDiscovery&) {
    return false;
}

std::string filament_temperature_root(const PrinterDiscovery&) {
    return {};
}

std::vector<std::string> filament_temperature_files(const PrinterDiscovery&) {
    return {};
}

std::map<FilamentKey, FilamentTemperatures>
parse_filament_temperatures(const std::string& json_text) {
    const nlohmann::json root = nlohmann::json::parse(json_text, nullptr, false);
    if (root.is_discarded()) {
        return {};
    }

    std::map<FilamentKey, FilamentTemperatures> table;
    if (!root.is_object()) {
        return table;
    }
    for (auto field = root.begin(); field != root.end(); ++field) {
        // Keys are {vendor}_{main_type}_{sub_type}_{field}; only load_temp
        // and unload_temp are temperatures. print_temp, flow_k*, vol_speed,
        // is_soft, version, the flow ceilings and the process_* keys are not
        // ours.
        const std::string& key = field.key();
        const bool unload = ends_with(key, "_unload_temp");
        if (!unload && !ends_with(key, "_load_temp")) {
            continue;
        }
        const std::string stem = key.substr(0, key.size() - (unload ? 12 : 10));
        // Split from the left: vendor, main_type, and the REST as sub_type,
        // so a sub_type holding a space or an underscore survives (main_type
        // itself never carries an underscore - the firmware spells it with
        // hyphens). A stem without three parts is skipped, not guessed.
        const size_t first_us = stem.find('_');
        if (first_us == std::string::npos) {
            continue;
        }
        const size_t second_us = stem.find('_', first_us + 1);
        if (second_us == std::string::npos) {
            continue;
        }
        const std::optional<int> temp = scalar_temp(field.value());
        if (!temp) {
            continue;
        }
        FilamentTemperatures& leaf = table[FilamentKey{
            helix::text_io::to_lower(stem.substr(0, first_us)),
            helix::text_io::to_lower(stem.substr(first_us + 1, second_us - first_us - 1)),
            helix::text_io::to_lower(stem.substr(second_us + 1))}];
        if (unload) {
            leaf.unload_c = temp;
        } else {
            leaf.load_c = temp;
        }
    }
    return table;
}

void merge_filament_temperatures(std::map<FilamentKey, FilamentTemperatures>& table,
                                 const std::map<FilamentKey, FilamentTemperatures>& line) {
    for (const auto& entry : line) {
        FilamentTemperatures& leaf = table[entry.first];
        if (!leaf.load_c) {
            leaf.load_c = entry.second.load_c;
        }
        if (!leaf.unload_c) {
            leaf.unload_c = entry.second.unload_c;
        }
    }
}

void store_filament_temperatures(const std::map<FilamentKey, FilamentTemperatures>& table) {
    store_table(table, helix::http_epoch::current());
}

void clear_filament_temperatures() {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_table.clear();
}

std::optional<FilamentTemperatures> lookup_filament_temperatures(const SlotInfo& slot) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    if (g_table_epoch != helix::http_epoch::current()) {
        return std::nullopt;
    }
    const std::string main = helix::text_io::to_lower(slot.material);
    const std::string vendor = helix::text_io::to_lower(slot.brand);
    const std::string sub = helix::text_io::to_lower(slot.spool_name);
    auto at = [&](const std::string& v,
                  const std::string& s) -> std::optional<FilamentTemperatures> {
        auto it = g_table.find(FilamentKey{v, main, s});
        if (it == g_table.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    if (auto t = at(vendor, sub)) {
        return t;
    }
    if (auto t = at(vendor, "generic")) {
        return t;
    }
    if (auto t = at("generic", sub)) {
        return t;
    }
    return at("generic", "generic");
}

void capture_on_connect(ITransfersAPI&, const PrinterDiscovery&) {
    clear_filament_temperatures();
}

} // namespace helix::filament_temps
