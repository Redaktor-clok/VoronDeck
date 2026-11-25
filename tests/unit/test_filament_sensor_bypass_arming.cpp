// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_sensor_bypass_arming.cpp
 * @brief Bypass⇄runout-sensor arming policy (FilamentSensorManager) and the
 *        CFS external-spool lane_data publish.
 *
 * Run with: ./build/bin/helix-tests "[bypass-arming]"
 *
 * Two halves of the same bypass story:
 *  1. Arming — when bypass engages, RUNOUT-role sensors the firmware holds
 *     disabled are armed via SET_FILAMENT_SENSOR and restored on disengage.
 *     Policy lives entirely in FilamentSensorManager (sensor abstraction
 *     layer); AmsState only notifies the transition.
 *  2. Slicer sync — the external spool is published as the lane one past the
 *     last CFS bay in the shared lane_data namespace so OrcaSlicer can select
 *     it. Capability dispatch via AmsBackend::publish_external_spool_lane;
 *     only CFS implements it today.
 */

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/filament_sensor_manager_test_access.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "ams_backend_afc.h"
#include "ams_types.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/backend_user_edit.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace helix {

// Friend shim reaching the manager's private state — same idiom as
// RunoutScopeTestAccess in test_runout_empty_lane_scope.cpp (per-TU class to
// avoid an ODR clash).
class BypassArmingTestAccess {
  public:
    static void reset(FilamentSensorManager& mgr) {
        mgr.set_sync_mode(true);
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex());
        mgr.entries_locked().clear();
        mgr.readings_.clear();
        mgr.lanes_.clear();
        mgr.known_.clear();
        mgr.bypass_armed_.clear();
        mgr.master_enabled_ = true;
        mgr.initial_status_received_ = false;
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }
};
} // namespace helix

namespace {
/// Fixture with the mock pair + API; the gcode wire is the client mock's
/// gcode_script_history().
class BypassArmingFixture : public HelixTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    FilamentSensorManager& mgr = FilamentSensorManager::instance();

    BypassArmingFixture() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        BypassArmingTestAccess::reset(mgr);
        mgr.set_moonraker_api(api.get());
    }

    ~BypassArmingFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        BypassArmingTestAccess::reset(mgr);
        mgr.set_moonraker_api(nullptr);
    }

    /// Discover one switch sensor and give it a first status frame.
    void seed_toolhead_sensor(bool firmware_enabled) {
        discover_filament_sensors(mgr, {"filament_switch_sensor filament_sensor"});
        mgr.set_role("filament_switch_sensor filament_sensor",
                     static_cast<uint8_t>(FilamentSensorRole::RUNOUT));
        mgr.update_from_status(
            json{{"filament_switch_sensor filament_sensor",
                  {{"filament_detected", true}, {"enabled", firmware_enabled}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    std::vector<std::string> gcode_sent() {
        return client.gcode_script_history();
    }
};
} // namespace

TEST_CASE("bypass arming: engages firmware-disabled runout sensor with bare name",
          "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();

    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    // SET_FILAMENT_SENSOR wants the bare name (post-section-prefix), which is
    // FilamentSensorConfig::sensor_name — the same form Creality's macros use.
    CHECK(sent[0] == "SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=1");
    CHECK(FilamentSensorManagerTestAccess::has_bypass_armed_sensors(fx.mgr));
}

TEST_CASE("bypass arming: firmware-enabled sensor is left alone", "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/true);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());
    CHECK_FALSE(FilamentSensorManagerTestAccess::has_bypass_armed_sensors(fx.mgr));
}

TEST_CASE("bypass arming: master-disabled monitoring refuses to arm", "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);
    fx.mgr.set_master_enabled(false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());
}

TEST_CASE("bypass arming: arm is idempotent, restore sends exactly one disable",
          "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fx.gcode_sent().size() == 1);

    // Second engage notification (e.g. two backends transitioning): no re-send.
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());

    // Disengage restores the pre-bypass firmware state exactly once.
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false);
    helix::ui::UpdateQueue::instance().drain();
    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=0");
    CHECK_FALSE(FilamentSensorManagerTestAccess::has_bypass_armed_sensors(fx.mgr));

    // Disengage with nothing armed is silent.
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(fx.gcode_sent().empty());
}

TEST_CASE("bypass arming: re-arm after a real firmware disable echo", "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fx.gcode_sent().size() == 1);

    // Someone else (a vendor macro) disabled the sensor again mid-bypass; the
    // next status frame reports it, and a subsequent engage edge re-arms.
    fx.mgr.update_from_status(json{{"filament_switch_sensor filament_sensor",
                                    {{"filament_detected", true}, {"enabled", false}}}});
    helix::ui::UpdateQueue::instance().drain();
    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false); // restore path: nothing held armed
    helix::ui::UpdateQueue::instance().drain();

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=1");
}

TEST_CASE("bypass arming: a wrapped SET_FILAMENT_SENSOR is bypassed for the renamed builtin",
          "[ams][bypass-arming]") {
    BypassArmingFixture fx;
    // A [gcode_macro SET_FILAMENT_SENSOR] with rename_existing saves the state
    // as a user setting; the temporary arm and restore go to the builtin.
    REQUIRE(fx.api->hardware().parse_sensor_toggle_command(
        json{{"gcode_macro set_filament_sensor",
              {{"rename_existing", "_SET_FILAMENT_SENSOR"}, {"gcode", "..."}}}}));
    fx.seed_toolhead_sensor(/*firmware_enabled=*/false);

    fx.mgr.on_bypass_active_changed(true);
    helix::ui::UpdateQueue::instance().drain();
    auto sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "_SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=1");

    fx.client.clear_gcode_script_history();
    fx.mgr.on_bypass_active_changed(false);
    helix::ui::UpdateQueue::instance().drain();
    sent = fx.gcode_sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "_SET_FILAMENT_SENSOR SENSOR=filament_sensor ENABLE=0");
}

