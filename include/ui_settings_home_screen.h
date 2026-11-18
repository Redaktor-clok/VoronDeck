// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "helix/xml/indexed_subject_pool.h"
#include "home_screen_preferences.h"
#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <string>
#include <vector>

namespace helix::settings {

/// Home presentation only. Device controls and discovery remain independent.
class HomeScreenSettingsOverlay : public OverlayBase {
  public:
    ~HomeScreenSettingsOverlay() override;

    const char* get_name() const override {
        return "Home screen";
    }
    const char* xml_component() const override {
        return "settings_home_screen_overlay";
    }
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    void on_activate() override;

    void handle_device_changed(HomeScreenPreferences::DeviceKind kind, int index, bool visible);
    void handle_spool_units_changed();
    void show_section(int section);
    void handle_back();

  protected:
    void before_show() override;
    void on_deactivating(DeactivateReason reason) override;

  private:
    struct DeviceRow {
        std::string id;
        std::string name;
        bool default_visible = true;
        bool operator==(const DeviceRow& other) const {
            return id == other.id && name == other.name && default_visible == other.default_visible;
        }
    };
    struct RowSubjects {
        explicit RowSubjects(const char* prefix)
            : name(prefix, xml::IndexedSubjectPool::Type::String, 128, "_name"),
              checked(prefix, xml::IndexedSubjectPool::Type::Int, 0, "_checked") {}
        xml::IndexedSubjectPool name;
        xml::IndexedSubjectPool checked;
        lv_subject_t count{};
        std::vector<DeviceRow> rows;
    };

    void refresh_inventory();
    void publish_rows(RowSubjects& group, const std::vector<DeviceRow>& rows,
                      HomeScreenPreferences::DeviceKind kind);
    RowSubjects& row_subjects(HomeScreenPreferences::DeviceKind kind);
    void stop_observers();
    void persist_changes();

    SubjectManager subjects_;
    lv_subject_t section_{};
    lv_subject_t title_{};
    char title_buf_[128]{};
    RowSubjects heaters_{"home_heater"};
    RowSubjects sensors_{"home_sensor"};
    RowSubjects fans_{"home_fan"};
    lv_subject_t spool_percent_{};
    lv_subject_t spool_grams_{};
    lv_subject_t spool_metres_{};
    ObserverGuard sensor_observer_;
    ObserverGuard extruder_observer_;
    ObserverGuard fan_observer_;
};

inline HomeScreenSettingsOverlay& get_home_screen_settings_overlay() {
    return lazy_global<HomeScreenSettingsOverlay>("HomeScreenSettingsOverlay");
}

} // namespace helix::settings
