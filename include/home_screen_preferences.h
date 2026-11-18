// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace helix {

class Config;

/// Presentation preferences only: never changes device availability or controls.
/// Reads the active printer on every call; main-thread only, like Config.
class HomeScreenPreferences {
  public:
    enum class DeviceKind { Heater, PassiveSensor, Fan };

    struct SpoolUnits {
        bool percent = true;
        bool grams = true;
        bool metres = true;
    };

    explicit HomeScreenPreferences(Config& config) : config_(config) {}

    /// Full Klipper object IDs are literal JSON keys, independent of display names.
    /// Explicit choices override the caller's legacy tile/sensor fallback.
    /// Without a fallback, absent or malformed choices default to visible.
    bool is_visible(DeviceKind kind, const std::string& object_id,
                    bool default_visible = true) const;

    /// Changes memory only; save() can batch a whole page of checkbox edits.
    void set_visible(DeviceKind kind, const std::string& object_id, bool visible);

    /// Sets all listed devices, including an explicit false for every device when
    /// hiding all. Other IDs are retained, so unplugging hardware loses no choices.
    /// Newly discovered IDs remain visible until explicitly changed.
    void set_all_visible(DeviceKind kind, const std::vector<std::string>& object_ids, bool visible);

    SpoolUnits spool_units() const;
    void set_spool_units(const SpoolUnits& units);

    /// Persist through Config and report its success/failure to the caller.
    bool save();

  private:
    Config& config_;
};

} // namespace helix
