// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"
#include "../catch_amalgamated.hpp"

using namespace helix;

class WizardOverlayReturnFixture : public LVGLUITestFixture {
  protected:
    WizardOverlayReturnFixture() {
        lv_obj_t* panels[kUiPanelCount] = {};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
        nav().set_panels(panels);
    }
    static NavigationManager& nav() { return NavigationManager::instance(); }
    static void drain() { ui::UpdateQueueTestAccess::drain_all(ui::UpdateQueue::instance()); }
    lv_obj_t* create_wizard() {
        ui_wizard_init_subjects();
        ui_wizard_register_event_callbacks();
        ui_wizard_container_register_responsive_constants();
        auto* root = ui_wizard_create(test_screen());
        REQUIRE(root);
        REQUIRE(ui_wizard_is_container(root));
        REQUIRE_FALSE(nav().is_panel_in_stack(root));
        return root;
    }
};

TEST_CASE_METHOD(WizardOverlayReturnFixture,
                 "Closing a wizard child overlay preserves only the actual wizard root",
                 "[wizard][navigation][wizard_overlay_return]") {
    auto* wizard = create_wizard();
    auto* overlay = lv_obj_create(test_screen());
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    nav().register_overlay_instance(overlay, nullptr);
    nav().push_overlay(overlay);
    drain();
    process_lvgl(60);
    REQUIRE(nav().is_panel_on_top(overlay));
    REQUIRE_FALSE(lv_obj_has_flag(wizard, LV_OBJ_FLAG_HIDDEN));

    // A matching name is not sufficient to exempt an unrelated screen child.
    auto* stale = lv_obj_create(test_screen());
    lv_obj_set_name(stale, "wizard_container");
    REQUIRE_FALSE(ui_wizard_is_container(stale));
    REQUIRE(nav().go_back());
    drain();
    process_lvgl(60);
    CHECK_FALSE(lv_obj_has_flag(wizard, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(stale, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(nav().is_panel_in_stack(overlay));
    CHECK_FALSE(nav().is_panel_in_stack(wizard));
}

TEST_CASE_METHOD(WizardOverlayReturnFixture,
                 "Navbar stale overlay sweep preserves the live wizard without exempting lookalikes",
                 "[wizard][navigation][wizard_overlay_return]") {
    auto* wizard = create_wizard();
    auto* stale = lv_obj_create(test_screen());
    lv_obj_set_name(stale, "wizard_container");
    NavigationManagerTestAccess::switch_to_panel(nav(), PanelId::Controls);
    drain();
    process_lvgl(60);
    CHECK_FALSE(lv_obj_has_flag(wizard, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(stale, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(WizardOverlayReturnFixture,
                 "Wizard root identity expires on deletion and teardown and follows a new session",
                 "[wizard][navigation][wizard_overlay_return]") {
    CHECK_FALSE(ui_wizard_is_container(nullptr));
    auto* wizard = create_wizard();
    lv_obj_delete(wizard); // WidgetRef must clear even if the screen owns deletion.
    CHECK_FALSE(ui_wizard_is_container(wizard));
    auto* replacement = create_wizard();
    REQUIRE(ui_wizard_is_container(replacement));
    ui_wizard_deinit_subjects();
    CHECK_FALSE(ui_wizard_is_container(replacement));
    CHECK_FALSE(ui_wizard_is_container(nullptr));

    auto* stale = lv_obj_create(test_screen());
    lv_obj_set_name(stale, "wizard_container");
    CHECK_FALSE(ui_wizard_is_container(stale));
    NavigationManagerTestAccess::switch_to_panel(nav(), PanelId::Controls);
    drain();
    process_lvgl(60);
    CHECK(lv_obj_has_flag(stale, LV_OBJ_FLAG_HIDDEN));
}
