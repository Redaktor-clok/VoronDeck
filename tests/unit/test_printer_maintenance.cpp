// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_maintenance.h"
#include "ui_settings_printer_maintenance.h"
#include "ui_nav.h"
#include "ui_update_queue.h"
#include "app_globals.h"
#include "printer_state.h"
#include "moonraker_client_mock.h"
#include "../lvgl_ui_test_fixture.h"
#include "../catch_amalgamated.hpp"

using namespace helix;
TEST_CASE("Recovery updater distinguishes staged config from a loaded macro", "[printer-maintenance][recovery-control]") {
    json component={{"version","legacy"},{"remote_version","1.0.0-beta.1+toggle-1"},
                    {"is_valid",true},{"vorondeck_recovery",true},
                    {"restart_required",true},{"update_available",false}};
    auto rows=maintenance::parse_components({{"version_info",{{"vorondeck_recovery",component}}}});
    REQUIRE(rows.size()==1);
    CHECK_FALSE(rows[0].can_update);
    CHECK(rows[0].note=="Recovery updated. Restart Klipper to activate it.");
    component["update_available"]=true;
    rows=maintenance::parse_components({{"version_info",{{"vorondeck_recovery",component}}}});
    CHECK(rows[0].can_update);
    component["vorondeck_recovery"]=false;
    rows=maintenance::parse_components({{"version_info",{{"vorondeck_recovery",component}}}});
    CHECK_FALSE(rows[0].can_update);
}
TEST_CASE("Maintenance reset endpoints have separate scopes", "[printer-maintenance]") {
    auto history=maintenance::reset_request(false), totals=maintenance::reset_request(true);
    CHECK(history.method=="server.history.delete_job");
    CHECK(history.params==json{{"all",true}});
    CHECK(totals.method=="server.history.reset_totals");
    CHECK(totals.params.empty());
    for (auto name : {"klipper","moonraker","system"}) {
        auto update=maintenance::update_request(name);
        CHECK(update.method==std::string("machine.update.")+name);
        CHECK(update.params.empty());
    }
    auto custom=maintenance::update_request("my-component");
    CHECK(custom.method=="machine.update.client");
    CHECK(custom.params==json{{"name","my-component"}});
}
TEST_CASE("Update inventory preserves custom components and blocks unsafe or deferred updates", "[printer-maintenance]") {
    json data={{"version_info",{
        {"klipper",{{"version","a"},{"remote_version","b"},{"is_valid",true}}},
        {"custom",{{"version","a"},{"remote_version","a"},{"commits_behind",json::array({"x"})}}},
        {"dirty",{{"version","a"},{"remote_version","b"},{"is_dirty",true}}},
        {"invalid",{{"version","a"},{"remote_version","b"},{"is_valid",false}}},
        {"corrupt",{{"version","a"},{"remote_version","b"},{"corrupt",true}}},
        {"current",{{"version","a"},{"remote_version","a"}}},
        {"unknown",json::object()},
        {"system",{{"package_count",2}}},
        {"vorondeck",{{"version","a"},{"remote_version","b"}}},
        {"helixscreen",{{"version","a"},{"remote_version","b"}}}
    }}};
    auto rows=maintenance::parse_components(data);
    REQUIRE(rows.size()==10);
    for (const auto& r:rows)
        CHECK(r.can_update==(r.name=="klipper" || r.name=="custom" || r.name=="system"));
    CHECK(maintenance::parse_components(nullptr).empty());
    CHECK(maintenance::parse_components({{"version_info",false}}).empty());
}
namespace {
struct MaintenanceFixture : LVGLUITestFixture {
    MoonrakerClientMock client;
    IMoonrakerClient* previous=nullptr;
    MaintenanceFixture() {
        client.connect("ws://mock/websocket",[] {},[] {});
        previous=get_moonraker_client(); set_moonraker_client(&client);
        lv_subject_set_int(get_printer_state().print_state().get_job_holds_machine_subject(),0);
        client.mock_update_components()={{"klipper",{{"version","a"},{"remote_version","b"}}}};
    }
    ~MaintenanceFixture() override {
        settings::get_printer_maintenance_overlay().close(); drain();
        lv_subject_set_int(get_printer_state().print_state().get_job_holds_machine_subject(),0);
        set_moonraker_client(previous);
        client.stop_temperature_simulation(); client.disconnect();
    }
    void drain() { for(int i=0;i<20;++i) { ui::UpdateQueue::instance().drain(); process_lvgl(10); } }
    void open(bool updates) { settings::get_printer_maintenance_overlay().open(test_screen(),updates); drain(); }
    void answer(const char* name) {
        auto* button=lv_obj_find_by_name(test_screen(),name); REQUIRE(button);
        lv_obj_send_event(button,LV_EVENT_CLICKED,nullptr); drain();
    }
};
}
TEST_CASE_METHOD(MaintenanceFixture,"Reset requires confirmation and rechecks print ownership", "[printer-maintenance]") {
    open(false);
    auto& overlay=settings::get_printer_maintenance_overlay();
    overlay.confirm_reset(false); drain();
    CHECK(client.call_count("server.history.delete_job")==0);
    answer("btn_secondary");
    CHECK_FALSE(client.mock_history_cleared());
    overlay.confirm_reset(false); drain();
    lv_subject_set_int(get_printer_state().print_state().get_job_holds_machine_subject(),1);
    answer("btn_primary");
    CHECK_FALSE(client.mock_history_cleared());
    CHECK(client.call_count("server.history.delete_job")==0);
}
TEST_CASE_METHOD(MaintenanceFixture,"History and totals reset independently without config reset", "[printer-maintenance]") {
    open(false);
    auto& overlay=settings::get_printer_maintenance_overlay();
    overlay.confirm_reset(false);drain();answer("btn_primary");
    CHECK(client.mock_history_cleared()); CHECK_FALSE(client.mock_totals_reset());
    overlay.confirm_reset(true);drain();answer("btn_primary");
    CHECK(client.mock_totals_reset());
    CHECK(client.call_count("server.history.delete_job")==1);
    CHECK(client.call_count("server.history.reset_totals")==1);
}
TEST_CASE_METHOD(MaintenanceFixture,"Component update rechecks server busy and sends only selected updater", "[printer-maintenance]") {
    open(true);
    auto& overlay=settings::get_printer_maintenance_overlay();
    overlay.confirm_update(0);drain();
    CHECK(client.call_count("machine.update.klipper")==0);
    client.set_update_manager_busy(true);answer("btn_primary");
    CHECK(client.call_count("machine.update.klipper")==0);
    client.set_update_manager_busy(false);overlay.refresh();drain();
    overlay.confirm_update(0);drain();answer("btn_primary");
    CHECK(client.call_count("machine.update.klipper")==1);
    CHECK(client.mock_update_components()["klipper"]["version"]=="b");
    CHECK(client.call_count("machine.update.full")==0);
}
TEST_CASE_METHOD(MaintenanceFixture,"Failed maintenance request does not claim success or mutate data", "[printer-maintenance]") {
    open(false);
    client.fail_next("server.history.reset_totals");
    auto& overlay=settings::get_printer_maintenance_overlay();
    overlay.confirm_reset(true);drain();answer("btn_primary");
    CHECK_FALSE(client.mock_totals_reset());
    auto* status=lv_xml_get_subject(nullptr,"maintenance_status"); REQUIRE(status);
    CHECK(std::string(lv_subject_get_string(status)).find("Request not confirmed")!=std::string::npos);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr,"maintenance_busy"))==0);
}
TEST_CASE_METHOD(MaintenanceFixture,"Confirmed update is blocked if printing starts or connection disappears", "[printer-maintenance]") {
    open(true);
    auto& overlay=settings::get_printer_maintenance_overlay();
    overlay.confirm_update(0);drain();
    lv_subject_set_int(get_printer_state().print_state().get_job_holds_machine_subject(),1);
    answer("btn_primary");
    CHECK(client.call_count("machine.update.klipper")==0);
    lv_subject_set_int(get_printer_state().print_state().get_job_holds_machine_subject(),0);
    overlay.confirm_reset(true);drain();client.disconnect();answer("btn_primary");
    CHECK(client.call_count("server.history.reset_totals")==0);
}

TEST_CASE("VoronDeck package updater is enabled only for its verified provider", "[printer-maintenance]") {
    auto data = json{{"version_info", {{"vorondeck", {{"version", "1.0.0-beta.1+abc"},
        {"remote_version", "1.0.0-beta.1+def"}, {"is_valid", true}, {"vorondeck_package", true}}}}}};
    auto rows = maintenance::parse_components(data);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].can_update);
    data["version_info"]["vorondeck"]["is_valid"] = false;
    CHECK_FALSE(maintenance::parse_components(data)[0].can_update);
    data["version_info"]["vorondeck"]["is_valid"] = true;
    data["version_info"]["vorondeck"].erase("vorondeck_package");
    CHECK_FALSE(maintenance::parse_components(data)[0].can_update);
}
