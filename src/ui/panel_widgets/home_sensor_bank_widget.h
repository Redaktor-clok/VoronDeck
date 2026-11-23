// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "helix/xml/indexed_subject_pool.h"
#include "panel_widget.h"
#include "subject_managed_panel.h"

#include <array>
#include <string>

namespace helix {

/// Compact passive-temperature inventory. Subjects precede the XML repeat rows.
class HomeSensorBankWidget : public PanelWidget {
  public:
    HomeSensorBankWidget();
    ~HomeSensorBankWidget() override;
    const char* id() const override {
        return "home_sensor_bank";
    }
    const char** xml_attrs() const override {
        return const_cast<const char**>(attrs_.data());
    }
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;

  private:
    void refresh();
    std::string prefix_;
    std::string count_name_;
    lv_subject_t count_{};
    SubjectManager subjects_;
    xml::IndexedSubjectPool names_;
    xml::IndexedSubjectPool temperatures_;
    std::array<const char*, 3> attrs_{};
    bool attached_ = false;
    ObserverGuard revision_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
