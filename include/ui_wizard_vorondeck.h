// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "calibration_receipt.h"
#include "subject_managed_panel.h"
#include "wizard_step.h"

#include <array>

class WizardAppearanceStep : public helix::wizard::Step {
  public:
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Appearance;
    }
    const char* component_name() const override {
        return "wizard_appearance";
    }
    const char* log_name() const override {
        return "WizardAppearance";
    }
    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;
    bool is_validated() const override {
        return true;
    }
    void refresh();
    void rename();

  private:
    bool initialized_ = false;
    SubjectManager subjects_;
    lv_subject_t image_{}, name_{};
    char image_buf_[512]{}, name_buf_[128]{};
    ObserverGuard image_observer_;
};

class WizardChecksStep : public helix::wizard::Step {
    friend struct WizardChecksTestAccess;

  public:
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Checks;
    }
    const char* component_name() const override {
        return "wizard_checks";
    }
    const char* log_name() const override {
        return "WizardChecks";
    }
    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;
    bool is_validated() const override {
        return true;
    }
    enum class Tool { Pid, Mesh, ZOffset, Shaper };
    void watch(Tool tool, const helix::CalibrationReceipt& receipt);
    bool is_completed(Tool tool);

  private:
    void begin_session(lv_obj_t* root, const std::string& printer);
    void receive(Tool tool, const helix::CalibrationReceipt::Event& event,
                 const std::string& current_printer);
    void clear_results();
    bool initialized_ = false;
    SubjectManager subjects_;
    std::array<lv_subject_t, 4> completed_{};
    std::array<uint64_t, 4> attempts_{};
    std::array<ObserverGuard, 4> receipt_observers_;
    helix::ui::WidgetRef session_root_;
    std::string printer_;
};

WizardAppearanceStep* get_wizard_appearance_step();
WizardChecksStep* get_wizard_checks_step();
