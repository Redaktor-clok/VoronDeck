// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AMS panel's dry-box unit as laid out: spools stand in the box, labels sit
// above its lid (or back wall), the readout is a pill on the box's end, and under
// per-lane lids each lane shows its own humidity behind a droplet.

#include "ui_ams_detail.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_humidity_drop.h"
#include "ui_panel_ams.h"
#include "ui_update_queue.h"
#include "ui_modal.h"
#include "../test_helpers/backend_user_edit.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../test_helpers/scoped_geometry.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_tray_projection.h"
#include "config.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "src/ui/ui_filament_path_internal.h"
#include "src/ui/ui_filament_path_plan.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace tray = helix::ui::tray;

namespace {

void register_ams_xml_once() {
    static bool done = false;
    if (done)
        return;
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_environment_indicator.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_env_pill.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");
    done = true;
}

class AmsTrayPanelFixture : public XMLTestFixture {
  public:
    /// A tool changer has no housing, and flat spools stand in none, so build()
    /// finds no tray for either.
    void build(const char* env_mode, int gates = 4, bool tool_changer = false) {
        helix::Config::get_instance()->set<std::string>("/ams/spool_style", flat_ ? "flat" : "3d");
        auto mock = std::make_unique<AmsBackendMock>(gates);
        if (tool_changer)
            mock->set_tool_changer_mode(true);
        else
            mock->set_afc_mode(true);
        mock->set_environment_mode(env_mode);
        REQUIRE(mock->start().success());
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
        register_ams_xml_once();

        panel_ = std::make_unique<AmsPanel>(state(), &api());
        panel_->init_subjects();
        lv_obj_t* parent = host_ ? host_ : test_screen();
        panel_obj_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_panel", nullptr));
        REQUIRE(panel_obj_ != nullptr);
        panel_->setup(panel_obj_, test_screen());
        panel_->on_activate();
        lv_obj_update_layout(test_screen());
        process_lvgl(50);
        panel_->refresh_slots();
        process_lvgl(20);
        lv_obj_update_layout(test_screen());

        container_ = lv_obj_find_by_name(panel_obj_, "slot_container");
        slot_grid_ = lv_obj_find_by_name(panel_obj_, "slot_grid");
        REQUIRE(container_ != nullptr);
        REQUIRE(slot_grid_ != nullptr);
        lv_area_t c;
        lv_obj_get_coords(container_, &c);
        origin_ = {c.x1, c.y1};
        CHECK_FALSE(helix::ui::ams_detail_tray_geometry(box_, lid_, lid_h_, half_));
    }

    ~AmsTrayPanelFixture() override {
        if (panel_) {
            panel_->on_deactivate(DeactivateReason::NavigateAway);
            panel_->clear_panel_reference();
        }
        if (panel_obj_)
            lv_obj_delete(panel_obj_);
        process_lvgl(10);
        AmsState::instance().set_backend(nullptr);
        helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
    }

    lv_obj_t* slot(int i) const {
        return lv_obj_get_child(slot_grid_, i);
    }

    float top() const {
        return tray::unit_top_y(box_, lid_h_, lid_ != tray::LidMode::None);
    }