// ---------------------------------------------------------------------------
// CFS external-spool lane_data publish (slicer sync)
// ---------------------------------------------------------------------------

namespace helix {

// Friend shim for AmsBackendAfc (declared in ams_backend_afc.h) — seeds lanes
// without start(), same shape as test_ams_backend_afc.cpp's
// AmsBackendAfcTestHelper::initialize_test_lanes_with_slots. Global scope so
// the friend declaration matches.
class AfcBypassPublishTestAccess : public AmsBackendAfc {
  public:
    explicit AfcBypassPublishTestAccess(IMoonrakerAPI* api) : AmsBackendAfc(api, nullptr) {}

    void seed_lanes(int count) {
        system_info_.units.clear();
        std::vector<std::string> names;
        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Box Turtle 1";
        unit.slot_count = count;
        unit.first_slot_global_index = 0;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i + 1));
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = i;
            slot.color_rgb = kAmsDefaultSlotColor;
            unit.slots.push_back(slot);
        }
        system_info_.units.push_back(unit);
        system_info_.total_slots = count;
        AfcTestAccess::slots(*this).initialize("Box Turtle 1", names);
        for (int i = 0; i < count; ++i) {
            auto* entry = AfcTestAccess::slots(*this).get_mut(i);
            if (entry) {
                entry->info.mapped_tool = i;
            }
        }
    }
};
} // namespace helix

namespace {} // namespace

// OrcaSlicer sends the lane as the tool, so on Fork the external lane is the
// firmware's own external slot: T4 on a one-box chain (#1464).

// With box 3 off the bus the fork's external slot is 8, box 3 bay A's key. That
// bay's record is not ours: publishing neither overwrites nor clears it (#1464).

// A mirror written before the mark existed carries no mark. When it names the
// external spool saved in settings it is ours: adopted, marked and kept current,
// so an existing user's OrcaSlicer tray does not freeze (#1464).

// A marked mirror is no bay's record where no bay is, but where its key is a
// bay the box reports, it is that bay's record: an adopted record may have been
// a real bay's before it was marked (#1464).

// A mirror kept as a bay's record becomes that bay's record outright: unmarked
// and persisted so, or the external publish would go on treating the bay's
// record as its own (#1464).

// Our mirror's key turning into a reported bay in any frame makes the record
// that bay's: unmarked there and then, so a later save of the bay is not
// written marked and the next publish does not clear it (#1464).

// The old key is cleared only where we hold our own marked mirror; a key we
// hold nothing for is no record of ours (#1464).

// When the fork's external slot moves (the top box returns), the mirror at the
// old key would stay behind as a phantom tray. It is cleared there, but only if
// the record is still ours (#1464).

// ---------------------------------------------------------------------------
// AFC + IFS external-spool lane publish (same capability, shared helper)
// ---------------------------------------------------------------------------

namespace {
/// Raw-store test for the helper's key-style contract: the outer key follows
/// the store's style while the inner 0-based `lane` field — what Orca reads —
/// is always the slot index.
struct ExternalLaneCacheDir {
    std::filesystem::path path;
    explicit ExternalLaneCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("external_lane_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ExternalLaneCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void seed_store_and_publish(helix::ams::LaneKeyStyle style, const char* expect_outer) {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};

    ExternalLaneCacheDir tmp{"style"};
    helix::ams::FilamentSlotOverrideStore store(&api, "t", style);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    SlotInfo spool;
    spool.material = "ASA";
    spool.color_rgb = 0x1A2B3C;
    spool.spoolman_id = 31;
    spool.spoolman_filament_id = 55;
    const int lane_index = 4; // e.g. T0-T3 lanes -> extern is 4
    CHECK(helix::ams::publish_external_lane(&store, lane_index, &spool, "test"));
    helix::ui::UpdateQueue::instance().drain();

    auto rec = client.mock_db_get("lane_data", expect_outer);
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4"); // inner field authoritative, 0-based string
    CHECK(rec["helix_material"] == "ASA");
    CHECK(rec.value("helix_spoolman_filament_id", 0) == 55);
}
} // namespace

TEST_CASE("external lane helper: outer key follows store style, inner lane is index",
          "[ams][bypass-arming]") {
    HelixTestFixture fx;
    // Tool style (AFC publish store, tool changers): "T4".
    seed_store_and_publish(helix::ams::LaneKeyStyle::Tool, "T4");
    // Lane style (HelixScreen filament systems): "lane5" (1-based outer).
    seed_store_and_publish(helix::ams::LaneKeyStyle::Lane, "lane5");
}

TEST_CASE("AFC external spool lane: publishes T{N} one past the last lane",
          "[ams][afc][bypass-arming]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api{client, state};

    AfcBypassPublishTestAccess backend(&api);
    backend.seed_lanes(4);
    REQUIRE(backend.get_system_info().total_slots == 4);
    REQUIRE(backend.get_system_info().supports_bypass);

    SlotInfo spool;
    spool.material = "ASA";
    spool.color_rgb = 0x1A2B3C;
    backend.publish_external_spool_lane(&spool);
    helix::ui::UpdateQueue::instance().drain();

    // AFC's own lane_data convention is T<n> since its virtual-tools
    // firmware — the extern entry rides the same style at T4.
    auto rec = client.mock_db_get("lane_data", "T4");
    REQUIRE_FALSE(rec.is_null());
    CHECK(rec["lane"] == "4");
    CHECK(rec["helix_material"] == "ASA");

    SECTION("null spool clears the lane") {
        backend.publish_external_spool_lane(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(client.mock_db_get("lane_data", "T4").is_null());
    }
}
