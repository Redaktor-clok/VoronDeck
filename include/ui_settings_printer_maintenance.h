// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "helix/xml/indexed_subject_pool.h"
#include "overlay_base.h"
#include "printer_maintenance.h"
#include "static_panel_registry.h"

#include <functional>
namespace helix {
class IMoonrakerClient;
}

namespace helix::settings {
class PrinterMaintenanceOverlay : public OverlayBase {
  public:
    ~PrinterMaintenanceOverlay() override;
    const char* get_name() const override {
        return "Printer maintenance";
    }
    const char* xml_component() const override {
        return "settings_printer_maintenance_overlay";
    }
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    void on_activate() override;
    void open(lv_obj_t* parent, bool updates);
    void refresh(bool check_remote = false);
    void confirm_reset(bool totals);
    void confirm_update(int index);

  private:
    bool may_act();
    void send(const maintenance::Request& request, std::function<void(const json&)> done,
              uint32_t timeout = 30000);
    void status(const std::string& text);
    void publish(const json& result);
    SubjectManager subjects_;
    lv_subject_t section_{}, title_{}, status_{}, busy_{}, count_{};
    char title_buf_[96]{}, status_buf_[768]{};
    xml::IndexedSubjectPool names_{"maintenance_component", xml::IndexedSubjectPool::Type::String,
                                   160, "_name"};
    xml::IndexedSubjectPool descriptions_{
        "maintenance_component", xml::IndexedSubjectPool::Type::String, 768, "_description"};
    xml::IndexedSubjectPool enabled_{"maintenance_component", xml::IndexedSubjectPool::Type::Int, 0,
                                     "_enabled"};
    std::vector<maintenance::Component> components_;
    std::string printer_id_;
    bool server_busy_ = false;
    uint64_t request_generation_ = 0;
    IMoonrakerClient* request_client_ = nullptr;
};
inline PrinterMaintenanceOverlay& get_printer_maintenance_overlay() {
    return lazy_global<PrinterMaintenanceOverlay>("PrinterMaintenanceOverlay");
}
} // namespace helix::settings
