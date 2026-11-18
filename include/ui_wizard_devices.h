// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wizard_step.h"
#include <string>
#include <vector>

namespace helix {
class PrinterDiscovery;
struct WizardDeviceDefault {
    std::string key;
    std::string value;
};
// Read-only proposals. Existing keys, including explicit empty selections, are preserved.
std::vector<WizardDeviceDefault> wizard_device_defaults(const Config& config,
                                                        const PrinterDiscovery& hardware);
// Applies only still-missing keys; does not save to disk.
bool apply_wizard_device_defaults(Config& config,
                                 const std::vector<WizardDeviceDefault>& defaults);
}

class WizardDevicesStep : public helix::wizard::Step {
  public:
    helix::wizard::StepId id() const override { return helix::wizard::StepId::Devices; }
    const char* component_name() const override { return "wizard_devices"; }
    const char* log_name() const override { return "Wizard Devices"; }
    void init_subjects() override {}
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void cleanup() override;
  private:
    std::vector<helix::WizardDeviceDefault> defaults_;
    std::string printer_prefix_;
    bool created_ = false;
};

WizardDevicesStep* get_wizard_devices_step();
