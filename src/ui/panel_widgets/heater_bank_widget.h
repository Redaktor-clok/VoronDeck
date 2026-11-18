// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ui_heating_animator.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>
#include <vector>
namespace helix {
class PrinterState;
class HeaterBankWidget : public PanelWidget {
  public:
    explicit HeaterBankWidget(PrinterState& state) : state_(state) {}
    ~HeaterBankWidget() override {
        detach();
    }
    const char* id() const override {
        return "heater_bank";
    }
    void set_config(const nlohmann::json& config) override {
        config_ = config;
    }
    std::string get_component_name() const override {
        return list_mode() ? "panel_widget_heater_bank_list" : "panel_widget_heater_bank";
    }
    void init_subjects() override;
    static void device_clicked(lv_event_t*);
    void attach(lv_obj_t*, lv_obj_t*) override;
    void detach() override;
    void on_activate() override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    void on_size_changed(int, int, int, int) override;
    bool fits_at(int, int) const override;

  private:
    struct Row {
        HeaterBankWidget* owner = nullptr;
        std::string name;
        lv_obj_t* object = nullptr;
        lv_obj_t* icon = nullptr;
        lv_obj_t* temperature = nullptr;
        int current = 0, target = 0, power = -1;
        size_t index = 0;
        ui::HeatingIconAnimator animator;
        ObserverGuard current_observer, target_observer, power_observer;
        void refresh();
    };
    struct LayoutFit {
        int value = -1;
        int icon = 0;
        bool compact_caption = false;
    };
    LayoutFit choose_size(size_t, int, int, bool list = false) const;
    std::vector<std::string> inventory() const;
    bool selected(const std::string&) const;
    bool apply_selection(const nlohmann::json&, bool list_mode);
    bool list_mode() const {
        return config_.contains("display_mode") && config_["display_mode"] == "list";
    }
    void rebuild_rows();
    void schedule_rebuild();
    void open_target(Row&);
    bool portrait() const;
    PrinterState& state_;
    nlohmann::json config_;
    ui::WidgetRef widget_, cells_, screen_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<std::string> applied_inventory_;
    std::vector<std::string> applied_selection_;
    std::vector<int32_t> columns_, tracks_;
    ObserverGuard inventory_observer_, extruder_observer_;
    bool rebuild_pending_ = false;
    AsyncLifetimeGuard lifetime_;
};
} // namespace helix
