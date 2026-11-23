// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::ui {

/// Presentation of the canonical external spool. Editing and operations stay
/// with the existing spool editor and FilamentPanel controllers.
class ExternalSpoolOverview : public OverlayBase {
  public:
    ~ExternalSpoolOverview() override;
    const char* get_name() const override {
        return "External Spool";
    }
    bool is_destination() const override {
        return true;
    }
    const char* xml_component() const override {
        return "external_spool_overview";
    }
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    void on_activate() override;
    void refresh_spool();

  protected:
    void on_deactivating(DeactivateReason) override;

  private:
    SubjectManager subjects_;
    lv_subject_t name_{};
    lv_subject_t material_{};
    lv_subject_t amount_{};
    lv_subject_t accounting_{};
    char name_buf_[128]{};
    char material_buf_[128]{};
    char amount_buf_[160]{};
    char accounting_buf_[192]{};
    ObserverGuard spool_observer_;
};

inline ExternalSpoolOverview& get_external_spool_overview() {
    return lazy_global<ExternalSpoolOverview>("ExternalSpoolOverview");
}

} // namespace helix::ui
