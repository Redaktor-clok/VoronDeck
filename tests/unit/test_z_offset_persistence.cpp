// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_persistence.cpp
 * @brief Tests for the firmware-persisted z-offset abstraction.
 *
 * Some firmware keeps the authoritative z-offset outside Klipper's gcode_move
 * and zeroes the live offset outside a print. helix::zoffset owns which
 * firmwares those are, what to subscribe, how to read the value, and how to turn
 * persistence on. Generic code asks these questions and never names a firmware,
 * so these tests are also the guard on that boundary: they exercise the
 * capability API, and reach a vendor only through a printer's macro list.
 */

#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "printer_discovery.h"
#include "z_offset_persistence.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Config;
using helix::PrinterDiscovery;
using nlohmann::json;

namespace {

/// A printer whose objects list carries the given gcode macros.
PrinterDiscovery printer_with_macros(std::initializer_list<const char*> macros) {
    json objects = json::array();
    objects.push_back("gcode_move");
    objects.push_back("toolhead");
    for (const char* m : macros) {
        objects.push_back(std::string("gcode_macro ") + m);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

// TEST_MIRROR_OK: contains() is a one-line assertion utility (vector membership), not logic
// under test
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

/// A ZMOD-shaped save_variables frame.

/// A Forge-X-shaped mod_params frame.

/// A ZMOD save_variables frame carrying the persistence flag.

/// A Forge-X mod_params frame carrying the persistence flag.

/// Spell a tri-state answer so a case fits on one line and a failure names the
/// state it got instead of printing an opaque optional.
std::string tri(const std::optional<bool>& v) {
    if (!v.has_value()) {
        return "unknown";
    }
    return *v ? "on" : "off";
}

/// The Config singleton, pointed at a named printer so df() routes the record
/// to a stable per-printer key. The suite's isolation listener has already
/// pointed the singleton at the sandbox and blanked its document, so every test
/// starts from a fresh install with no settings.json on disk.
Config* fresh_config(const char* printer_id = "zoffset_test_printer") {
    Config* cfg = Config::get_instance();
    helix::ConfigTestAccess::active_printer_id(*cfg) = printer_id;
    return cfg;
}

/// Replace the live document with whatever save() actually wrote to disk. A
/// record that only reached memory does not survive this.
void reload_from_disk(Config* cfg) {
    const std::string path = cfg->get_path();
    INFO("reading back " << path);
    std::ifstream in(path);
    REQUIRE(in.good());
    json saved = json::parse(in, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(saved.is_discarded());
    helix::ConfigTestAccess::data(*cfg) = saved;
}

} // namespace

// ============================================================================
// Detection and the capability questions
// ============================================================================

TEST_CASE("z-offset persistence: a plain printer needs nothing", "[zoffset][persistence]") {
    // The overwhelming majority: the live gcode_move offset IS the truth, so no
    // extra subscription, no enable command, and the row must not switch source.
    PrinterDiscovery hw = printer_with_macros({"START_PRINT", "END_PRINT"});

    CHECK_FALSE(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(helix::zoffset::required_status_objects(hw).empty());
    CHECK(helix::zoffset::persistence_enable_gcode(hw).empty());
    CHECK(helix::zoffset::persistence_provider_name(hw).empty());
}

// ============================================================================
// Clearing the stale probe delta before an idle adjustment
// ============================================================================

// ============================================================================
// Reading the stored value
// ============================================================================

TEST_CASE("z-offset persistence: a frame without the schema objects reads nothing",
          "[zoffset][persistence]") {
    // The subscription builder only subscribes save_variables / mod_params
    // when a provider matched, so this is the frame shape of the overwhelming
    // majority of printers — the fast path must answer nullopt, not probe.
    json frame = json{{"gcode_move", json{{"z_offset", -0.15}}}, {"toolhead", json{{"x", 1.0}}}};
    CHECK_FALSE(helix::zoffset::read_persisted_offset_microns(frame).has_value());
}

// ============================================================================
// The enable gate
// ============================================================================

TEST_CASE("z-offset persistence: enable gate requires a firmware that needs it",
          "[zoffset][persistence]") {
    using helix::zoffset::should_enable_persistence;

    CHECK_FALSE(should_enable_persistence(false, false, false, std::nullopt));
    CHECK_FALSE(should_enable_persistence(false, true, false, std::nullopt));
    CHECK_FALSE(should_enable_persistence(false, false, true, std::nullopt));
    CHECK_FALSE(should_enable_persistence(false, false, false, false));
}

TEST_CASE("z-offset persistence: enable gate fires once, while idle", "[zoffset][persistence]") {
    using helix::zoffset::should_enable_persistence;

    CHECK(should_enable_persistence(true, false, false, std::nullopt));
    // Never mid-print: this injects gcode into a running job.
    CHECK_FALSE(should_enable_persistence(true, true, false, std::nullopt));
    // Never again once this install has sent it. The flag is persisted, so a
    // user who turns the setting back off is not overruled at the next launch
    // (prestonbrown/helixscreen#1432).
    CHECK_FALSE(should_enable_persistence(true, false, true, std::nullopt));
}

TEST_CASE("z-offset persistence: enable gate stands down when the firmware already agrees",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::should_enable_persistence;

    // The write is persistent firmware state and costs the user the slicer's
    // per-print Z_OFFSET / SKIP_ZOFFSET parameters, so a firmware that already
    // holds the setting must be left alone.
    CHECK_FALSE(should_enable_persistence(true, false, false, true));
    // Off is the state the send exists for.
    CHECK(should_enable_persistence(true, false, false, false));
    // Unknown is not an off. Only a definite true suppresses.
    CHECK(should_enable_persistence(true, false, false, std::nullopt));

    // The other gates still bind regardless of what the firmware reports.
    CHECK_FALSE(should_enable_persistence(true, true, false, false));
    CHECK_FALSE(should_enable_persistence(true, false, true, false));
}

// ============================================================================
// Reading the firmware's current enable state (prestonbrown/helixscreen#1432)
// ============================================================================

// ============================================================================
// Helper-Script save-zoffset (prestonbrown/helixscreen#1401)
// ============================================================================

TEST_CASE("z-offset persistence: Helper-Script's wrapper is detected by the wrapper object",
          "[zoffset][persistence][1401]") {
    // save-zoffset.cfg defines [gcode_macro SET_GCODE_OFFSET] with
    // rename_existing, and shadowing a builtin REQUIRES rename_existing - so
    // the wrapper object existing in objects/list is exactly "a renaming
    // wrapper is installed". The renamed original is a bare command, never an
    // object (verified against debug bundle 5J49T5RU: SET_GCODE_OFFSET in the
    // 83 macro objects, _SET_GCODE_OFFSET in none).
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});

    CHECK(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(helix::zoffset::persistence_provider_name(hw) == "Helper-Script");
    CHECK(contains(helix::zoffset::required_status_objects(hw), "save_variables"));
    // The module loads at boot via its own delayed_gcode; there is nothing to
    // enable and no stale probe-delta variable to clear.
    CHECK(helix::zoffset::persistence_enable_gcode(hw).empty());
    CHECK(helix::zoffset::stale_probe_delta_clear_gcode(hw).empty());
}

TEST_CASE("z-offset persistence: the Helper-Script row reads the zoffset save-variable",
          "[zoffset][persistence][1401]") {
    // SAVE_VARIABLE VARIABLE=zoffset VALUE="{'z': -0.475}" lands in Moonraker's
    // status as save_variables.variables.zoffset = {"z": -0.475}.
    json frame =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", -0.475}}}}}}}};
    auto microns = helix::zoffset::read_persisted_offset_microns(frame);
    REQUIRE(microns.has_value());
    CHECK(*microns == -475);

    // Seeded as {'z': None} before the first wrapped SET_GCODE_OFFSET.
    json null_z =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", nullptr}}}}}}}};
    CHECK_FALSE(helix::zoffset::read_persisted_offset_microns(null_z).has_value());

    // A save_variables store without the module's variable is some other
    // consumer's data - not ours to read.
    json other = json{{"save_variables", json{{"variables", json{{"lan_clients", 7}}}}}};
    CHECK_FALSE(helix::zoffset::read_persisted_offset_microns(other).has_value());
}

// ============================================================================
// Refuting an over-matched provider (prestonbrown/helixscreen#1401)
// ============================================================================
//
// The Helper-Script row detects on the WRAPPER object, which proves something
// shadows SET_GCODE_OFFSET but not that the shadow stores anything - wrapping
// the command to log, clamp, or fan out per-tool offsets is a standard Voron /
// Klippain / toolchanger pattern. The match still latches immediately, because
// the two mistakes are not symmetric: over-matching costs a benign-wrapper user
// the Save Z Offset button, while under-matching folds a real Helper-Script
// offset into the probe and stacks it every boot (0.060 -> 2.515mm over five
// cycles, nozzle-on-bed). Only a frame that PROVES the store is absent relaxes
// it.

TEST_CASE("z-offset persistence: a wrapper with no zoffset store is refuted",
          "[zoffset][persistence][1401]") {
    // The over-match case. save_variables arrived, complete, carrying somebody
    // else's variables and no `zoffset` key at all - save-zoffset.cfg is not
    // installed, so the wrapper is one of the benign kinds and Save Z Offset
    // must come back.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});
    REQUIRE(helix::zoffset::firmware_persists_z_offset(hw));

    json frame = json{{"save_variables", json{{"variables", json{{"lan_clients", 7}}}}}};
    CHECK(helix::zoffset::status_refutes_persistence(hw, frame));

    // An empty store is the same proof: the module seeds its variable, so a
    // delivered-and-empty variables dict cannot be an install.
    json empty = json{{"save_variables", json{{"variables", json::object()}}}};
    CHECK(helix::zoffset::status_refutes_persistence(hw, empty));
}

TEST_CASE("z-offset persistence: a real Helper-Script install is never refuted",
          "[zoffset][persistence][1401]") {
    // The dangerous direction. The store carries the module's variable with a
    // real value: this printer persists, and Save Z Offset must stay down.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});

    json frame =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", -0.475}}}}}}}};
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(hw, frame));

    // Sibling keys in the same store change nothing.
    json with_siblings =
        json{{"save_variables",
              json{{"variables", json{{"lan_clients", 7}, {"zoffset", json{{"z", -0.475}}}}}}}};
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(hw, with_siblings));
}

TEST_CASE("z-offset persistence: the seeded zoffset placeholder does not refute",
          "[zoffset][persistence][1401]") {
    // THE boundary that protects against the damaging direction. save-zoffset.cfg
    // seeds `zoffset` as {'z': None} at install and only fills it on the first
    // wrapped SET_GCODE_OFFSET. The read path correctly calls that "nothing
    // stored yet" - but the module IS installed and its boot gcode WILL re-apply
    // whatever lands there, so treating the placeholder as a refutation would
    // hand the user a Save button on a printer that stacks the offset.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});

    json seeded =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", nullptr}}}}}}}};
    REQUIRE_FALSE(helix::zoffset::read_persisted_offset_microns(seeded).has_value());
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(hw, seeded));