    /// The environment pill stands against the box's right end, as tall as the
    /// box, inside the unit's row, and covers no spool, badge or label.
    void check_env_chip_clear(int gates) {
        lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
        REQUIRE(pill != nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
        lv_obj_update_layout(test_screen());
        lv_area_t pa;
        lv_obj_get_coords(pill, &pa);
        CAPTURE(pa.x1, pa.y1, pa.x2, pa.y2);
        CHECK(std::fabs((float)(pa.x1 - origin_.x) - tray::pill_left_x(box_)) <= 1);
        CHECK(std::fabs((float)(pa.y1 - origin_.y) - top()) <= 1);
        CHECK(std::fabs((float)(pa.y2 - origin_.y) - box_.fb) <= 1);
        lv_area_t row;
        lv_obj_get_coords(lv_obj_get_parent(container_), &row);
        CHECK(pa.x2 <= row.x2);
        std::vector<lv_obj_t*> obstacles;
        for (const char* layer_name : {"badge_layer", "labels_layer"}) {
            lv_obj_t* layer = lv_obj_find_by_name(panel_obj_, layer_name);
            for (uint32_t i = 0; layer && i < lv_obj_get_child_count(layer); ++i)
                obstacles.push_back(lv_obj_get_child(layer, (int32_t)i));
        }
        for (int i = 0; i < gates; ++i)
            for (const char* n : {"material_label", "name_label", "tool_badge"})
                if (lv_obj_t* o = lv_obj_find_by_name(slot(i), n))
                    obstacles.push_back(o);
        int checked = 0;
        for (lv_obj_t* o : obstacles) {
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
                continue;
            lv_area_t oa;
            lv_obj_get_coords(o, &oa);
            CAPTURE(oa.x1, oa.y1, oa.x2, oa.y2);
            CHECK_FALSE((oa.x1 <= pa.x2 && oa.x2 >= pa.x1 && oa.y1 <= pa.y2 && oa.y2 >= pa.y1));
            ++checked;
        }
        REQUIRE(checked >= 2 * gates);
        // The spools stand inside the box, and the pill outside its right end.
        const tray::TrayFaces f = tray::tray_faces(box_);
        float box_right = -1e9f;
        for (const auto* face : {f.back_wall, f.floor, f.front, f.right_side})
            for (int k = 0; k < 4; ++k)
                box_right = std::max(box_right, face[k].x);
        CHECK((float)(pa.x1 - origin_.x) >= box_right - 1);
    }

    /// Every lane's number badge lies on the box front.
    void check_badges_on_front(int gates) {
        lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
        REQUIRE(badges != nullptr);
        REQUIRE(lv_obj_get_child_count(badges) == (uint32_t)gates);
        for (int i = 0; i < gates; ++i) {
            CAPTURE(i);
            lv_area_t ba;
            lv_obj_get_coords(lv_obj_get_child(badges, i), &ba);
            CHECK((float)(ba.y1 - origin_.y) >= box_.ft - 1);
            CHECK((float)(ba.y2 - origin_.y) <= box_.fb + 1);
        }
    }

    bool flat_ = false;
    std::unique_ptr<AmsPanel> panel_;
    lv_obj_t* panel_obj_ = nullptr;
    /// Where build() puts the panel; the screen when null.
    lv_obj_t* host_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* slot_grid_ = nullptr;
    lv_point_t origin_{};
    tray::TrayBox box_{};
    tray::LidMode lid_ = tray::LidMode::None;
    float lid_h_ = 0;
    float half_ = 0;
};

} // namespace


// The real detail panel intentionally no longer displays the 3D box. Projection
// and drawing contracts remain in the independent tray geometry/draw suites.
namespace {
lv_area_t bounds(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a;
}
void check_inside(lv_obj_t* child, lv_obj_t* parent) {
    const auto a = bounds(child), p = bounds(parent);
    CHECK(a.x1 >= p.x1); CHECK(a.x2 <= p.x2);
    CHECK(a.y1 >= p.y1); CHECK(a.y2 <= p.y2);
}
}

TEST_CASE("AMS face-on cells stay readable and wrap within one unit", "[ams][detail-rings]") {
    for (int width : {240,400,640})
        for (int count = 1; count <= 24; ++count) {
            const auto layout = calculate_ams_ring_layout(width,count);
            CHECK(layout.overlap == 0);
            CHECK(layout.slot_width > 0);
            CHECK(layout.slot_width >= 88);
            CHECK(layout.slot_width <= width);
        }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS detail rings climate and native topology fit portrait and landscape",
                 "[ams][tray][detail-rings][ui_integration]") {
    const bool portrait = GENERATE(false,true);
    const bool seven_inch = GENERATE(false, true);
    const int count = GENERATE(4,8,16,24);
    const char* climate = GENERATE("passive","off","slot");
    const int width = seven_inch ? 1024 : 800;
    const int height = seven_inch ? 600 : 480;
    helix::test::ScopedGeometry geometry(portrait ? height : width, portrait ? width : height);
    build(climate,count);
    if (portrait) {
        // The live portrait navigation occupies the bottom 72 pixels.
        lv_obj_set_height(panel_obj_, width - 72);
        process_lvgl(30);
    }
    REQUIRE(lv_obj_get_child_count(slot_grid_) == static_cast<uint32_t>(count));
    int32_t previous_right = -1, baseline = -1;
    for (int i=0;i<count;++i) {
        CAPTURE(i,count,portrait,climate);
        auto* canvas = lv_obj_find_by_name(slot(i),"spool_graphic");
        auto* badge = ui_ams_slot_get_status_badge(slot(i));
        REQUIRE(canvas); REQUIRE(badge);
        CHECK(ui_ams_slot_is_simple(slot(i)));
        CHECK(helix::ui::spool_canvas_get_view(canvas) == helix::ui::SpoolView::Ring);
        check_inside(canvas,slot(i)); check_inside(badge,slot(i));
        check_inside(slot(i),slot_grid_);
        if (bounds(slot(i)).y1 == bounds(slot(0)).y1) {
            check_inside(slot(i), lv_obj_get_parent(slot_grid_));
            check_inside(slot(i), lv_obj_find_by_name(panel_obj_, "left_column"));
        }
        const auto a=bounds(canvas);
        CHECK(lv_obj_get_width(slot(i)) >= 88);
        CHECK(a.x2-a.x1+1 >= 48);
        if (a.y1 != baseline) previous_right = -1;
        CHECK(a.x1 > previous_right);
        if (baseline>=0) CHECK(a.y1>=baseline);
        baseline=a.y1; previous_right=a.x2;
        auto* lane_climate=lv_obj_find_by_name(slot(i),"lane_humidity");
        REQUIRE(lane_climate);
        if (!lv_obj_has_flag(lane_climate,LV_OBJ_FLAG_HIDDEN)) {
            CHECK(bounds(lane_climate).y1>a.y2);
            check_inside(lane_climate,slot(i));
        }
    }
    auto* pill=lv_obj_find_by_name(panel_obj_,"env_indicator");
    auto* unit=lv_obj_find_by_name(panel_obj_,"unit_detail");
    REQUIRE(pill); REQUIRE(unit);
    if (!lv_obj_has_flag(pill,LV_OBJ_FLAG_HIDDEN)) {
        if (portrait) {
            check_inside(pill,unit);
            CHECK(bounds(pill).y2 < bounds(slot_grid_).y1);
        } else {
            auto* sidebar=lv_obj_find_by_name(panel_obj_,"ams_operation_sidebar");
            REQUIRE(sidebar);
            check_inside(pill,sidebar);
        }
    }
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(panel_obj_,"slot_tray"),LV_OBJ_FLAG_HIDDEN));
    auto* canvas=lv_obj_find_by_name(panel_obj_,"path_canvas");
    REQUIRE(canvas);
    REQUIRE(lv_obj_get_height(canvas)>0);
    // Portrait has one scroll surface for cards and schematic, while actions stay fixed.
    lv_obj_scroll_to_view_recursive(canvas, LV_ANIM_OFF);
    process_lvgl(30);
    check_inside(canvas,panel_obj_);
    check_inside(canvas,lv_obj_find_by_name(panel_obj_, "left_column"));
    const auto* data=helix::ui::fpath::get_data(canvas);
    REQUIRE(data);
    const auto c=bounds(canvas);
    if (!data->slot_grid) {
        const auto frame = helix::ui::fpath::compute_base_geometry(canvas, data);
        CHECK(frame.lane_entry_y >= c.y1);
    }
    int32_t previous_lane = -1;
    for (int i=0;i<count;++i) {
        const auto ring=bounds(lv_obj_find_by_name(slot(i),"spool_graphic"));
        const int32_t lane_x=c.x1+helix::ui::fpath::get_slot_x(data,i,c.x1);
        if (data->slot_grid) {
            CHECK(std::abs(lane_x-(ring.x1+ring.x2)/2)<=2);
        } else {
            CHECK(lane_x > previous_lane);
            CHECK(lane_x >= c.x1);
            CHECK(lane_x <= c.x2);
        }
        previous_lane = lane_x;
    }
    lv_obj_scroll_to_view_recursive(lv_obj_find_by_name(slot(count-1),"spool_graphic"), LV_ANIM_OFF);
    process_lvgl(30);
    const auto last_ring=bounds(lv_obj_find_by_name(slot(count-1),"spool_graphic"));
    const auto viewport=bounds(lv_obj_get_parent(slot_grid_));
    CHECK(last_ring.y1 >= viewport.y1);
    CHECK(last_ring.y2 <= viewport.y2);
    check_inside(lv_obj_find_by_name(slot(count-1),"spool_graphic"),
                 lv_obj_find_by_name(panel_obj_, "left_column"));
    REQUIRE(lv_obj_find_by_name(panel_obj_,"btn_load"));
    REQUIRE(lv_obj_find_by_name(panel_obj_,"btn_unload"));
    REQUIRE(lv_obj_find_by_name(panel_obj_,"btn_reset"));
    for (const char* name : {"btn_load", "btn_unload", "btn_reset"}) {
        auto* action = lv_obj_find_by_name(panel_obj_, name);
        for (auto* parent = lv_obj_get_parent(action); parent; parent = lv_obj_get_parent(parent))
            check_inside(action, parent);
        CHECK(bounds(action).y2 <= bounds(panel_obj_).y2 - 2);
    }
    CHECK(Config::get_instance()->get<std::string>("/ams/spool_style","")=="3d");
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS detail ring fill and readings share actual zero half full unknown weights",
                 "[ams][tray][detail-rings][ui_integration]") {
    build("off");
    auto& ams=AmsState::instance();
    auto* backend=ams.get_backend();
    REQUIRE(backend);
    const float weights[]={0,500,1000,-1};
    for (int i=0;i<4;++i) {
        auto info=backend->get_slot_info(i);
        info.spool_name="Named "+std::to_string(i+1);
        info.material="PLA";info.color_rgb=0xE9434B;
        info.remaining_weight_g=weights[i];info.total_weight_g=i==3?-1:1000;
        REQUIRE(helix::test::apply_edit(*backend,i,info).success());
    }
    ams.sync_from_backend();process_lvgl(30);panel_->refresh_slots();process_lvgl(30);
    const char* percentages[]={"0%","50%","100%","\xE2\x80\x94"};
    for(int i=0;i<4;++i) {
        CAPTURE(i);
        auto* graphic=lv_obj_find_by_name(slot(i),"spool_graphic");
        auto* material=lv_obj_find_by_name(slot(i),"material_label");
        auto* percent=lv_obj_find_by_name(slot(i),"slot_percent_label");
        auto* name=lv_obj_find_by_name(slot(i),"name_label");
        REQUIRE(graphic);REQUIRE(material);REQUIRE(name);REQUIRE(percent);
        const std::string text=lv_label_get_text(percent);
        CHECK(text.find(percentages[i])!=std::string::npos);
        CHECK(std::string(lv_label_get_text(name))=="Named "+std::to_string(i+1));
        CHECK(ui_ams_slot_get_fill_level(slot(i))==Catch::Approx(i==3?0:weights[i]/1000));
        CHECK(lv_color_eq(ui_spool_canvas_get_color(graphic),lv_color_hex(0xE9434B)));
        if(i==3) CHECK(text.find("100%") == std::string::npos);
    }
    auto changed=backend->get_slot_info(1);changed.remaining_weight_g=250;
    REQUIRE(helix::test::apply_edit(*backend,1,changed).success());
    ams.sync_from_backend();process_lvgl(30);
    CHECK(ui_ams_slot_get_fill_level(slot(1))==Catch::Approx(.25));
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(slot(1),"slot_percent_label")))=="25%");
}

