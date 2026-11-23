// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "src/ui/panel_widgets/tiled_panel_widget.h"

namespace helix {
class CooldownWidget : public TiledPanelWidget {
  public:
    CooldownWidget();
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "cooldown";
    }
    static void clicked_cb(lv_event_t* e);

  private:
    lv_subject_t direction_{};
    SubjectManager subjects_;
};
void register_cooldown_widget();
} // namespace helix