    // Any shape of the key counts as present: what refutes is the key's total
    // absence, never our failure to parse its value.
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(
        hw, json{{"save_variables", json{{"variables", json{{"zoffset", json::object()}}}}}}));
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(
        hw, json{{"save_variables", json{{"variables", json{{"zoffset", nullptr}}}}}}));
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(
        hw, json{{"save_variables", json{{"variables", json{{"zoffset", "-0.475"}}}}}}));
}

TEST_CASE("z-offset persistence: no news is not evidence of absence",
          "[zoffset][persistence][1401]") {
    // save_variables is delta-only: the overwhelming majority of frames do not
    // carry it, and one that arrives without its variables member has told us
    // nothing either. Refuting on those would drop the stand-down on a real
    // Helper-Script box on the very next gcode_move frame.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});
    using helix::zoffset::status_refutes_persistence;

    CHECK_FALSE(status_refutes_persistence(hw, json{{"gcode_move", json{{"speed", 100.0}}}}));
    CHECK_FALSE(status_refutes_persistence(hw, json{{"save_variables", json::object()}}));
    CHECK_FALSE(
        status_refutes_persistence(hw, json{{"save_variables", json{{"variables", "nope"}}}}));
    CHECK_FALSE(status_refutes_persistence(hw, json{{"save_variables", nullptr}}));
    // Degenerate inputs must not throw.
    CHECK_FALSE(status_refutes_persistence(hw, json::object()));
    CHECK_FALSE(status_refutes_persistence(hw, json()));
}

// ============================================================================
// Claiming the one-shot enable (prestonbrown/helixscreen#1432)
// ============================================================================