TEST_CASE_METHOD(AmsTrayPanelFixture,"AMS detail long material cannot ellipsize the remaining percent",
                 "[ams][tray][detail-rings][ui_integration]") {
    build("off");
    auto& ams=AmsState::instance();
    auto* backend=ams.get_backend();REQUIRE(backend);
    auto info=backend->get_slot_info(1);
    info.material="Silk PLA EXTRA LONG MATERIAL";
    info.remaining_weight_g=750;info.total_weight_g=1000;
    REQUIRE(helix::test::apply_edit(*backend,1,info).success());
    ams.sync_from_backend();process_lvgl(20);
    lv_obj_set_width(slot(1),80);
    ui_ams_slot_fit_simple(slot(1),80);
    lv_obj_update_layout(test_screen());
    auto* percent=lv_obj_find_by_name(slot(1),"slot_percent_label");
    auto* material=lv_obj_find_by_name(slot(1),"material_label");
    REQUIRE(percent);REQUIRE(material);
    CHECK(std::string(lv_label_get_text(percent))=="75%");
    CHECK_FALSE(lv_obj_has_flag(percent,LV_OBJ_FLAG_HIDDEN));
    lv_point_t glyphs;
    lv_text_get_size(&glyphs,"75%",lv_obj_get_style_text_font(percent,LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(percent,LV_PART_MAIN),0,
                     LV_COORD_MAX,LV_TEXT_FLAG_NONE);
    CHECK(lv_obj_get_content_width(percent)>=glyphs.x);
    check_inside(percent,slot(1));
    CHECK(bounds(material).x2 < bounds(percent).x1);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,"AMS detail loaded highlight and native lane click stay reactive",
                 "[ams][tray][detail-rings][ui_integration]") {
    build("off");
    auto& ams=AmsState::instance();
    lv_subject_set_int(ams.get_slot_active_loaded_subject(1),1);process_lvgl(20);
    CHECK(lv_obj_get_style_bg_opa(slot(1),LV_PART_MAIN)>LV_OPA_TRANSP);
    lv_subject_set_int(ams.get_slot_active_loaded_subject(1),0);process_lvgl(20);
    CHECK(lv_obj_get_style_bg_opa(slot(1),LV_PART_MAIN)==LV_OPA_TRANSP);
    lv_obj_send_event(slot(0),LV_EVENT_CLICKED,nullptr);process_lvgl(30);
    CHECK(AmsPanelTestAccess::context_menu_open(*panel_));
}

TEST_CASE_METHOD(AmsTrayPanelFixture,"AMS detail toolchanger keeps parallel routing",
                 "[ams][tray][detail-rings][filament-path][ui_integration]") {
    build("off",4,true);
    auto* canvas=lv_obj_find_by_name(panel_obj_,"path_canvas");REQUIRE(canvas);
    const auto* data=helix::ui::fpath::get_data(canvas);REQUIRE(data);
    CHECK(data->topology==static_cast<int>(PathTopology::PARALLEL));
    for(int i=0;i<4;++i) {
        auto* badge=ui_ams_slot_get_status_badge(slot(i));REQUIRE(badge);
        check_inside(badge,slot(i));
    }
    AmsPanelTestAccess::tap_toolhead(*panel_,1);process_lvgl(30);
    // A toolhead tap offers a tool change, unlike a lane/spool tap's menu.
    CHECK_FALSE(ModalStack::instance().stack_empty());
    ModalStack::instance().clear();
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: a badge moved onto a layer with no width keeps its corner",
                 "[ams][tray][ui_integration]") {
    build("passive", 4, /*tool_changer=*/true);
    lv_obj_t* wide = lv_obj_create(test_screen());
    lv_obj_set_size(wide, 400, 100);
    lv_obj_t* empty = lv_obj_create(test_screen());
    lv_obj_set_size(empty, 0, 0);
    lv_obj_update_layout(test_screen());

    lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(0));
    REQUIRE(badge != nullptr);
    ui_ams_slot_move_badge_to_layer(slot(0), wide, 100);
    const int32_t corner_x = lv_obj_get_style_x(badge, LV_PART_MAIN);
    REQUIRE(corner_x > 0);
    ui_ams_slot_move_badge_to_layer(slot(0), empty, 100);
    CHECK(lv_obj_get_style_x(badge, LV_PART_MAIN) == corner_x);
}


TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit: a spool name stacks directly over its material",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    int named = 0;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* name = lv_obj_find_by_name(slot(i), "name_label");
        lv_obj_t* material = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(name != nullptr);
        REQUIRE(material != nullptr);
        if (lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN))
            continue;
        ++named;
        lv_area_t na, ma;
        lv_obj_get_coords(name, &na);
        lv_obj_get_coords(material, &ma);
        // The tray lifts the material above the unit; the name rides with it.
        const int32_t gap = ma.y1 - na.y2 - 1;
        CHECK(gap >= 0);
        CHECK(gap <= lv_obj_get_style_pad_row(slot(i), LV_PART_MAIN));
    }
    REQUIRE(named > 0);
}


TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS panel in portrait at 480x800: the weight follows the position",
                 "[ams][tray][ui_integration][portrait]") {
    helix::test::ScopedGeometry portrait(480, 800);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "ui_breakpoint")) >= 3);
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, 480, 730);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    build("passive", 8);
    lv_obj_update_layout(test_screen());

    lv_obj_t* details = lv_obj_find_by_name(panel_obj_, "loaded_details");
    lv_obj_t* position = lv_obj_find_by_name(panel_obj_, "loaded_position");
    lv_obj_t* weight = lv_obj_find_by_name(panel_obj_, "loaded_weight");
    REQUIRE(details != nullptr);
    REQUIRE(position != nullptr);
    REQUIRE(weight != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(weight, LV_OBJ_FLAG_HIDDEN));
    lv_area_t da, pa, wa;
    lv_obj_get_coords(details, &da);
    lv_obj_get_coords(position, &pa);
    lv_obj_get_coords(weight, &wa);
    CAPTURE(da.x1, da.x2, pa.x1, pa.x2, wa.x1, wa.x2);
    // The text column is wide, so the weight sits right after the position
    // rather than at the column's far end beside the meters.
    REQUIRE(lv_area_get_width(&da) > 3 * (lv_area_get_width(&pa) + lv_area_get_width(&wa)) / 2);
    CHECK(wa.x1 > pa.x2);
    CHECK(wa.x1 - pa.x2 <= 16);
    CHECK(wa.x2 < da.x2 - lv_area_get_width(&wa));
    CHECK(wa.y1 < pa.y2);
    CHECK(wa.y2 > pa.y1);
}


TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS panel: an endless-spool backup between lanes past sixteen is shown",
                 "[ams][slot_ceiling][endless_spool]") {
    constexpr int kLanes = 20;
    build("off", kLanes);
    AmsBackend* backend = AmsState::instance().get_backend();
    REQUIRE(backend != nullptr);
    REQUIRE(backend->get_endless_spool_capabilities().available());
    REQUIRE(backend->set_endless_spool_backup(kLanes - 2, kLanes - 1).success());

    AmsPanelTestAccess::update_endless_arrows(*panel_);
    lv_obj_t* arrows = lv_obj_find_by_name(panel_obj_, "endless_arrows");
    REQUIRE(arrows != nullptr);
    CHECK_FALSE(lv_obj_has_flag(arrows, LV_OBJ_FLAG_HIDDEN));
}
