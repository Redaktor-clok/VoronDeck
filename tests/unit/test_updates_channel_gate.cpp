// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_panel_settings.h"
#include "ui_settings_updates.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

namespace {
struct UpdatesChannelGateFixture : public LVGLUITestFixture {
    UpdatesChannelGateFixture() {
        get_global_settings_panel().init_subjects();
        helix::settings::get_updates_settings_overlay().register_callbacks();
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_updates_overlay", nullptr));
    }
    ~UpdatesChannelGateFixture() override {
        if (root_ && helix::test::widget_in_tree(root_))
            lv_obj_delete(root_);
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
    }
    lv_obj_t* root_ = nullptr;
};
} // namespace

TEST_CASE_METHOD(UpdatesChannelGateFixture,
                 "VoronDeck update check remains available with upstream updater disabled",
                 "[settings][updates][update][channel][vorondeck_updates]") {
    REQUIRE(root_ != nullptr);
    auto* gate = lv_xml_get_subject(nullptr, "show_update_settings");
    REQUIRE(gate != nullptr);
    for (const int value : {0, 1}) {
        lv_subject_set_int(gate, value);
        process_lvgl(10);
        for (const char* name : {"row_stable_release", "row_test_release", "row_beta_release",
                                 "row_component_updates", "vorondeck_update_status"}) {
            CAPTURE(value, name);
            auto* row = lv_obj_find_by_name(root_, name);
            REQUIRE(row != nullptr);
            for (auto* node = row; node && node != root_; node = lv_obj_get_parent(node)) {
                CHECK_FALSE(lv_obj_has_flag(node, LV_OBJ_FLAG_HIDDEN));
            }
        }
    }
    for (const char* name :
         {"row_vorondeck_check", "row_update_channel", "row_update_channel_dev",
          "row_install_update", "row_check_updates", "row_updates_firmware_managed"}) {
        CAPTURE(name);
        CHECK(lv_obj_find_by_name(root_, name) == nullptr);
    }
}
