// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix::detection {

enum class DetectionKind { Spaghetti, DirtyBed, Residue, DirtyNozzle, Unknown };

/// Normalized "a detector reported a defect" event.
struct DetectionEvent {
    std::string source_id;
    DetectionKind kind = DetectionKind::Unknown;
    bool attributable = false;
    std::optional<float> confidence;
    bool already_paused = false;
    std::string message;
};

/// The printer's own stored on/off + pause choice, as the vendor stack keeps
/// it. Seeded into HelixScreen settings once, on the first start where a
/// capable source exists; after that the settings own both values.
struct DetectionPreference {
    bool enabled = true;
    bool pause = true;
};

/// A backend that can report print-failure detections.
class DetectionSource {
  public:
    using Callback = std::function<void(const DetectionEvent&)>;

    virtual ~DetectionSource() = default;
    virtual std::string id() const = 0;
    virtual bool available() const = 0;
    virtual bool can_tune() const {
        return false;
    }
    /// Whether the source's own firmware pauses the print on a detection.
    /// The default (false) is the common shape: HelixScreen sends the pause,
    /// so the pause-on-detect setting governs the source and its settings
    /// row shows. Sources paused in firmware override to true; the setting
    /// is inert for them, their row hides, and a warn-only response still
    /// escalates to the modal.
    virtual bool self_pauses() const {
        return false;
    }
    /// Lower the source's sensitivity (the response modal's Tune button).
    /// Called only while can_tune() is true, on the main thread.
    virtual void tune() {}
    /// Re-run the local capability probe. Called on every connect: answers
    /// that depend on state saved after boot (e.g. the wizard's printer
    /// type) can change without a restart. Sources whose capability arrives
    /// over the wire keep the base no-op.
    virtual void refresh_capability() {}
    /// Take this connection's printer.objects.list answer (every Klipper
    /// object name). Called on every connect, on the main thread. Sources
    /// whose capability is a Klipper module decide it here; the rest keep
    /// the base no-op.
    virtual void on_objects_listed(const std::vector<std::string>& objects) {
        (void)objects;
    }
    /// The printer's stored preference, or nullopt when this source has none
    /// (settings then keep their defaults: on, pause on detect).
    virtual std::optional<DetectionPreference> printer_preference() const {
        return std::nullopt;
    }
    /// Whether the print is paused right now because this source detected
    /// something. Read by other pause consumers (the runout guidance) so one
    /// pause gets one explanation, whichever observer runs first.
    virtual bool explains_current_pause() const {
        return false;
    }

    /// The bed check as the printer reports it. A detection module switched off
    /// as a whole runs no bed check, whatever its own flag says.
    struct BedCheckState {
        bool enabled = false;
        bool module_enabled = true;
    };
    /// Callback for a read: the printer's state, or nullopt when the printer
    /// could not be asked or did not answer. Called on any thread; the caller
    /// marshals.
    using BedCheckReadCallback = std::function<void(std::optional<BedCheckState>)>;
    /// Callback for a switch: whether the printer accepted it, and when it did
    /// not, the printer's own reason (empty when it gave none). Any thread.
    using BedCheckSetCallback = std::function<void(bool accepted, std::string refusal)>;
    /// Whether the printer runs a pre-print check for objects left on the bed
    /// that the user can switch on and off.
    virtual bool has_bed_check() const {
        return false;
    }
    /// Ask the printer whether its bed check is on. Called only while
    /// has_bed_check() is true.
    virtual void read_bed_check(BedCheckReadCallback done) {
        done(std::nullopt);
    }
    /// Switch the printer's bed check. Called only while has_bed_check().
    virtual void set_bed_check(bool enabled, BedCheckSetCallback done) {
        (void)enabled;
        done(false, {});
    }
    /// Callback fires on the MAIN thread (already marshaled).
    virtual void set_callback(Callback cb) = 0;
};

} // namespace helix::detection
