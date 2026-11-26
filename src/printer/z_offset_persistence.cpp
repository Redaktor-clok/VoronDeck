// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_persistence.h"

#include "config.h"
#include "printer_discovery.h"

#include <cmath>
#include <string>

namespace helix::zoffset {
namespace {

/// One firmware that stores the z-offset outside gcode_move.
struct Provider {
    const char* name;
    /// Macro whose presence identifies the firmware.
    const char* detect_macro;
    /// Status objects that carry the stored value.
    std::vector<std::string> status_objects;
    /// One-shot command that turns persistence on, or nullptr.
    const char* enable_gcode;
    /// Command clearing a stale probe delta the firmware subtracts before
    /// persisting an adjustment, or nullptr when it has no such variable.
    const char* stale_delta_clear_gcode;
    /// Pull the stored offset, in microns, out of a status frame.
    std::optional<int> (*read)(const nlohmann::json& status);
    /// Read whether enable_gcode's setting is already on, or nullptr where the
    /// firmware exposes no such flag. Must answer nullopt when the frame does
    /// not carry the key: absence is silence, not an off.
    std::optional<bool> (*read_enabled)(const nlohmann::json& status);
    /// Predicate a status frame must satisfy to PROVE this row's storage is
    /// absent - i.e. that the detect macro matched something other than this
    /// firmware. nullptr where the detect macro is unambiguous and the match
    /// therefore cannot be wrong. Must answer false for "no news": a frame
    /// that simply does not carry the store is not evidence of absence.
    bool (*refuted_by)(const nlohmann::json& status);
};

/// Fetch status.<object>.<member> as an object, or nullptr.
const nlohmann::json* nested_object(const nlohmann::json& status, const char* object,
                                    const char* member) {
    if (!status.is_object()) {
        return nullptr;
    }
    auto outer = status.find(object);
    if (outer == status.end() || !outer->is_object()) {
        return nullptr;
    }
    auto inner = outer->find(member);
    if (inner == outer->end() || !inner->is_object()) {
        return nullptr;
    }
    return &(*inner);
}

// Generic SET_GCODE_OFFSET wrappers can persist the live offset in
// save_variables.zoffset and restore it at startup. Avoid folding that same
// value into the probe configuration a second time.
std::optional<int> read_helper_script(const nlohmann::json& status) {
    const nlohmann::json* variables = nested_object(status, "save_variables", "variables");
    if (!variables) {
        return std::nullopt;
    }
    auto zoffset = variables->find("zoffset");
    if (zoffset == variables->end() || !zoffset->is_object()) {
        return std::nullopt;
    }
    auto z = zoffset->find("z");
    if (z == zoffset->end() || !z->is_number()) {
        // Seeded as {'z': None} before the first wrapped SET_GCODE_OFFSET.
        return std::nullopt;
    }
    return static_cast<int>(std::lround(z->get<double>() * 1000.0));
}

// The detect macro for this row is the WRAPPER, not a vendor-unique name, so
// the match proves only that SOMETHING shadows SET_GCODE_OFFSET. Wrapping it to
// log, clamp, or fan out per-tool offsets is a standard Voron / Klippain /
// toolchanger pattern, and such a wrapper stores nothing. This is the frame
// that tells the two apart: save-zoffset.cfg SAVE_VARIABLEs `zoffset` on every
// wrapped call and its boot delayed_gcode reads it straight back, so a
// save_variables store delivered WITH its variables member and carrying no
// `zoffset` key at all is positive evidence the module is not installed.
//
// The distinction that keeps this on the safe side of #1401: `zoffset` present
// but seeded as {'z': None} is the module installed and not yet used, which is
// emphatically NOT a refutation. Only total absence of the key refutes.
bool refuted_helper_script(const nlohmann::json& status) {
    const nlohmann::json* variables = nested_object(status, "save_variables", "variables");
    if (!variables) {
        // No news. save_variables is delta-only, and it can arrive without its
        // variables member; neither shape proves anything either way.
        return false;
    }
    return variables->find("zoffset") == variables->end();
}

const std::vector<Provider>& providers() {
    // Row order is match priority: a box exposing two firmwares' macros
    // resolves to the first hit.
    static const std::vector<Provider> table = {
        // Keyed on the WRAPPER object. Klipper exposes a builtin command as a
        // printer object ONLY when a [gcode_macro] shadows it (and shadowing a
        // builtin requires rename_existing), so `gcode_macro
        // SET_GCODE_OFFSET` in objects/list is exactly "a renaming wrapper
        // exists" - stock Klipper never lists it. The renamed original the
        // wrapper creates is a bare command, not an object; verified against
        // printer object lists: the wrapper is listed, the renamed command is not.
        //
        // That signature proves a wrapper exists, NOT that it persists
        // anything, so this is the one row that carries a refutation - see
        // refuted_helper_script().
        {"Helper-Script",
         "SET_GCODE_OFFSET",
         {"save_variables"},
         nullptr,
         nullptr,
         &read_helper_script,
         // The module's boot delayed_gcode re-applies the stored offset
         // unconditionally; there is no setting to enable and none to read.
         nullptr,
         &refuted_helper_script},
    };
    return table;
}

const Provider* match(const PrinterDiscovery& hw) {
    for (const auto& p : providers()) {
        if (hw.has_macro(p.detect_macro)) {
            return &p;
        }
    }
    return nullptr;
}

/// Where the "already sent" record lives. Scoped to the active printer: the
/// setting belongs to that printer's firmware, so a second printer on the same
/// install still gets its own one send. Named here once so the read and the
/// write cannot drift apart.
std::string enable_sent_key(const Config& config) {
    return config.df() + "zoffset_persistence_enable_sent";
}

} // namespace

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    if (const Provider* p = match(hw)) {
        return p->status_objects;
    }
    return {};
}

