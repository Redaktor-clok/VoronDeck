// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_calibration.h"

#include "printer_discovery.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace helix::pacal {
namespace {

/// One firmware that can measure pressure advance without a human judging a
/// printed part.
struct Provider {
    const char* name;

    /// Macro whose presence identifies the firmware. This is evidence, not a
    /// guess: a printer that does not define it cannot run the procedure, and
    /// the capability is reported absent rather than attempted and refused.
    /// Null when the firmware is detected by object instead.
    const char* detect_macro;

    /// Printer object whose presence identifies the firmware, for commands
    /// registered by a klippy Python extra: those never appear in the object
    /// list as "gcode_macro X". Null when the firmware is detected by macro.
    const char* detect_object;

    /// Command template. {tool} is the tool index and {temp} the nozzle
    /// temperature; a template may use either, both or neither.
    const char* gcode;

    /// Whether each tool is calibrated separately.
    bool per_tool;

    /// See Procedure: regexes over the firmware's console output.
    const char* result_pattern;
    const char* attempt_pattern;
    const char* failure_pattern;
    int expected_attempts;

    /// See Procedure.
    bool applies_result;
    int estimate_minutes;

    /// The extruder kind this firmware's machines use, which is what makes the
    /// plausibility band meaningful. Detected extruder geometry does not exist
    /// in PrinterDiscovery, and inferring it from kinematics would be a guess:
    /// the firmware entry knows its own hardware.
    SaneRange range;

    uint32_t timeout_ms;
};

constexpr SaneRange kDirectDrive{0.02f, 0.08f, "direct drive"};

const Provider* match(const PrinterDiscovery& /*hw*/) {
    return nullptr;
}

} // namespace

bool is_supported(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string{};
}

bool is_per_tool(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->per_tool;
}

std::optional<Procedure> procedure_for(const PrinterDiscovery& hw, int tool_index, int temp_c) {
    const Provider* p = match(hw);
    if (!p) {
        return std::nullopt;
    }

    Procedure proc;
    proc.provider = p->name;
    proc.start_gcode =
        fmt::format(fmt::runtime(p->gcode), fmt::arg("tool", tool_index), fmt::arg("temp", temp_c));
    proc.result_pattern = p->result_pattern;
    proc.attempt_pattern = p->attempt_pattern ? p->attempt_pattern : "";
    proc.failure_pattern = p->failure_pattern ? p->failure_pattern : "";
    proc.expected_attempts = p->expected_attempts;
    proc.applies_result = p->applies_result;
    proc.estimate_minutes = p->estimate_minutes;
    proc.timeout_ms = p->timeout_ms;

    // The command word is the first token of the template, which is also what
    // Klipper quotes back in "Unknown command:".
    proc.command_word = proc.start_gcode.substr(0, proc.start_gcode.find(' '));

    return proc;
}

SaneRange sane_range(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->range : kDirectDrive;
}

bool is_plausible(const PrinterDiscovery& hw, float k) {
    const SaneRange r = sane_range(hw);
    return k >= r.low && k <= r.high;
}

} // namespace helix::pacal
