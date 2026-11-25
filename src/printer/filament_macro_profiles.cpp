// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_macro_profiles.h"

#include "json_utils.h"
#include "text_io.h"

#include <array>
#include <string>

namespace helix::filament_macros {
namespace {

/// A load macro that is the first step of a wizard the firmware's own screen
/// finishes. Every script is a template: `{temp}` becomes the temperature the
/// macro ran at.
struct GuidedLoadProfile {
    /// The parameter the macro reads its temperature from, and its default.
    const char* temp_param;
    int default_temp_c;
    /// Object, field and value that report the macro stopped for a hand push.
    const char* waiting_object;
    const char* waiting_field;
    const char* waiting_value;
    const char* feed;
    const char* retry;
    const char* flush;
    const char* release;
    const char* cleanup;
};

/// One stock macro, and what it does for itself.
struct MacroProfile {
    /// Uppercase macro name. Must be a spelling that carries no meaning in
    /// stock Klipper - a generic name proves nothing about the body.
    const char* name;
    /// Which firmware ships it, for the reader deciding whether a new row
    /// belongs beside this one.
    const char* firmware;
    /// Drives the hotend to temperature and waits, so a UI preheat in front of
    /// it is a second wait at a temperature the macro then overrides.
    bool heats_hotend;
    /// Moves the toolhead with no home of its own, so the printer must be homed
    /// before it is sent.
    bool needs_home_first;
    /// What the macro leaves undone that must be sent after it, or "".
    const char* restore_after;
    /// The wizard this macro starts, or nullptr.
    const GuidedLoadProfile* guided_load;
};

constexpr std::array<MacroProfile, 0> kMacroProfiles{};

/// The row for @p macro_name, or nullptr when no firmware we know ships it.
const MacroProfile* find_profile(const std::string& macro_name) {
    if (macro_name.empty()) {
        return nullptr;
    }
    const std::string upper = helix::text_io::to_upper(macro_name);
    for (const MacroProfile& p : kMacroProfiles) {
        if (upper == p.name) {
            return &p;
        }
    }
    return nullptr;
}

std::string render(const char* tmpl, int temp_c) {
    std::string out = tmpl;
    const std::string temp = std::to_string(temp_c);
    for (size_t at = out.find("{temp}"); at != std::string::npos;
         at = out.find("{temp}", at + temp.size())) {
        out.replace(at, 6, temp);
    }
    return out;
}

GuidedLoad render_guided_load(const GuidedLoadProfile& w, int temp_c) {
    GuidedLoad g;
    g.waiting_object = w.waiting_object;
    g.waiting_field = w.waiting_field;
    g.waiting_value = w.waiting_value;
    g.feed = render(w.feed, temp_c);
    g.retry = render(w.retry, temp_c);
    g.flush = render(w.flush, temp_c);
    g.release = render(w.release, temp_c);
    g.cleanup = render(w.cleanup, temp_c);
    return g;
}

} // namespace

bool macro_heats_hotend(const std::string& macro_name) {
    const MacroProfile* p = find_profile(macro_name);
    return p && p->heats_hotend;
}

bool macro_needs_home_first(const std::string& macro_name) {
    const MacroProfile* p = find_profile(macro_name);
    return p && p->needs_home_first;
}

std::optional<GuidedLoad> guided_load(const std::string& macro_name,
                                      const std::map<std::string, std::string>& sent_params) {
    const MacroProfile* p = find_profile(macro_name);
    if (!p || !p->guided_load) {
        return std::nullopt;
    }
    const GuidedLoadProfile& w = *p->guided_load;
    int temp_c = w.default_temp_c;
    for (const auto& [key, value] : sent_params) {
        if (helix::text_io::to_upper(key) == w.temp_param) {
            const int sent = helix::text_io::parse_leading<int>(value).value_or(0);
            if (sent > 0) {
                temp_c = sent;
            }
        }
    }
    return render_guided_load(w, temp_c);
}

std::optional<GuidedLoad> guided_load_example() {
    for (const MacroProfile& p : kMacroProfiles) {
        if (p.guided_load) {
            return render_guided_load(*p.guided_load, p.guided_load->default_temp_c);
        }
    }
    return std::nullopt;
}

nlohmann::json guided_load_status_query(const GuidedLoad& steps) {
    return {{"objects", {{steps.waiting_object, nlohmann::json::array({steps.waiting_field})}}}};
}

bool guided_load_waits(const nlohmann::json& query_response, const GuidedLoad& steps) {
    const nlohmann::json* result = json_util::find_member(query_response, "result");
    const nlohmann::json* status = result ? json_util::find_member(*result, "status") : nullptr;
    const nlohmann::json* object =
        status ? json_util::find_member(*status, steps.waiting_object.c_str()) : nullptr;
    const nlohmann::json* field =
        object ? json_util::find_member(*object, steps.waiting_field.c_str()) : nullptr;
    return field && field->is_string() && json_util::as_string(*field) == steps.waiting_value;
}

std::string restore_after(const std::string& macro_name) {
    const MacroProfile* p = find_profile(macro_name);
    return p ? p->restore_after : std::string{};
}

} // namespace helix::filament_macros