std::optional<int> read_persisted_offset_microns(const nlohmann::json& status) {
    // Fast path for the overwhelming majority: the subscription builder only
    // subscribes save_variables / mod_params when a provider matched, so a
    // frame carrying NEITHER object belongs to a printer with no persistence
    // firmware and the per-frame schema probes below are pure waste. (A frame
    // that DOES carry one of them for unrelated reasons simply falls through
    // to the probes and misses — the gate is a skip, never an answer.)
    if (!status.is_object()) {
        return std::nullopt;
    }
    const bool carries_schema_object =
        status.contains("save_variables") || status.contains("mod_params");
    if (!carries_schema_object) {
        return std::nullopt;
    }
    // Read by schema rather than by detected firmware: this runs on the status
    // path, which has no PrinterDiscovery to hand, and the schemas are distinct
    // enough to identify themselves. A printer without the firmware never
    // carries the keys, so it simply never matches.
    for (const auto& p : providers()) {
        if (auto microns = p.read(status)) {
            return microns;
        }
    }
    return std::nullopt;
}

std::string persistence_enable_gcode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (p && p->enable_gcode) {
        return p->enable_gcode;
    }
    return {};
}

std::optional<bool> persistence_already_enabled(const PrinterDiscovery& hw,
                                                const nlohmann::json& status) {
    const Provider* p = match(hw);
    if (!p || !p->read_enabled) {
        return std::nullopt;
    }
    return p->read_enabled(status);
}

std::string stale_probe_delta_clear_gcode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (p && p->stale_delta_clear_gcode) {
        return p->stale_delta_clear_gcode;
    }
    return {};
}

bool firmware_persists_z_offset(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

bool status_refutes_persistence(const PrinterDiscovery& hw, const nlohmann::json& status) {
    const Provider* p = match(hw);
    // No provider: nothing latched, nothing to refute. A provider whose detect
    // macro is unambiguous carries no refutation and can never be talked out of
    // the match by a status frame.
    if (!p || !p->refuted_by) {
        return false;
    }
    return p->refuted_by(status);
}

bool claim_persistence_enable(Config* config, const PrinterDiscovery& hw,
                              const nlohmann::json* status, bool print_active) {
    if (!config) {
        return false;
    }
    const std::string key = enable_sent_key(*config);
    const std::optional<bool> already_enabled =
        status ? persistence_already_enabled(hw, *status) : std::nullopt;
    if (!should_enable_persistence(!persistence_enable_gcode(hw).empty(), print_active,
                                   config->get<bool>(key, false), already_enabled)) {
        // Nothing recorded on a no: a print in progress or an already-enabled
        // firmware must leave the one shot unspent for a later discovery.
        return false;
    }
    config->set<bool>(key, true);
    // set() writes the in-memory document only. Without the save the record is
    // gone at the next launch and the command is re-sent for the life of the
    // install.
    config->save();
    return true;
}

void release_persistence_enable(Config* config) {
    if (!config) {
        return;
    }
    const std::string key = enable_sent_key(*config);
    if (!config->get<bool>(key, false)) {
        return; // never claimed, or already given back
    }
    config->set<bool>(key, false);
    config->save();
    spdlog::info("[ZOffset] Persistence enable did not land; the one shot is available again");
}

std::string persistence_provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string{};
}

} // namespace helix::zoffset
