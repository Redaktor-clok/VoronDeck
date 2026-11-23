#pragma once

#include "i_moonraker_client.h"
#include "manual_bed_screws.h"
#include "observer_factory.h"
#include "overlay_base.h"

class ManualBedScrewsPanel : public OverlayBase {
  public:
    ~ManualBedScrewsPanel() override;
    const char* get_name() const override {
        return "Manual Bed Screws";
    }
    const char* xml_component() const override {
        return "manual_bed_screws";
    }
    void init_subjects() override;
    void register_callbacks() override;
    void on_activate() override;
    void refresh();
    void act(helix::manual_bed_screws::Action action);

  protected:
    void on_deactivating(DeactivateReason reason) override;

  private:
    void request(const std::string& method, const nlohmann::json& params,
                 std::function<void(const nlohmann::json&)> done);
    void display(const nlohmann::json& status);
    void unavailable(const char* text);
    SubjectManager subjects_;
    lv_subject_t phase_{}, busy_{}, status_{}, start_allowed_{}, next_allowed_{}, abort_allowed_{};
    char status_buf_[768]{};
    bool pending_ = false;
    uint64_t generation_ = 0;
    helix::IMoonrakerClient* client_ = nullptr;
    helix::SubscriptionId subscription_ = 0;
    ObserverGuard connection_observer_;
};

void ui_manual_bed_screws_register_callbacks();
