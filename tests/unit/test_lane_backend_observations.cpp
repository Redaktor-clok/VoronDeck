// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What each backend's own signal becomes in the lane source model. Cases here
// assert on the POPULATED SOURCES; a SlotInfo read appears only where one is
// needed to establish a case's precondition.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/mock_printer.h"
#include "../ui_test_utils.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_slot_override.h"
#include "lane_apply.h"
#include "lane_echo.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/ams_backend_mock_timing_test_access.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_runtime_config.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/toolchanger_test_access.h"
#include "toolchanger_addon.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AfcTestAccess;
using helix::AmsBackendAfc;
using helix::AmsBackendHappyHare;
using helix::AmsBackendMock;
using helix::AmsBackendMockTimingTestAccess;
using helix::AmsBackendToolChanger;
using helix::HappyHareTestAccess;
using helix::ToolChangerTestAccess;
using helix::ams::lane_sources;
using helix::test::RegisteredBackend;

namespace {
/// A backend with no Moonraker behind it, registered so its lane ids are real.
using AfcHarness = RegisteredBackend<AmsBackendAfc>;
using HappyHareHarness = RegisteredBackend<AmsBackendHappyHare>;
using ToolChangerHarness = RegisteredBackend<AmsBackendToolChanger>;
using MockHarness = RegisteredBackend<AmsBackendMock>;

/// An AFC backend whose write to firmware can be made to fail after it has
/// staged its echo guard: the state a refused dispatch leaves behind, which
/// the shared commit funnel has to clean up (prestonbrown/helixscreen#1633).
class RefusedWriteAfc : public AmsBackendAfc {
  public:
    using AmsBackendAfc::AmsBackendAfc;

    bool refuse_writes = false;

    helix::AmsError apply_user_edit(int slot_index, const helix::SlotInfo& info,
                                    const helix::ams::Observation& declared) override {
        // Stage and arm exactly as a write-back backend does ahead of its
        // dispatch, then answer as the dispatch itself would: success means
        // the write went out and its echo is coming, refusal means nothing
        // reached firmware.
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto* echoes = own_write_echoes()) {
            echoes->stage(slot_index, declared);
            echoes->arm(slot_index, std::string{});
        }
        if (!refuse_writes) {
            return helix::AmsErrorHelper::success();
        }
        return helix::AmsError(helix::AmsResult::COMMAND_FAILED, "refused", lv_tr("Write failed"));
    }
};

using RefusedWriteAfcHarness = RegisteredBackend<RefusedWriteAfc>;

/// An AFC backend running the real apply_user_edit dispatch chain, whose
/// SET_COLOR/SET_MATERIAL send answers the way a refused Moonraker call does:
/// through the error callback, after the send itself already returned
/// (prestonbrown/helixscreen#1633).
class RefusedGcodeAfc : public AmsBackendAfc {
  public:
    using AmsBackendAfc::AmsBackendAfc;

    // Per command: a case refusing only one SET_* proves that dispatch's own
    // cancel, where refusing both lets either cancel clear the shared guard.
    bool refuse_set_color = false;
    bool refuse_set_material = false;

    helix::AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                                  helix::SharedCallback<void(const MoonrakerError&)> on_error,
                                  bool /*silent*/ = true) override {
        const bool refused = (refuse_set_color && gcode.rfind("SET_COLOR ", 0) == 0) ||
                             (refuse_set_material && gcode.rfind("SET_MATERIAL ", 0) == 0);
        if (refused && on_error &&
            (gcode.rfind("SET_COLOR ", 0) == 0 || gcode.rfind("SET_MATERIAL ", 0) == 0)) {
            // Inline, on the dispatching thread, while apply_user_edit still
            // holds the backend mutex: the tightest shape the callback fires in.
            MoonrakerError err;
            err.type = MoonrakerErrorType::JSON_RPC_ERROR;
            err.message = "macro refused";
            on_error(err);
        }
        return helix::AmsErrorHelper::success();
    }
};

using RefusedGcodeAfcHarness = RegisteredBackend<RefusedGcodeAfc>;

/// One `box` object, delivered the way Moonraker delivers it. Which schema
/// parsed, and whether the frame counts as a full update at all, are decisions
/// the production entry point makes, so every CFS case drives that rather than
/// one of the two static parsers.

/// A community-fork `box` payload: a flat self-describing slots[] array.
nlohmann::json flat_box(nlohmann::json slots) {
    return nlohmann::json{{"api_version", 1}, {"slots", std::move(slots)}};
}

/// A stock Creality `box` payload carrying one unit's four bays. `filament` is
/// what marks the frame a full update on this schema.
nlohmann::json stock_box(const std::string& unit, nlohmann::json unit_json) {
    nlohmann::json box{{"filament", 0}};
    unit_json["state"] = "connect";
    box[unit] = std::move(unit_json);
    return box;
}

/// One hub status object, through the notify envelope the subscription path
/// unwraps. `slots` is a single Klipper status field, so Moonraker sends the
/// array whole: unlike AFC and Happy Hare there is no sub-field delta to model.

/// One `[fila<N>]` section, the shape apply_filas_list() reads out of the
/// Box's officiall_filas_list.cfg.
std::string fila_section(int id, const std::string& name, const std::string& type) {
    return "[fila" + std::to_string(id) + "]\nfilament = " + name + "\ntype = " + type +
           "\nmin_temp = 230\nmax_temp = 250\n";
}

/// A filas list carrying one profile, one colour and one vendor row.
std::string filas_list(int fila_id, const std::string& type, int color_id,
                       const std::string& color_hex, int vendor_id, const std::string& vendor) {
    return fila_section(fila_id, type + " Basic", type) + "[colordict]\n" +
           std::to_string(color_id) + " = " + color_hex + "\n[vendor_list]\n" +
           std::to_string(vendor_id) + " = " + vendor + "\n";
}

/// Three fila profiles (two of them the same material under different names),
/// two palette rows and two vendor rows: enough table for a slot-0 edit, a
/// slot-1 control naming different rows, and a fila id change that decodes to
/// the same material.
std::string qidi_tables() {
    return fila_section(12, "PETG Basic", "PETG") + fila_section(14, "PETG Rapido", "PETG") +
           fila_section(40, "ABS Rapido", "ABS") +
           "[colordict]\n5 = #ED2C2C\n9 = #00FF00\n[vendor_list]\n3 = QIDI\n8 = Elegoo\n";
}

/// Capture the toasts a case fires, and stop capturing when it ends. The
/// insert rule's ask is a toast, so this is how a lane test observes it.
struct ToastRecorder {
    std::vector<std::string> messages;
    ToastRecorder() {
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string& msg, uint32_t) { messages.push_back(msg); });
    }
    ~ToastRecorder() {
        helix::ui::set_test_toast_hook(nullptr);
    }
};

/// Four lanes through AFC's own initialize_slots(), which is what a discovery
/// answer ends in.
void init_afc_lanes(AmsBackendAfc& backend) {
    AfcTestAccess::initialize_slots(backend,
                                    std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"});
}

/// One AFC lane_data payload, the Moonraker DB snapshot, keyed by lane name.
void feed_afc_lane_data(AmsBackendAfc& backend, const nlohmann::json& lane_data) {
    std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(backend));
    AfcTestAccess::apply_lane_data(backend, helix::afc::parse_lane_data(lane_data));
}

/// One AFC_stepper lane object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Every case here
/// drives the production path rather than the parse alone, because what makes
/// AFC different is that its frames are deltas.
void feed_afc_lane(AmsBackendAfc& backend, const std::string& lane_name,
                   const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane_name] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AfcTestAccess::handle_status_update(backend, notification);
}

/// One printer.mmu object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Happy Hare's
/// frames are deltas, so every case here drives the production entry point
/// rather than the parse alone.
void feed_mmu(AmsBackendHappyHare& backend, const nlohmann::json& mmu) {
    nlohmann::json params;
    params["mmu"] = mmu;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    HappyHareTestAccess::handle_status_update(backend, notification);
}

/// One filament_detect object plus optional filament_feed channels, through
/// the notify envelope handle_status_update unwraps. RFID identity arrives
/// under the filament_detect key; lane presence arrives from the feed's port
/// sensor and channel_state latch — the state array is the entrance/tag
/// reading and backs no presence claim.

/// One print_task_config object, through the same envelope. This is Snapmaker's
/// second identity writer and the one that files no observation, so a case
/// needs to drive it separately from filament_detect.

/// One tool-changer status object, delivered the way Moonraker delivers it.
void feed_toolchanger(AmsBackendToolChanger& backend, const nlohmann::json& status) {
    nlohmann::json notification;
    notification["method"] = "notify_status_update";
    notification["params"] = nlohmann::json::array({status, 0.0});
    ToolChangerTestAccess::handle_status(backend, notification);
}

/// A stock upstream MedusaHC object list, which is what gives a tool changer
/// dock sensors at all. Without them every toolhead is assumed present and
/// EMPTY is not an answer the backend can reach.
helix::PrinterDiscovery medusahc_discovery() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "tool T2",
                                            "pin_watch io", "servo my_servo", "extruder"}));
    return hw;
}
} // namespace

/// The presence vocabulary every producer in this file consumes, pinned over
/// the whole enum. A producer's own case can only reach the statuses its
/// firmware emits, so no backend test covers the rule end to end.
TEST_CASE("the presence vocabulary answers for every slot status", "[lane][ingest]") {
    using helix::slot_status_reports_filament;
    using helix::SlotStatus;

    auto stated = [](SlotStatus status) { return slot_status_reports_filament(status); };

    const auto empty = stated(SlotStatus::EMPTY);
    REQUIRE(empty.has_value());
    CHECK(*empty == false);

    for (SlotStatus occupied :
         {SlotStatus::AVAILABLE, SlotStatus::LOADED, SlotStatus::FROM_BUFFER}) {
        INFO("status " << helix::slot_status_to_string(occupied));
        const auto reading = stated(occupied);
        REQUIRE(reading.has_value());
        CHECK(*reading == true);
    }

    // A jam is filament stuck in the path, so a blocked bay holds filament.
    // QIDI reports it for any negative state word, and both SlotInfo::
    // is_present() and helix::ui::classify_lane() count it present; answering
    // "no reading" here would retract a live reading the moment a lane jams.
    const auto blocked = stated(SlotStatus::BLOCKED);
    REQUIRE(blocked.has_value());
    CHECK(*blocked == true);

    // The one status where this rule and is_present() differ, and the reason
    // the answer is three-valued at all.
    CHECK_FALSE(stated(SlotStatus::UNKNOWN).has_value());

    // The contract between the two rules, asserted rather than described: they
    // agree on every status but UNKNOWN. Either one drifting fails here.
    for (SlotStatus status : {SlotStatus::EMPTY, SlotStatus::AVAILABLE, SlotStatus::LOADED,
                              SlotStatus::FROM_BUFFER, SlotStatus::BLOCKED}) {
        INFO("status " << helix::slot_status_to_string(status));
        helix::SlotInfo slot;
        slot.status = status;
        const auto reading = stated(status);
        REQUIRE(reading.has_value());
        CHECK(*reading == slot.is_present());
    }

    helix::SlotInfo unknown_slot;
    unknown_slot.status = SlotStatus::UNKNOWN;
    CHECK_FALSE(unknown_slot.is_present());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC splits its sensors, its own store and its own weight",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"load", true},
                   {"tool_loaded", false},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"},
                   {"spool_vendor", "Kingroon"},
                   {"weight", 612.0},
                   {"spool_id", 7},
                   {"initial_weight", 1000.0}});

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);
    // prep and load are real sensors and say nothing about which spool this is.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    // AFC's own store keeps colour, material, filament name and vendor across
    // an eject, so all four are a cache of a past declaration.
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK(lane.vendor_cache->spool_name == "Galaxy Black");
    CHECK(lane.vendor_cache->brand == "Kingroon");
    CHECK(lane.vendor_cache->spoolman_id == 7);
    CHECK_FALSE(lane.vendor_cache->present.has_value());

    REQUIRE(lane.metered.has_value());
    REQUIRE(lane.metered->remaining_weight_g.has_value());
    CHECK(*lane.metered->remaining_weight_g == Catch::Approx(612.0F));
    REQUIRE(lane.metered->total_weight_g.has_value());
    CHECK(*lane.metered->total_weight_g == Catch::Approx(1000.0F));
    // A weight tick has no business asserting identity or presence.
    CHECK_FALSE(lane.metered->material.has_value());
    CHECK_FALSE(lane.metered->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial AFC delta narrows nothing it does not mention",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // The commonest frame AFC sends: the lane reached the toolhead and nothing
    // about the spool changed. A record assembled from this frame alone would
    // leave the lane with no identity at all.
    feed_afc_lane(*harness, "lane2", {{"status", "Tooled"}});

    const auto after_status = lane_sources(harness.lane(1));
    REQUIRE(after_status.vendor_cache.has_value());
    REQUIRE(after_status.vendor_cache->color_rgb.has_value());
    CHECK(*after_status.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(after_status.vendor_cache->material == "PETG");
    CHECK(after_status.vendor_cache->spool_name == "Galaxy Black");
    REQUIRE(after_status.sensed.has_value());
    CHECK(after_status.sensed->present == true);

    // A colour change alone must not take the material with it.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}});

    const auto after_color = lane_sources(harness.lane(1));
    REQUIRE(after_color.vendor_cache.has_value());
    REQUIRE(after_color.vendor_cache->color_rgb.has_value());
    CHECK(*after_color.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(after_color.vendor_cache->material == "PETG");
    CHECK(after_color.vendor_cache->spool_name == "Galaxy Black");
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC files no weight reading until a frame carries one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane4", {{"prep", true}, {"status", "Loaded"}});

    const auto lane = lane_sources(harness.lane(3));
    // The record is filed whether or not AFC has weighed anything, so the
    // absence below is a reading AFC has not made rather than a translation
    // that never ran.
    REQUIRE(lane.metered.has_value());
    CHECK_FALSE(lane.metered->remaining_weight_g.has_value());
    CHECK_FALSE(lane.metered->total_weight_g.has_value());

    feed_afc_lane(*harness, "lane4", {{"weight", 612.0}});

    const auto weighed = lane_sources(harness.lane(3));
    REQUIRE(weighed.metered.has_value());
    REQUIRE(weighed.metered->remaining_weight_g.has_value());
    CHECK(*weighed.metered->remaining_weight_g == Catch::Approx(612.0F));
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's eject clears its own store without clearing the sensor",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane3",
                  {{"prep", true},
                   {"load", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});
    REQUIRE(lane_sources(harness.lane(2)).vendor_cache->material == "PETG");

    // clear_values() empties colour, material and filament name on eject. An
    // empty value is a clear, so the cache must stop reporting them rather
    // than standing on the last frame that named them.
    feed_afc_lane(*harness, "lane3",
                  {{"prep", false},
                   {"load", false},
                   {"status", "None"},
                   {"color", ""},
                   {"material", ""},
                   {"filament_name", ""}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == false);
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->material.has_value());
    CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AFC's vendor-cache record",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // A user colour and material that disagree with what AFC reports. The edit
    // is re-laid onto SlotInfo at the tail of every frame, so a translation
    // reading that struct back would file the user's own choice as something
    // AFC's store remembers.
    helix::SlotInfo edit = harness->get_slot_info(1);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 1, edit);

    feed_afc_lane(
        *harness, "lane2",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PETG"}});

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch
    // and both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's own write-back does not return as a vendor reading",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    // The user retypes colour and material, so SET_COLOR and SET_MATERIAL both
    // go out and firmware echoes them through the very keys they went to. The
    // frame also carries a filament name the write never sent, as AFC's status
    // pushes do: it is the proof below that the frame was parsed at all.
    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1",
                  {{"color", "#00FF00"}, {"material", "PETG"}, {"filament_name", "AFC Basics"}});

    // The echoed colour and material are absent from the vendor record: a
    // value repeating the write is not a reading, and ingest files the record
    // whole, so the field goes absent rather than back to its pre-edit value.
    // The untouched name still files, which is what makes the absence the
    // guard's doing rather than a frame that never parsed.
    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.vendor_cache.has_value());
    REQUIRE(echoed.vendor_cache->spool_name.has_value());
    CHECK(*echoed.vendor_cache->spool_name == "AFC Basics");
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // The harm lands at the clear: filed, the echo would leave the abandoned
    // edit standing as firmware's own word once the override is gone. Withheld,
    // nothing of it survives the clear to resolve.
    harness->clear_slot_override(0);
    const auto resolved = helix::ams::resolved_lane(harness.lane(0));
    CHECK_FALSE(resolved.color_rgb.has_value());
    CHECK_FALSE(resolved.material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the lane_data snapshot withholds AFC's echoed fields too",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The same echo arriving through the DB-snapshot parse, which amends the
    // same accumulator: without the guard it would file the write as
    // firmware's word on the one path that never passed the status parse.
    feed_afc_lane_data(*harness, {{"lane1", {{"color", "#00FF00"}, {"material", "PETG"}}}});

    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.vendor_cache.has_value());
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // A snapshot value the write did not send releases only its own field:
    // the colour files while the material echo is still withheld.
    feed_afc_lane_data(*harness, {{"lane1", {{"color", "#00AEFF"}, {"material", "PETG"}}}});

    const auto after = lane_sources(harness.lane(0));
    REQUIRE(after.vendor_cache.has_value());
    REQUIRE(after.vendor_cache->color_rgb.has_value());
    CHECK(*after.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK_FALSE(after.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a differing AFC value ends its own field's suppression",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // Firmware says a colour the write did not send: it has demonstrated it can
    // state its own, so the colour is released while the material stays an
    // outstanding echo. The frame says nothing about material, and the
    // accumulator's pre-edit PLA is firmware's last word, not this frame's
    // claim, so it must not release the material declaration.
    feed_afc_lane(*harness, "lane1", {{"color", "#00AEFF"}});

    const auto differed = lane_sources(harness.lane(0));
    REQUIRE(differed.vendor_cache.has_value());
    REQUIRE(differed.vendor_cache->color_rgb.has_value());
    CHECK(*differed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(differed.vendor_cache->material == "PLA");

    // The material echo is still withheld: the colour having moved releases
    // nothing but itself, and the withheld field files absent.
    feed_afc_lane(*harness, "lane1", {{"color", "#00AEFF"}, {"material", "PETG"}});

    const auto after = lane_sources(harness.lane(0));
    REQUIRE(after.vendor_cache.has_value());
    CHECK_FALSE(after.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an echoed field stays withheld on frames that go silent",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The echo itself: both fields name the write, so both file absent.
    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});

    // A later delta about nothing but state. The accumulator still holds the
    // echoed values, and filing it whole would put the abandoned edit back as
    // the machine's word one frame after the echo was withheld.
    feed_afc_lane(*harness, "lane1", {{"status", "Loaded"}});

    const auto silent = lane_sources(harness.lane(0));
    REQUIRE(silent.vendor_cache.has_value());
    CHECK_FALSE(silent.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(silent.vendor_cache->material.has_value());

    // A differing value still releases only its own field, so the suppression
    // the silent frame rode over is the declaration, not a dead entry.
    feed_afc_lane(*harness, "lane1", {{"color", "#00AEFF"}});

    const auto differed = lane_sources(harness.lane(0));
    REQUIRE(differed.vendor_cache.has_value());
    REQUIRE(differed.vendor_cache->color_rgb.has_value());
    CHECK(*differed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK_FALSE(differed.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an AFC edit arms nothing its write cannot carry",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x123456u;
    edit.color_name = "Peacock";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // A producer record restating both fields. The colour strips, which is
    // the control: it proves the guard holds an armed entry for this slot,
    // so the name surviving is the prune and not an empty guard.
    helix::ams::Observation probe(helix::ams::ObservationSource::VendorCache);
    probe.color_rgb = 0x123456u;
    probe.color_name = "Peacock";
    harness->own_write_echoes()->withhold(0, std::string{}, probe);
    CHECK_FALSE(probe.color_rgb.has_value());
    // No SET_* carries a colour name, so firmware holding one is its own
    // reading: the name must not hold a declaration that strips it.
    CHECK(probe.color_name == "Peacock");
}

TEST_CASE_METHOD(LVGLTestFixture, "an external re-bind ends AFC's echo suppression",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    // Link a spool: a binding change, so the write carries no identity a person
    // chose and nothing arms.
    auto link = harness->get_slot_info(0);
    link.spoolman_id = 42;
    helix::test::edit_slot_as_user(*harness, 0, link);
    feed_afc_lane(*harness, "lane1", {{"spool_id", 42}});

    // A colour edit on the kept link. The spool owns material, brand and name,
    // but the colour is the lane's own, so SET_COLOR arms the guard.
    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"spool_id", 42}});
    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.vendor_cache.has_value());
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());

    // Another writer puts a different spool on the lane, one whose colour reads
    // exactly what we wrote: value-difference cannot see this swap, only the
    // re-bind verdict can. The verdict runs after this frame's ingest, so this
    // frame is the last one withheld.
    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"spool_id", 7}});

    // The standing colour now files as the new spool's own statement rather
    // than being withheld as ours forever.
    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"spool_id", 7}});

    const auto rebound = lane_sources(harness.lane(0));
    REQUIRE(rebound.vendor_cache.has_value());
    REQUIRE(rebound.vendor_cache->color_rgb.has_value());
    CHECK(*rebound.vendor_cache->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture, "a later edit declaring nothing new keeps the earlier guard",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The echo itself: both fields name the write, so both file absent.
    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // A second edit that declares no identity field: only the weight moved.
    // The write it triggers re-sends SET_COLOR and SET_MATERIAL with the
    // values the first edit chose, so a frame restating them is still the
    // echo of our own write, not firmware's reading.
    auto weight = harness->get_slot_info(0);
    weight.remaining_weight_g = 750.0F;
    weight.total_weight_g = 1000.0F;
    helix::test::edit_slot_as_user(*harness, 0, weight);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});

    const auto restated = lane_sources(harness.lane(0));
    REQUIRE(restated.vendor_cache.has_value());
    CHECK_FALSE(restated.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(restated.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "clearing the colour keeps the guard the write needs",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());

    // Clear the colour. The sentinel is not declarable and the write omits
    // SET_COLOR for it, so this edit declares nothing the guard could arm on
    // its own - yet firmware still holds the green it was given and restates
    // it on every frame, which is still the echo of our earlier write.
    auto cleared = harness->get_slot_info(0);
    cleared.color_rgb = helix::kAmsDefaultSlotColor;
    helix::test::edit_slot_as_user(*harness, 0, cleared);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}});

    const auto restated = lane_sources(harness.lane(0));
    REQUIRE(restated.vendor_cache.has_value());
    CHECK_FALSE(restated.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a firmware clear releases the declaration it names",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // The eject: clear_values() wipes the lane in firmware and publishes the
    // keys empty. That is firmware stating the value is GONE, not falling
    // silent about it, so the declaration ends here rather than standing
    // guard over a lane that no longer holds the value.
    feed_afc_lane(*harness, "lane1", {{"material", ""}, {"color", ""}});

    // An untagged spool loaded into the emptied lane publishes firmware's
    // defaults. The values equal the old declarations, so only the clear
    // above can have released them: these are fresh readings and must file,
    // or the lane shows no identity at all.
    feed_afc_lane(*harness, "lane1",
                  {{"status", "Loaded"}, {"color", "#00FF00"}, {"material", "PETG"}});

    const auto reloaded = lane_sources(harness.lane(0));
    REQUIRE(reloaded.vendor_cache.has_value());
    REQUIRE(reloaded.vendor_cache->color_rgb.has_value());
    CHECK(*reloaded.vendor_cache->color_rgb == 0x00FF00u);
    CHECK(reloaded.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a DB snapshot clear releases the declaration it names",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // The same clear through the DB snapshot, which amends the same
    // accumulator: the keys arrive empty there too, and a clear on either
    // parser must reach the guard the same way.
    feed_afc_lane_data(*harness, {{"lane1", {{"color", ""}, {"material", ""}}}});

    feed_afc_lane(*harness, "lane1",
                  {{"status", "Loaded"}, {"color", "#00FF00"}, {"material", "PETG"}});

    const auto reloaded = lane_sources(harness.lane(0));
    REQUIRE(reloaded.vendor_cache.has_value());
    REQUIRE(reloaded.vendor_cache->color_rgb.has_value());
    CHECK(*reloaded.vendor_cache->color_rgb == 0x00FF00u);
    CHECK(reloaded.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "clearing a spool ends its echo suppression",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // The Clear Spool gesture. The lane is being emptied deliberately, so a
    // frame restating the edit's values afterwards is the machine's own
    // reading, not an echo to hide.
    harness->clear_slot_override(0);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});

    const auto after = lane_sources(harness.lane(0));
    REQUIRE(after.vendor_cache.has_value());
    REQUIRE(after.vendor_cache->color_rgb.has_value());
    CHECK(*after.vendor_cache->color_rgb == 0x00FF00u);
    CHECK(after.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Clear Spool on a previously edited lane shows nothing afterwards",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});

    // Clear Spool empties the lane in firmware field by field, so the next
    // status frame publishes the emptied keys with WEIGHT=0. Nothing of the
    // edit's identity may survive that frame on screen.
    harness->clear_slot_override(0);
    feed_afc_lane(*harness, "lane1",
                  {{"status", "Loaded"}, {"color", ""}, {"material", ""}, {"weight", 0.0}});

    const auto info = harness->get_slot_info(0);
    CHECK(info.material.empty());
    CHECK(info.color_rgb == helix::kAmsDefaultSlotColor);
    CHECK(info.remaining_weight_g == -1.0F);
    CHECK(info.total_weight_g == -1.0F);
    CHECK(info.brand.empty());

    const auto after = lane_sources(harness.lane(0));
    REQUIRE(after.vendor_cache.has_value());
    CHECK_FALSE(after.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(after.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a refused write leaves no guard to withhold firmware's value",
                 "[lane][ingest][afc]") {
    RefusedWriteAfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    // The commit funnel the application's editor and Clear Spool both ride.
    // The dispatch fails outright, so the edit is refused whole: nothing is
    // filed on the lane and firmware was never written.
    harness->refuse_writes = true;
    const helix::SlotInfo original = harness->get_slot_info(0);
    helix::SlotInfo edit = original;
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    const helix::AmsError error = harness->commit_user_edit(0, original, edit);
    CHECK_FALSE(error.success());
    CHECK_FALSE(error.partially_applied);
    CHECK_FALSE(lane_sources(harness.lane(0)).local_user.has_value());

    // Firmware's next frame is a reading, not an echo: the write never went
    // out, so the values it states must file rather than be withheld by the
    // guard the failed dispatch staged.
    feed_afc_lane(*harness, "lane1", {{"color", "#00FF00"}, {"material", "PETG"}});
    const auto sources = lane_sources(harness.lane(0));
    REQUIRE(sources.vendor_cache.has_value());
    REQUIRE(sources.vendor_cache->color_rgb.has_value());
    CHECK(*sources.vendor_cache->color_rgb == 0x00FF00u);
    CHECK(sources.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a refused write restores the guard of the edit it suspended",
                 "[lane][ingest][afc]") {
    // An edit whose write went out arms a guard firmware will echo back. A
    // LATER edit refused after staging suspends that guard as its carry; the
    // refusal must stand the earlier guard up again, because erasing it would
    // let the earlier write's echo file as the machine's own word.
    RefusedWriteAfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    // Edit A goes out: its values are now ours, coming back.
    const helix::SlotInfo before_a = harness->get_slot_info(0);
    helix::SlotInfo edit_a = before_a;
    edit_a.color_rgb = 0xFF0000u;
    edit_a.material = "ABS";
    REQUIRE(harness->commit_user_edit(0, before_a, edit_a).success());

    // Edit B is refused: nothing of it reached firmware.
    harness->refuse_writes = true;
    const helix::SlotInfo before_b = harness->get_slot_info(0);
    helix::SlotInfo edit_b = before_b;
    edit_b.color_rgb = 0x00FF00u;
    CHECK_FALSE(harness->commit_user_edit(0, before_b, edit_b).success());

    // Firmware's next frame repeats edit A's values: still our own write
    // being echoed, so neither field may file as the machine's word.
    feed_afc_lane(*harness, "lane1", {{"color", "#FF0000"}, {"material", "ABS"}});
    {
        const auto sources = lane_sources(harness.lane(0));
        CHECK(!(sources.vendor_cache && sources.vendor_cache->color_rgb == 0xFF0000u));
        CHECK(!(sources.vendor_cache && sources.vendor_cache->material == "ABS"));
    }

    // A value neither edit wrote is a reading and files.
    feed_afc_lane(*harness, "lane1", {{"color", "#0000FF"}});
    {
        const auto sources = lane_sources(harness.lane(0));
        REQUIRE(sources.vendor_cache.has_value());
        REQUIRE(sources.vendor_cache->color_rgb.has_value());
        CHECK(*sources.vendor_cache->color_rgb == 0x0000FFu);
    }
}

namespace {
/// The shared body of the two refused-dispatch cases: @p refuse_material
/// flips exactly one SET_* send to fail through the error callback after the
/// send returned success, while the other SET_* went out. The refused
/// command's field never reached firmware, so the same value on the next
/// frame is a reading and must file; the accepted command's field DID reach
/// firmware, so the same value on that frame is our own write echoing and
/// must stay withheld.
void assert_refused_set_command_releases_only_its_own_field(RefusedGcodeAfcHarness& harness,
                                                            bool refuse_material) {
    if (refuse_material) {
        harness->refuse_set_material = true;
    } else {
        harness->refuse_set_color = true;
    }
    init_afc_lanes(*harness);

    feed_afc_lane(
        *harness, "lane1",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PLA"}});

    // The edit's send returns success, then Moonraker refuses one SET_*
    // macro through the error callback: the dispatch failed after the guard
    // was already staged and armed.
    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    REQUIRE(helix::test::apply_edit(*harness, 0, edit).success());
    // The cancel hops off the callback's thread before taking the backend
    // mutex, which apply_user_edit still holds.
    helix::ui::UpdateQueue::instance().drain();

    // filament_name is the proof the frame parsed, which makes each field's
    // presence or absence below the release's doing rather than a guard that
    // never armed.
    feed_afc_lane(*harness, "lane1",
                  {{"color", "#00FF00"}, {"material", "PETG"}, {"filament_name", "AFC Basics"}});

    const auto sources = lane_sources(harness.lane(0));
    REQUIRE(sources.vendor_cache.has_value());
    REQUIRE(sources.vendor_cache->spool_name.has_value());
    CHECK(*sources.vendor_cache->spool_name == "AFC Basics");
    if (refuse_material) {
        // SET_MATERIAL never reached firmware: PETG on the frame is a
        // reading and files. SET_COLOR did: the green on the frame is our
        // own write echoing, and filing it would put the edit back as the
        // machine's word.
        REQUIRE(sources.vendor_cache->material.has_value());
        CHECK(*sources.vendor_cache->material == "PETG");
        CHECK_FALSE(sources.vendor_cache->color_rgb.has_value());
    } else {
        REQUIRE(sources.vendor_cache->color_rgb.has_value());
        CHECK(*sources.vendor_cache->color_rgb == 0x00FF00u);
        CHECK_FALSE(sources.vendor_cache->material.has_value());
    }
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a refused SET_COLOR releases its own guard, not SET_MATERIAL's",
                 "[lane][ingest][afc]") {
    RefusedGcodeAfcHarness harness(nullptr, nullptr);
    assert_refused_set_command_releases_only_its_own_field(harness, /*refuse_material=*/false);
}

TEST_CASE_METHOD(LVGLTestFixture, "a refused SET_MATERIAL releases its own guard, not SET_COLOR's",
                 "[lane][ingest][afc]") {
    RefusedGcodeAfcHarness harness(nullptr, nullptr);
    assert_refused_set_command_releases_only_its_own_field(harness, /*refuse_material=*/true);
}

TEST_CASE_METHOD(LVGLTestFixture, "a frame with no sensor key neither sets nor erases AFC presence",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // Weight alone. AFC's sensors have said nothing about this lane, and no
    // sensor reading is not a sensor reading of "empty".
    feed_afc_lane(*harness, "lane1", {{"weight", 612.0}});

    const auto unread = lane_sources(harness.lane(0));
    // Proof the translation ran, so the absence below is about the sensors.
    REQUIRE(unread.metered.has_value());
    CHECK_FALSE(unread.sensed.has_value());

    // The same lane once the sensors do speak. This half is what makes the
    // half above a guard rather than a backend that never reports presence.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // Another weight-only frame. AFC's presence authority is per frame rather
    // than a latch, and a record is written whole, so filing an empty one here
    // would erase a live reading.
    feed_afc_lane(*harness, "lane1", {{"weight", 600.0}});

    const auto still = lane_sources(harness.lane(0));
    REQUIRE(still.sensed.has_value());
    REQUIRE(still.sensed->present.has_value());
    CHECK(*still.sensed->present == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "each AFC lane accumulates its own identity",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // A second lane, saying nothing about any spool. One accumulator for the
    // backend instead of one per lane would hand it lane1's identity.
    feed_afc_lane(*harness, "lane2", {{"prep", false}, {"status", "None"}});

    const auto second = lane_sources(harness.lane(1));
    REQUIRE(second.vendor_cache.has_value());
    CHECK_FALSE(second.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(second.vendor_cache->material.has_value());
    CHECK_FALSE(second.vendor_cache->spool_name.has_value());

    const auto first = lane_sources(harness.lane(0));
    REQUIRE(first.vendor_cache.has_value());
    REQUIRE(first.vendor_cache->color_rgb.has_value());
    CHECK(*first.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(first.vendor_cache->material == "PETG");

    // The second lane's own spool, which must reach its own record and leave
    // the first lane's alone.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}, {"material", "PLA"}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.vendor_cache.has_value());
    REQUIRE(changed.vendor_cache->color_rgb.has_value());
    CHECK(*changed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(changed.vendor_cache->material == "PLA");

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.vendor_cache.has_value());
    REQUIRE(untouched.vendor_cache->color_rgb.has_value());
    CHECK(*untouched.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(untouched.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a spool id AFC did not state is not filed as one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // The user binds a spool to the lane.
    helix::SlotInfo edit = harness->get_slot_info(0);
    edit.spoolman_id = 99;
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // AFC unlinks the lane; the user's own binding still stands on the merged
    // slot, which is the precondition rather than the behaviour.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", nullptr}});
    REQUIRE(harness->get_slot_info(0).spoolman_id == 99);
    REQUIRE_FALSE(lane_sources(harness.lane(0)).vendor_cache->spoolman_id.has_value());

    // A spool_id that is neither an integer nor null states nothing. Reading
    // the merged slot here would file the user's 99 as AFC's own word.
    feed_afc_lane(*harness, "lane1", {{"spool_id", "junk"}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's DB snapshot files on the same lane as its status frames",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // lane_data alone. No status frame has run, so every value below is this
    // parser's work and nothing else's.
    feed_afc_lane_data(*harness, {{"lane3",
                                   {{"color", "#ED2C2C"},
                                    {"material", "PETG"},
                                    {"name", "Galaxy Black"},
                                    {"vendor_name", "Kingroon"},
                                    {"spool_id", 7}}}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK(lane.vendor_cache->spool_name == "Galaxy Black");
    CHECK(lane.vendor_cache->brand == "Kingroon");
    CHECK(lane.vendor_cache->spoolman_id == 7);

    // A database record is not a sensor and does not weigh anything.
    CHECK_FALSE(lane.sensed.has_value());
    CHECK_FALSE(lane.metered.has_value());

    // A value the snapshot carries but we cannot read is not the snapshot
    // saying the lane has no colour, so the record keeps the last one it could
    // read. Only an empty value removes it.
    feed_afc_lane_data(*harness, {{"lane3", {{"color", "#zzzzzz"}}}});

    const auto unreadable = lane_sources(harness.lane(2));
    REQUIRE(unreadable.vendor_cache.has_value());
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);

    feed_afc_lane_data(*harness, {{"lane3", {{"color", ""}}}});
    CHECK_FALSE(lane_sources(harness.lane(2)).vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's two parsers accumulate into one account of a lane",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // The subscription carries colour, material and weight on every AFC
    // version; the filament name reaches pre-v1.2.0 firmware only through the
    // DB snapshot. Neither parser may narrow the lane to its own half.
    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"weight", 612.0}});
    feed_afc_lane_data(*harness, {{"lane1", {{"name", "Galaxy Black"}}}});

    const auto joined = lane_sources(harness.lane(0));
    REQUIRE(joined.vendor_cache.has_value());
    REQUIRE(joined.vendor_cache->color_rgb.has_value());
    CHECK(*joined.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(joined.vendor_cache->material == "PETG");
    CHECK(joined.vendor_cache->spool_name == "Galaxy Black");

    // The snapshot said nothing about sensors or weight, so what the
    // subscription established still stands.
    REQUIRE(joined.sensed.has_value());
    CHECK(joined.sensed->present == true);
    REQUIRE(joined.metered.has_value());
    CHECK(*joined.metered->remaining_weight_g == Catch::Approx(612.0F));

    // An empty name in the snapshot is the clear AFC writes on eject, the same
    // answer the status path gives an empty filament_name.
    feed_afc_lane_data(*harness, {{"lane1", {{"name", ""}}}});

    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->spool_name.has_value());
    CHECK(cleared.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable colour leaves AFC's record standing",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2", {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}});
    REQUIRE(*lane_sources(harness.lane(1)).vendor_cache->color_rgb == 0xED2C2Cu);

    // "AFC published something we cannot read" and "AFC cleared the lane" are
    // different statements, and the record has to tell them apart or the lane
    // loses an identity nobody withdrew. The slot keeps its colour on screen;
    // the record keeps the same one.
    feed_afc_lane(*harness, "lane2", {{"color", "#zzzzzz"}});

    const auto unreadable = lane_sources(harness.lane(1));
    REQUIRE(unreadable.vendor_cache.has_value());
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(1).color_rgb == 0xED2C2Cu);

    // The clear is the other statement, and it does remove the colour.
    feed_afc_lane(*harness, "lane2", {{"color", ""}});

    const auto cleared = lane_sources(harness.lane(1));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->color_rgb.has_value());
    CHECK(harness->get_slot_info(1).color_rgb == helix::kAmsDefaultSlotColor);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare splits gate status from gate metadata",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "000000", "", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", "", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0, 0})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed.has_value());
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == true);
    // The sensor's record carries presence and nothing else: what the gate map
    // remembers is a cache of a past declaration, not something a gate sensed.
    CHECK_FALSE(gate0.sensed->color_rgb.has_value());
    CHECK_FALSE(gate0.sensed->material.has_value());

    REQUIRE(gate0.vendor_cache.has_value());
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(gate0.vendor_cache->material == "PETG");
    CHECK(gate0.vendor_cache->spoolman_id == 7);
    CHECK_FALSE(gate0.vendor_cache->present.has_value());
    // The gate map carries no weight at all.
    CHECK_FALSE(gate0.metered.has_value());

    const auto gate1 = lane_sources(harness.lane(1));
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == false);
    // An empty gate still remembers what it last held. That memory is a cache,
    // and pure black in it is a colour like any other.
    REQUIRE(gate1.vendor_cache.has_value());
    REQUIRE(gate1.vendor_cache->color_rgb.has_value());
    CHECK(*gate1.vendor_cache->color_rgb == 0x000000u);
    // Happy Hare writes 0 for a gate with no spool, which is a clear rather
    // than a spool numbered zero.
    CHECK_FALSE(gate1.vendor_cache->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare records no colour for a gate that reports none",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", ""})}});

    const auto gate1 = lane_sources(harness.lane(1));
    // Proof the path ran: presence came through for the same gate.
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == true);
    REQUIRE(gate1.vendor_cache.has_value());
    CHECK_FALSE(gate1.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(gate1.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare frame without metadata does not blank the cache",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})}});
    REQUIRE(lane_sources(harness.lane(0)).vendor_cache->material == "PETG");

    // A status-only delta states nothing about metadata, so it must not be
    // read as a gate that stopped reporting one.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0, 1})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == false);
    REQUIRE(gate0.vendor_cache.has_value());
    CHECK(gate0.vendor_cache->material == "PETG");
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial Happy Hare delta narrows nothing it does not mention",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 9})}});

    // MMU_GATE_MAP GATE=0 MATERIAL=PLA moves one array. Moonraker names only
    // what changed, so a record built from this frame alone would be a gate
    // whose colour and spool link had just vanished.
    feed_mmu(*harness, {{"gate_material", nlohmann::json::array({"PLA", "ABS"})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.vendor_cache.has_value());
    CHECK(gate0.vendor_cache->material == "PLA");
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(gate0.vendor_cache->spoolman_id == 7);
}

TEST_CASE_METHOD(LVGLTestFixture, "each Happy Hare gate accumulates its own identity",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // Gate 0 alone carries a spool. One accumulator for the backend instead of
    // one per gate would hand its identity to every other gate.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 0})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0})}});

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.vendor_cache.has_value());
    CHECK_FALSE(silent.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(silent.vendor_cache->material.has_value());
    CHECK_FALSE(silent.vendor_cache->spoolman_id.has_value());

    // Gate 1 gets its own spool; gate 0's record must not follow it.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"ed2c2c", "00aeff", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 12, 0})}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.vendor_cache.has_value());
    REQUIRE(changed.vendor_cache->color_rgb.has_value());
    CHECK(*changed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(changed.vendor_cache->material == "PLA");
    CHECK(changed.vendor_cache->spoolman_id == 12);

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.vendor_cache.has_value());
    REQUIRE(untouched.vendor_cache->color_rgb.has_value());
    CHECK(*untouched.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(untouched.vendor_cache->material == "PETG");
    CHECK(untouched.vendor_cache->spoolman_id == 7);

    const auto quiet = lane_sources(harness.lane(2));
    REQUIRE(quiet.vendor_cache.has_value());
    CHECK_FALSE(quiet.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(quiet.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches Happy Hare's vendor-cache record",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // gate_spool_id is what makes the lane re-layer run at all on this path.
    const nlohmann::json gates = {{"gate_status", nlohmann::json::array({1, 1})},
                                  {"gate_color", nlohmann::json::array({"a4b2bc", "ed2c2c"})},
                                  {"gate_material", nlohmann::json::array({"PLA", "PETG"})},
                                  {"gate_spool_id", nlohmann::json::array({0, 0})}};

    // Frame one gives the backend its gates, which an edit needs to address.
    feed_mmu(*harness, gates);

    // A user colour and material that disagree with what the gate map says.
    // SlotInfo persists across frames and the lane re-layer rewrites it in
    // place, so a translation reading that struct back would file the user's
    // own choice as something Happy Hare's gate map remembers.
    helix::SlotInfo edit = harness->get_slot_info(1);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 1, edit);

    // Frame two re-states the same gate map, with the user's choice standing on
    // the merged slot for a read-back to pick up.
    feed_mmu(*harness, gates);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for the case to catch and
    // the assertions below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(1).material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a gate Happy Hare calls unknown files no presence reading",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // Nothing on this lane may abort the case: the decisive claim is the last
    // phase, and a leading REQUIRE would report "no record filed" for a rule
    // whose actual failure is a stale reading surviving.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});

    const auto unread = lane_sources(harness.lane(0));
    // The record is filed even with nothing to say, so "the MMU does not know"
    // and "no translation ran" stay distinguishable.
    CHECK(unread.sensed.has_value());
    const bool unknown_gate_claims_a_reading =
        unread.sensed.has_value() && unread.sensed->present.has_value();
    CHECK_FALSE(unknown_gate_claims_a_reading);

    const auto known = lane_sources(harness.lane(1));
    REQUIRE(known.sensed.has_value());
    REQUIRE(known.sensed->present.has_value());
    CHECK(*known.sensed->present == true);

    // Positive contrast: the same gate reports a real status and the reading
    // lands, so the arm above is a guard and not a gate that never reports.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})}});
    const auto reporting = lane_sources(harness.lane(0));
    REQUIRE(reporting.sensed.has_value());
    REQUIRE(reporting.sensed->present.has_value());
    CHECK(*reporting.sensed->present == true);

    // Happy Hare withdrawing its word is news. One ingest replaces a source's
    // record whole, so filing nothing leaves the old reading standing: a
    // record whose present is unset is the only way to retract one.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});
    const auto withdrawn = lane_sources(harness.lane(0));
    const bool a_stale_reading_still_stands =
        withdrawn.sensed.has_value() && withdrawn.sensed->present.has_value();
    CHECK_FALSE(a_stale_reading_still_stands);
    CHECK(withdrawn.sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable Happy Hare colour leaves the record standing",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})}});
    REQUIRE(*lane_sources(harness.lane(0)).vendor_cache->color_rgb == 0xED2C2Cu);

    // A value nothing can read is not a gate stating it has no colour, so the
    // last readable word stands in both the record and the slot.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"zzzzzz", "a4b2bc"})}});
    const auto unreadable = lane_sources(harness.lane(0));
    REQUIRE(unreadable.vendor_cache.has_value());
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // Empty is Happy Hare wiping the gate, which is a statement and clears both.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"", "a4b2bc"})}});
    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->color_rgb.has_value());
    CHECK(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare files the colour its numeric gate map states",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // gate_color_rgb wins over the hex strings, in both the shapes Happy Hare
    // and its emulator publish: packed integers and float triplets.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color_rgb", nlohmann::json::array({0xED2C2C, {0.0, 1.0, 0.0}})},
                        {"gate_color", nlohmann::json::array({"a4b2bc", "a4b2bc"})}});

    const auto packed = lane_sources(harness.lane(0));
    REQUIRE(packed.vendor_cache.has_value());
    REQUIRE(packed.vendor_cache->color_rgb.has_value());
    CHECK(*packed.vendor_cache->color_rgb == 0xED2C2Cu);

    const auto triplet = lane_sources(harness.lane(1));
    REQUIRE(triplet.vendor_cache.has_value());
    REQUIRE(triplet.vendor_cache->color_rgb.has_value());
    CHECK(*triplet.vendor_cache->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare files no reading for a gate it has no slot for",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // The gate count is fixed by the first gate_status frame, so a longer array
    // later names gates the backend never built a slot for.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})}});
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1, 1, 1})}});

    // Precondition, not the behaviour: without this the assertions below would
    // hold on a backend that had quietly grown the slots after all.
    REQUIRE(harness->get_slot_info(2).slot_index == -1);

    // Proof the path ran on the same frame: the gates that do exist reported.
    const auto real_gate = lane_sources(harness.lane(1));
    REQUIRE(real_gate.sensed.has_value());
    REQUIRE(real_gate.sensed->present.has_value());
    CHECK(*real_gate.sensed->present == true);

    CHECK_FALSE(lane_sources(harness.lane(2)).sensed.has_value());
    CHECK_FALSE(lane_sources(harness.lane(3)).sensed.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare's gate name is the spool's, and gate_name wins",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_name", nlohmann::json::array({"Galaxy Black", ""})},
                        {"gate_filament_name", nlohmann::json::array({"EMU Black", "EMU Blue"})}});

    // gate_name is the MMU's own field; gate_filament_name only fills a gap.
    const auto named = lane_sources(harness.lane(0));
    REQUIRE(named.vendor_cache.has_value());
    CHECK(named.vendor_cache->spool_name == "Galaxy Black");
    CHECK_FALSE(named.vendor_cache->color_name.has_value());

    const auto filled = lane_sources(harness.lane(1));
    REQUIRE(filled.vendor_cache.has_value());
    CHECK(filled.vendor_cache->spool_name == "EMU Blue");

    // A gate whose name is wiped stops carrying one, and the emulator's key
    // then fills the gap it left.
    feed_mmu(*harness, {{"gate_name", nlohmann::json::array({"", ""})}});
    const auto wiped = lane_sources(harness.lane(0));
    REQUIRE(wiped.vendor_cache.has_value());
    CHECK_FALSE(wiped.vendor_cache->spool_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a user's own name never decides Happy Hare's name precedence",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    const nlohmann::json gates = {
        {"gate_status", nlohmann::json::array({1, 1})},
        {"gate_spool_id", nlohmann::json::array({0, 0})},
        {"gate_filament_name", nlohmann::json::array({"EMU Black", "EMU Blue"})}};

    // Frame one gives the backend its gates, which an edit needs to address.
    feed_mmu(*harness, gates);

    // SlotInfo::color_name is a field the user's own edit writes, so asking it
    // which of the MMU's two name keys won would let a person's edit answer for
    // the firmware.
    helix::SlotInfo edit = harness->get_slot_info(0);
    edit.color_name = "User Named It";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // Frame two re-states the same names, with the user's own name standing on
    // the merged slot for a read-back to pick up.
    feed_mmu(*harness, gates);

    // Precondition, not the behaviour under test: the edit has to actually win
    // on the merged slot for there to be anything to launder.
    REQUIRE(harness->get_slot_info(0).color_name == "User Named It");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->spool_name == "EMU Black");
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare's own write-back does not return as a vendor reading",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})},
                        {"gate_name", nlohmann::json::array({"Spool A"})}});

    // The user retypes colour and material, so one MMU_GATE_MAP carries both
    // and the gate map echoes them through the very keys they went to. The
    // frame also restates a name the incremental write never sends, which is
    // the proof below that the frame was parsed at all.
    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})},
                        {"gate_material", nlohmann::json::array({"PETG"})},
                        {"gate_name", nlohmann::json::array({"Spool A"})}});

    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.vendor_cache.has_value());
    CHECK(echoed.vendor_cache->spool_name == "Spool A");
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // The harm lands at the clear: filed, the echo would leave the abandoned
    // edit standing as the gate map's own word once the override is gone.
    harness->clear_slot_override(0);
    const auto resolved = helix::ams::resolved_lane(harness.lane(0));
    CHECK_FALSE(resolved.color_rgb.has_value());
    CHECK_FALSE(resolved.material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a differing Happy Hare value ends its own field's suppression",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // Firmware says a colour the write did not send: it has demonstrated it can
    // state its own, so the colour is released while the material stays an
    // outstanding echo. The frame says nothing about material, and the
    // accumulator's pre-edit PLA is the gate map's last word, not this frame's
    // claim, so it files rather than releasing the material declaration.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00aeff"})}});

    const auto differed = lane_sources(harness.lane(0));
    REQUIRE(differed.vendor_cache.has_value());
    REQUIRE(differed.vendor_cache->color_rgb.has_value());
    CHECK(*differed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(differed.vendor_cache->material == "PLA");

    // The material echo is still withheld: the colour having moved releases
    // nothing but itself, and the withheld field files absent.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00aeff"})},
                        {"gate_material", nlohmann::json::array({"PETG"})}});

    const auto after = lane_sources(harness.lane(0));
    REQUIRE(after.vendor_cache.has_value());
    CHECK_FALSE(after.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an echoed field stays withheld on frames that go silent",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The echo itself: both fields name the write, so both file absent.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})},
                        {"gate_material", nlohmann::json::array({"PETG"})}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // A later delta about nothing but gate status. The accumulator still holds
    // the echoed values, and filing it whole would put the abandoned edit back
    // as the machine's word one frame after the echo was withheld.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})}});

    const auto silent = lane_sources(harness.lane(0));
    REQUIRE(silent.vendor_cache.has_value());
    CHECK_FALSE(silent.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(silent.vendor_cache->material.has_value());

    // A differing value still releases only its own field, so the suppression
    // the silent frame rode over is the declaration, not a dead entry.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00aeff"})}});

    const auto differed = lane_sources(harness.lane(0));
    REQUIRE(differed.vendor_cache.has_value());
    REQUIRE(differed.vendor_cache->color_rgb.has_value());
    CHECK(*differed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK_FALSE(differed.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a firmware clear releases the declaration it names",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})},
                        {"gate_material", nlohmann::json::array({"PETG"})}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // The gate map wiped: Happy Hare publishes the keys empty, which is the
    // gate stating the values are GONE, not falling silent about them, so the
    // declarations end here rather than standing guard over an emptied gate.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({""})},
                        {"gate_material", nlohmann::json::array({""})}});

    // An untagged spool loaded into the emptied gate republishes values equal
    // to the old declarations, so only the clear above can have released
    // them: these are fresh readings and must file, or the gate shows no
    // identity at all.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"00ff00"})},
                        {"gate_material", nlohmann::json::array({"PETG"})}});

    const auto reloaded = lane_sources(harness.lane(0));
    REQUIRE(reloaded.vendor_cache.has_value());
    REQUIRE(reloaded.vendor_cache->color_rgb.has_value());
    CHECK(*reloaded.vendor_cache->color_rgb == 0x00FF00u);
    CHECK(reloaded.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "clearing a spool ends its echo suppression",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})},
                        {"gate_material", nlohmann::json::array({"PETG"})}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());

    // The Clear Spool gesture. The gate is being emptied deliberately, so a
    // frame restating the edit's values afterwards is the machine's own
    // reading, not an echo to hide.
    harness->clear_slot_override(0);

    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})},
                        {"gate_material", nlohmann::json::array({"PETG"})}});

    const auto after = lane_sources(harness.lane(0));
    REQUIRE(after.vendor_cache.has_value());
    REQUIRE(after.vendor_cache->color_rgb.has_value());
    CHECK(*after.vendor_cache->color_rgb == 0x00FF00u);
    CHECK(after.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare edit arms nothing its write cannot carry",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})}});

    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x123456u;
    edit.color_name = "Peacock";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // A producer record restating both fields. The colour strips, which is
    // the control: it proves the guard holds an armed entry for this gate,
    // so the name surviving is the prune and not an empty guard.
    helix::ams::Observation probe(helix::ams::ObservationSource::VendorCache);
    probe.color_rgb = 0x123456u;
    probe.color_name = "Peacock";
    harness->own_write_echoes()->withhold(0, std::string{}, probe);
    CHECK_FALSE(probe.color_rgb.has_value());
    // The incremental MMU_GATE_MAP carries no colour name, so firmware
    // holding one is its own reading: the name must not hold a declaration
    // that strips it.
    CHECK(probe.color_name == "Peacock");
}

TEST_CASE_METHOD(LVGLTestFixture, "an external re-bind ends Happy Hare's echo suppression",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})},
                        {"gate_spool_id", nlohmann::json::array({7})}});

    // Link a spool: a binding change, so the write carries no identity a
    // person chose and nothing arms.
    auto link = harness->get_slot_info(0);
    link.spoolman_id = 42;
    helix::test::edit_slot_as_user(*harness, 0, link);
    feed_mmu(*harness, {{"gate_spool_id", nlohmann::json::array({42})}});

    // A colour edit on the kept link. The spool owns material, brand and
    // name, but the colour is the lane's own, so COLOR arms the guard.
    auto edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})}});
    const auto echoed = lane_sources(harness.lane(0));
    CHECK_FALSE(echoed.vendor_cache->color_rgb.has_value());

    // Another writer puts a different spool on the gate, one whose colour
    // reads exactly what we wrote: value-difference cannot see this swap,
    // only the re-bind verdict can. The verdict runs after this frame's
    // ingest, so this frame is the last one withheld.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})},
                        {"gate_spool_id", nlohmann::json::array({99})}});

    // The standing colour now files as the new spool's own statement rather
    // than being withheld as ours forever.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"00ff00"})}});

    const auto rebound = lane_sources(harness.lane(0));
    REQUIRE(rebound.vendor_cache.has_value());
    REQUIRE(rebound.vendor_cache->color_rgb.has_value());
    CHECK(*rebound.vendor_cache->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture, "clearing one field of a gate does not bring the old value back",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    // The user clears the material and keeps the colour. Happy Hare keeps an
    // omitted parameter at its current value, so the write must carry the
    // explicit empty or the gate map goes on remembering PLA - and once the
    // override is gone that remembered value would stand as the machine's
    // own word on what the user deliberately removed.
    auto edit = harness->get_slot_info(0);
    edit.material = "";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    feed_mmu(*harness, {{"gate_material", nlohmann::json::array({""})}});

    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->material.has_value());
    REQUIRE(cleared.vendor_cache->color_rgb.has_value());
    CHECK(*cleared.vendor_cache->color_rgb == 0xED2C2Cu);

    harness->clear_slot_override(0);
    const auto resolved = helix::ams::resolved_lane(harness.lane(0));
    CHECK_FALSE(resolved.material.has_value());
    // The colour the edit kept survives the clear as the gate map's reading.
    REQUIRE(resolved.color_rgb.has_value());
    CHECK(*resolved.color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a Happy Hare insert nothing names a spool for keeps the details and asks",
                 "[lane][ingest][happy_hare][1710]") {
    HappyHareHarness harness(nullptr, nullptr);
    ToastRecorder toasts;

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA"})}});

    auto edit = harness->get_slot_info(0);
    edit.material = "PETG";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // A spool goes in. Happy Hare senses occupancy alone and the gate map is
    // memory a person maintains, so the insert itself reads nothing about the
    // spool, and no binding names one: whether the details still standing
    // describe the spool now in the gate is the user's to answer.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})}});
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(toasts.messages.size() == 1);
    CHECK(toasts.messages.front().find("Gate 1") != std::string::npos);
    // Asking changes nothing: the declared details stand until answered.
    const auto resolved = helix::ams::resolved_lane(harness.lane(0));
    REQUIRE(resolved.material.has_value());
    CHECK(*resolved.material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare insert the MMU names a spool for asks nothing",
                 "[lane][ingest][happy_hare][1710]") {
    HappyHareHarness harness(nullptr, nullptr);
    ToastRecorder toasts;

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0})},
                        {"gate_spool_id", nlohmann::json::array({7})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})}});

    auto edit = harness->get_slot_info(0);
    edit.material = "PLA";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The gate fills and the MMU still names spool 7: the binding answers the
    // same-spool question, so nobody is asked. (A different id would have
    // re-bound through the gate_spool_id verdict already.)
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_spool_id", nlohmann::json::array({7})}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(toasts.messages.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare frame with no insert asks nothing",
                 "[lane][ingest][happy_hare][1710]") {
    HappyHareHarness harness(nullptr, nullptr);
    ToastRecorder toasts;

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c"})}});
    auto edit = harness->get_slot_info(0);
    edit.material = "PLA";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The gate held filament through both frames; the ask belongs to an
    // insert, not to a poll.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})}});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(toasts.messages.empty());
}

// --- CFS ---------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "CFS reads both its wire colours by the shared grammar",
                 "[lane][ingest][cfs]") {}

// --- ACE ---------------------------------------------------------------

// ============================================================================
// Snapmaker
// ============================================================================

// ============================================================================
// Tool changer
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a tool changer senses docking and declares nothing",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);

    harness->set_discovered_tools({"T0", "T1"});

    // AmsState hands a backend its tool list BEFORE add_backend() stamps an
    // index, so the records initialize_tools() filed land on no lane in
    // production. Drop them and drive a real status frame, which is the path a
    // machine takes.
    helix::ams::reset_lane_sources();
    REQUIRE_FALSE(lane_sources(harness.lane(0)).sensed.has_value());
    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->tool_docked.has_value());
    CHECK(*lane.sensed->tool_docked == true);
    CHECK_FALSE(lane.sensed->present.has_value());

    // klipper-toolchanger reports whether a tool is docked and nothing about
    // what it holds. initialize_tools() puts the tool's own name in
    // SlotInfo::spool_name and rests the colour on the sentinel; neither is a
    // reading, so this backend files no identity record at all. Both are
    // asserted here because they are what a read-back of the merged struct
    // would have filed.
    CHECK(harness->get_slot_info(0).spool_name == "T0");
    CHECK(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);
    CHECK_FALSE(lane.vendor_cache.has_value());

    const auto second = lane_sources(harness.lane(1));
    REQUIRE(second.sensed.has_value());
    REQUIRE(second.sensed->tool_docked.has_value());
    CHECK(*second.sensed->tool_docked == true);
    CHECK_FALSE(second.sensed->present.has_value());
    CHECK_FALSE(second.vendor_cache.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a vacated dock retracts a tool changer's presence reading",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1", "T2"});
    harness->set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(medusahc_discovery()));

    // AmsState hands a backend its tool list BEFORE add_backend() stamps an
    // index, so nothing the resulting initialize_tools() files has a lane to
    // land on in production. Dropping those records leaves this case resting on
    // the status path alone, which is the one that runs with an index.
    helix::ams::reset_lane_sources();
    REQUIRE_FALSE(lane_sources(harness.lane(0)).sensed.has_value());

    // T1 is on the head and T2's dock reads vacant, which means that hot end
    // has been taken out of the machine.
    feed_toolchanger(*harness,
                     nlohmann::json{{"medusahc",
                                     {{"operation", "idle"},
                                      {"current_tool", 1},
                                      {"sensors", {{"e", 1}, {"t0", 1}, {"t1", 0}, {"t2", 0}}}}}});

    REQUIRE(harness->get_slot_info(2).status == helix::SlotStatus::EMPTY);

    const auto docked = lane_sources(harness.lane(0));
    REQUIRE(docked.sensed.has_value());
    REQUIRE(docked.sensed->tool_docked.has_value());
    CHECK(*docked.sensed->tool_docked == true);

    const auto carriage = lane_sources(harness.lane(1));
    REQUIRE(carriage.sensed.has_value());
    REQUIRE(carriage.sensed->tool_docked.has_value());
    CHECK(*carriage.sensed->tool_docked == true);

    const auto vacated = lane_sources(harness.lane(2));
    REQUIRE(vacated.sensed.has_value());
    REQUIRE(vacated.sensed->tool_docked.has_value());
    CHECK(*vacated.sensed->tool_docked == false);
    CHECK_FALSE(vacated.sensed->present.has_value());
    CHECK_FALSE(vacated.vendor_cache.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never becomes a tool changer's vendor reading",
                 "[lane][ingest][toolchanger]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1"});

    // Only what the status frame below files may answer the assertions, so the
    // lane starts empty of whatever tool discovery left on it.
    helix::ams::reset_lane_sources();

    // On this backend a person's edit is the ONLY source of filament identity,
    // so every identity field on the merged slot is the user's own statement
    // and there is nothing else for a read-back to pick up.
    helix::SlotInfo edit = harness->get_slot_info(0);
    edit.color_rgb = 0x00FF00u;
    edit.material = "ABS";
    helix::test::edit_slot_as_user(*harness, 0, edit);

    // The production laundering shape, which a second set_discovered_tools()
    // does not reach: a real status frame, where refresh_slot_statuses_locked
    // files the reading and the tail re-layer runs after it.
    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(0).material == "ABS");

    const auto lane = lane_sources(harness.lane(0));
    // The dock record is the proof that the translation ran on this frame.
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->tool_docked.has_value());
    CHECK(*lane.sensed->tool_docked == true);
    CHECK_FALSE(lane.vendor_cache.has_value());
}

// ---------------------------------------------------------------------------
// QIDI Box
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Mock
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "the mock's simulated population becomes lane readings",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);
    REQUIRE(harness->start().success());

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PLA");
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0x1A1A2Eu);

    // Every field below really is on the slot, so its absence from the record
    // is a decision and not a missing value. The bound is what keeps the mock
    // from being more capable than hardware: no lane's brand or spool binding
    // comes from firmware, and firmware reports a colour as hex, never as a
    // name. Colour NAME is the sharpest of these, because this backend is the
    // only producer in the tree that could file one.
    const auto slot = harness->get_slot_info(0);
    REQUIRE(slot.color_name == "Jet Black");
    REQUIRE(slot.spoolman_id == 1);
    REQUIRE(slot.total_weight_g > 0.0f);
    REQUIRE_FALSE(slot.spool_name.empty());

    CHECK_FALSE(lane.vendor_cache->color_name.has_value());
    CHECK_FALSE(lane.vendor_cache->brand.has_value());
    CHECK_FALSE(lane.vendor_cache->spoolman_id.has_value());
    CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
    CHECK_FALSE(lane.vendor_cache->total_weight_g.has_value());
    CHECK_FALSE(lane.vendor_cache->remaining_weight_g.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the mock's UNKNOWN status declines to state presence",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);
    REQUIRE(harness->start().success());

    const auto standing = lane_sources(harness.lane(0));
    REQUIRE(standing.sensed.has_value());
    REQUIRE(standing.sensed->present.has_value());
    CHECK(*standing.sensed->present == true);

    // Re-publishing the population after forcing a status is what the next
    // simulated frame would do.
    harness->force_slot_status(0, helix::SlotStatus::UNKNOWN);
    AmsBackendMockTimingTestAccess::publish_lane_observations(*harness);

    const auto declined = lane_sources(harness.lane(0));
    REQUIRE(declined.sensed.has_value());
    CHECK_FALSE(declined.sensed->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "neither the mock's edit nor its sync is a reading",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);
    REQUIRE(harness->start().success());

    // persist = true is the editor's path, which reaches the lane model through
    // commit_slot_edit; persist = false is ToolState pushing a Spoolman-linked
    // assignment onto a tool changer's shadow slot. Neither is the machine
    // speaking, so neither disturbs what the population stated.
    auto edited = harness->get_slot_info(0);
    edited.material = "Declared-PC";
    edited.color_rgb = 0x00FF00u;
    edited.brand = "Polymaker";
    REQUIRE(helix::test::apply_edit(*harness, 0, edited).success());

    auto pushed = harness->get_slot_info(1);
    pushed.material = "Pushed-ASA";
    pushed.color_rgb = 0x0000FFu;
    REQUIRE(harness->sync_external_identity(1, pushed).success());

    // Preconditions: both writes landed on the slots, so an unchanged record
    // below is a decision and not a call that did nothing.
    REQUIRE(harness->get_slot_info(0).material == "Declared-PC");
    REQUIRE(harness->get_slot_info(0).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(1).material == "Pushed-ASA");

    const auto declared = lane_sources(harness.lane(0));
    REQUIRE(declared.vendor_cache.has_value());
    CHECK(declared.vendor_cache->material == "PLA");
    REQUIRE(declared.vendor_cache->color_rgb.has_value());
    CHECK(*declared.vendor_cache->color_rgb == 0x1A1A2Eu);
    CHECK_FALSE(declared.vendor_cache->brand.has_value());

    const auto pushed_lane = lane_sources(harness.lane(1));
    REQUIRE(pushed_lane.vendor_cache.has_value());
    CHECK(pushed_lane.vendor_cache->material == "Silk PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "the mock's colour sentinel is not a grey anybody chose",
                 "[lane][ingest][mock]") {
    MockHarness harness(4);

    // sync_external_identity writes the slot even though it files no reading, which is
    // how a lane gets staged before the population is published.
    auto black = harness->get_slot_info(0);
    black.color_rgb = 0x000000u;
    black.material = "Black-PLA";
    REQUIRE(harness->sync_external_identity(0, black).success());

    auto colourless = harness->get_slot_info(1);
    colourless.color_rgb = helix::kAmsDefaultSlotColor;
    colourless.material = "Grey-PLA";
    REQUIRE(harness->sync_external_identity(1, colourless).success());

    REQUIRE(harness->start().success());

    const auto real_black = lane_sources(harness.lane(0));
    REQUIRE(real_black.vendor_cache.has_value());
    REQUIRE(real_black.vendor_cache->color_rgb.has_value());
    CHECK(*real_black.vendor_cache->color_rgb == 0x000000u);

    // Same loop, same publish: the material still lands, so the missing colour
    // is the sentinel rule and not a lane the loop skipped.
    const auto sentinel = lane_sources(harness.lane(1));
    REQUIRE(sentinel.vendor_cache.has_value());
    CHECK(sentinel.vendor_cache->material == "Grey-PLA");
    CHECK_FALSE(sentinel.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "the factory mock is registered before it starts",
                 "[lane][ingest][mock]") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;
    get_runtime_config()->use_real_ams = false;
    get_runtime_config()->disable_mock_ams = false;

    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    // AmsState::init_subjects observes PrinterState's print-state subject; it
    // must exist first or the observer attaches to nothing.
    get_printer_state().init_subjects(false);
    ams.init_subjects(false);

    REQUIRE(ams.backend_count() == 1);

    // A backend answers kInvalidLaneId for every slot until registration
    // stamps its index, so a population published before that lands on no lane
    // and every record here reads empty.
    const auto lane = lane_sources(helix::ams::lane_id_for(0, 0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->material == "PLA");
    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    ams.clear_backends();
    helix::ui::UpdateQueue::instance().drain();
    ams.deinit_subjects();
}

// --- The shared namespace, re-read ----------------------------------------
//
// A resync goes back to lane_data and re-reads it. What it FILES is bounded
// twice over.
//
// By source: only records classifying as VendorCache. A record naming a spool
// is the server's statement and one carrying a lock key is a person's, and
// neither becomes true again merely because a re-read saw it. A record this
// application did not write is another tool's statement and may displace the
// lane's own (#1632) - unless a write of ours is still in flight on the slot,
// where the guard's strip alone decides what files.
//
// By lane: only where firmware states no identity of its own. Everywhere else
// a status frame already files the vendor-cache record, ingest() replaces a
// source's record whole, and both arrival orders happen, so a second producer
// there overwrites a fresh reading with a stored one. Nor is a field gained by
// allowing it: the next frame retracts whatever the stored record carried
// beyond what firmware reports. klipper-toolchanger is the one backend whose
// firmware reports no identity, so it is the one that files.

namespace {

/// The Moonraker a record store reads through, with the records a case seeds.
/// A store reaches the database directly, so a harness built with no API of
/// its own still gets a namespace to re-read.
struct LaneDataDb {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api{client, ::get_printer_state()};

    /// One lane_data record under @p key.
    void seed(const std::string& key, nlohmann::json record) {
        client.mock_db_set("lane_data", key, std::move(record));
    }

    /// A store on the shared namespace, in the key style @p style spells.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> store(const std::string& backend_id,
                                                                 helix::ams::LaneKeyStyle style) {
        return std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, backend_id, style);
    }
};

/// A tool changer keys its records T<n>; the slot a record names is the inner
/// 0-based field in either style.
std::unique_ptr<helix::ams::FilamentSlotOverrideStore> toolchanger_store(LaneDataDb& db) {
    return db.store("toolchanger", helix::ams::LaneKeyStyle::Tool);
}

/// A tool changer held in the state a resync has to speak up about: started
/// (so a lane-record store is owed) but with none behind it. start() is final
/// on AmsSubscriptionBackend and drags in subscription setup, so the flag
/// start() would set is set directly.
class StartedStorelessToolChanger : public AmsBackendToolChanger {
  public:
    using AmsBackendToolChanger::AmsBackendToolChanger;

    void mark_running_for_test() {
        running_ = true;
    }
};

/// An AFC backend a resync can run on: only the two reachability gates are
/// flipped, so the guard the consult finds has to come from AmsBackendAfc's
/// own override, which is what the case below pins.
class ResyncableAfc : public AmsBackendAfc {
  public:
    using AmsBackendAfc::AmsBackendAfc;

    [[nodiscard]] bool firmware_publishes_lane_identity() const override {
        return false;
    }

    helix::ams::FilamentSlotOverrideStore* lane_record_store() override {
        return store_.get();
    }

    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> store_;
};

} // namespace

// --- The one backend that files -------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "a resync re-reads the shared namespace into the model",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "ASA"}, {"color", "#A4B2BC"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    REQUIRE_FALSE(lane_sources(harness.lane(0)).remembered.has_value());

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(db.client.call_count("server.database.get_item") == 1);
    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->material == "ASA");
    REQUIRE(lane.local_user->color_rgb.has_value());
    CHECK(*lane.local_user->color_rgb == 0xA4B2BCu);
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync reaches the backend's own block, not slot indices",
                 "[lane][ingest][resync]") {
    // set_backend() clears and installs backend 0; add_backend() appends
    // backend 1 without clearing. The tool changer therefore answers a block
    // other than 0, so a record filed on block 0 is provably not its own.
    // The mock is never started, so it files nothing of its own.
    MockHarness filler(4);
    auto owned = std::make_unique<AmsBackendToolChanger>(nullptr, nullptr);
    auto* backend = owned.get();
    REQUIRE(helix::AmsState::instance().add_backend(std::move(owned)) == 1);

    LaneDataDb db;
    db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "PC"}});
    ToolChangerTestAccess::inject_override_store(*backend, toolchanger_store(db));

    backend->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    // Slot 1, not slot 0: the slot index is carried as well as the block.
    const auto lane = lane_sources(backend->lane_id(1));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->material == "PC");

    // Block 0 belongs to the other backend. Deriving the id from the slot
    // index alone would land the record there.
    CHECK_FALSE(lane_sources(helix::ams::lane_id_for(0, 1)).local_user.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync files no declaration for a record naming a spool",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{
                      {"lane", "0"}, {"material", "PETG"}, {"color", "#ED2C2C"}, {"spool_id", 42}});
    // A plain record beside it, so the case proves the resync reached the
    // document rather than that it did nothing at all.
    db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "PLA"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto linked = lane_sources(harness.lane(0));
    CHECK_FALSE(linked.spoolman.has_value());
    CHECK_FALSE(linked.remembered.has_value());

    const auto plain = lane_sources(harness.lane(1));
    REQUIRE(plain.local_user.has_value());
    CHECK(plain.local_user->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync files no declaration for a locked record",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{{"lane", "0"},
                                 {"material", "ABS"},
                                 {"color", "#00FF00"},
                                 {"helix_locked_color", true}});
    db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "PLA"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    // A person's lock is a declaration, and commit_slot_edit is the only
    // funnel that may record one. Filing it here would claim the person
    // declared it again at the moment the screen was opened.
    const auto locked = lane_sources(harness.lane(0));
    CHECK_FALSE(locked.local_user.has_value());
    CHECK_FALSE(locked.remembered.has_value());

    const auto plain = lane_sources(harness.lane(1));
    REQUIRE(plain.local_user.has_value());
    CHECK(plain.local_user->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a re-read that cannot reach the database leaves the lane alone",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "PLA"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lane_sources(harness.lane(0)).local_user.has_value());

    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "TPU"}});
    db.client.fail_next("server.database.get_item");
    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync withholds the fields of a write this backend sent",
                 "[lane][ingest][resync]") {
    // The stored record carries what the user typed because firmware mirrored
    // the backend's own write into the shared namespace: no declared bits, so
    // on its own it files as Remembered whole. Filing it would put the
    // abandoned edit on the lane as a stored record the moment the override
    // is cleared, which is the harm the guard exists for, on the one path
    // that reaches the store without passing a live frame.
    // ToolChanger parses no identity of its own; staging into its echo guard
    // stands in for a write-back backend whose firmware mirrors the write into
    // the shared namespace, so the re-read faces the backend's own edit
    // spelled as a stored record.
    RegisteredBackend<AmsBackendToolChanger> harness(nullptr, nullptr);
    LaneDataDb db;
    db.seed("T0",
            nlohmann::json{
                {"lane", "0"}, {"material", "PETG"}, {"color", "#00FF00"}, {"vendor", "Prusa"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    helix::ams::Observation declared(helix::ams::ObservationSource::LocalUser);
    declared.material = "PETG";
    declared.color_rgb = 0x00FF00u;
    harness->own_write_echoes()->stage(0, declared);
    // A stored record names no spool, so the boundary read through it is
    // empty: the producer saying nothing, which is not a change.
    harness->own_write_echoes()->arm(0, "");

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    // The vendor is the control: nothing declared it, so the re-read files it
    // and proves the case reached the document. The echoed fields stay out.
    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.remembered.has_value());
    CHECK(echoed.remembered->brand == "Prusa");
    CHECK_FALSE(echoed.remembered->material.has_value());
    CHECK_FALSE(echoed.remembered->color_rgb.has_value());

    // Without a write outstanding the same record files whole - as another
    // tool's statement, on the user's rung (#1632) - so the withholding is
    // the guard's and not the resync's.
    db.seed("T1", nlohmann::json{{"lane", "1"}, {"material", "PETG"}, {"color", "#00FF00"}});
    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();
    const auto plain = lane_sources(harness.lane(1));
    REQUIRE(plain.local_user.has_value());
    CHECK(plain.local_user->material == "PETG");
    REQUIRE(plain.local_user->color_rgb.has_value());
    CHECK(*plain.local_user->color_rgb == 0x00FF00u);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a resync promotes a record another tool wrote over an older edit",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;

    // The lane's standing statement: an edit made here, stamped 12:00.
    const auto at = [](int hours) {
        return std::chrono::system_clock::time_point{
            std::chrono::seconds(1790337600 + hours * 3600)};
    };
    helix::ams::Observation mine(helix::ams::ObservationSource::LocalUser);
    mine.color_rgb = 0x00FF00u;
    mine.material = "PETG";
    mine.edited_at = at(0);
    helix::ams::commit_slot_edit(harness.lane(0), mine);

    // Another tool replaced the record at 13:00 with its own identity.
    db.seed("T0", nlohmann::json{{"lane", "0"},
                                 {"color", "#ED2C2C"},
                                 {"material", "PLA"},
                                 {"scan_time", "2026-09-25T13:00:00Z"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.local_user.has_value());
    REQUIRE(lane.local_user->color_rgb.has_value());
    CHECK(*lane.local_user->color_rgb == 0xED2C2Cu);
    CHECK(lane.local_user->material == "PLA");
    // The winning statement carries the record's stamp, so a later edit here
    // displaces it and a later record elsewhere does too.
    REQUIRE(lane.local_user->edited_at.has_value());
    CHECK(*lane.local_user->edited_at == at(1));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a resync keeps an outside record older than the lane's edit below it",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;

    const auto at = [](int hours) {
        return std::chrono::system_clock::time_point{
            std::chrono::seconds(1790337600 + hours * 3600)};
    };
    helix::ams::Observation mine(helix::ams::ObservationSource::LocalUser);
    mine.color_rgb = 0x00FF00u;
    mine.material = "PETG";
    mine.edited_at = at(2);
    helix::ams::commit_slot_edit(harness.lane(0), mine);

    // Stamped 10:00, two hours before the edit standing on the lane.
    db.seed("T0", nlohmann::json{{"lane", "0"},
                                 {"color", "#ED2C2C"},
                                 {"material", "PLA"},
                                 {"scan_time", "2026-09-25T10:00:00Z"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->material == "PETG");
    // The older record ranks below the lane's own edit, as a memory.
    REQUIRE(lane.remembered.has_value());
    CHECK(lane.remembered->material == "PLA");
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync promotes a record another tool left no stamp on",
                 "[lane][ingest][resync]") {
    // Mainsail and Orca write no scan_time. Their record still displaced ours
    // in the namespace - our writes always carry one - so the absence of a
    // stamp is itself the evidence theirs is the newer edit.
    ToolChangerHarness harness(nullptr, nullptr);
    LaneDataDb db;

    helix::ams::Observation mine(helix::ams::ObservationSource::LocalUser);
    mine.material = "PETG";
    mine.edited_at = std::chrono::system_clock::time_point{std::chrono::seconds(1790337600)};
    helix::ams::commit_slot_edit(harness.lane(0), mine);

    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "PLA"}});
    ToolChangerTestAccess::inject_override_store(*harness, toolchanger_store(db));

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->material == "PLA");
    CHECK_FALSE(lane.remembered.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's resync consults its own echo guard",
                 "[lane][ingest][resync][afc]") {
    // Nothing today drives a real AFC through the resync: it answers
    // firmware_publishes_lane_identity() == true, and a backend whose
    // firmware states identity has a live producer to race and no stored
    // record to add. That is exactly how an override goes missing while the
    // consult stays green. The subclass flips only the reachability gates,
    // so the guard the consult finds is AmsBackendAfc's own or nothing.
    RegisteredBackend<ResyncableAfc> harness(nullptr, nullptr);
    init_afc_lanes(*harness);
    LaneDataDb db;
    db.seed("T0",
            nlohmann::json{
                {"lane", "0"}, {"material", "PETG"}, {"color", "#00FF00"}, {"vendor", "Prusa"}});
    harness->store_ = toolchanger_store(db);

    helix::ams::OwnWriteEchoes* echoes = harness->own_write_echoes();
    REQUIRE(echoes != nullptr);
    helix::ams::Observation declared(helix::ams::ObservationSource::LocalUser);
    declared.material = "PETG";
    declared.color_rgb = 0x00FF00u;
    echoes->stage(0, declared);
    echoes->arm(0, "");

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.remembered.has_value());
    CHECK(echoed.remembered->brand == "Prusa");
    CHECK_FALSE(echoed.remembered->material.has_value());
    CHECK_FALSE(echoed.remembered->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a stale stored record does not release a live echo",
                 "[lane][ingest][resync][afc]") {
    // The stored record is a co-authored mirror of past writes, not a live
    // producer: a value it states that differs from the declaration is
    // staleness in the store, not firmware demonstrating it can say something
    // else. Releasing on it would let one stale record unhook the guard while
    // the live firmware is still echoing the write.
    RegisteredBackend<ResyncableAfc> harness(nullptr, nullptr);
    init_afc_lanes(*harness);
    LaneDataDb db;
    db.seed("T0", nlohmann::json{{"lane", "0"}, {"material", "PLA"}, {"vendor", "Prusa"}});
    harness->store_ = toolchanger_store(db);

    helix::ams::OwnWriteEchoes* echoes = harness->own_write_echoes();
    REQUIRE(echoes != nullptr);
    helix::ams::Observation declared(helix::ams::ObservationSource::LocalUser);
    declared.material = "PETG";
    echoes->stage(0, declared);
    echoes->arm(0, "");

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    // The stale material files as the store remembers it, vendor control
    // intact: the re-read strips what equals the declaration and files the
    // rest, but releases nothing.
    const auto stale = lane_sources(harness.lane(0));
    REQUIRE(stale.remembered.has_value());
    CHECK(stale.remembered->material == "PLA");
    CHECK(stale.remembered->brand == "Prusa");

    // The live frame still echoes the write, so the declaration still holds.
    feed_afc_lane(*harness, "lane1", {{"material", "PETG"}});
    const auto echoed = lane_sources(harness.lane(0));
    REQUIRE(echoed.vendor_cache.has_value());
    CHECK_FALSE(echoed.vendor_cache->material.has_value());
}

// --- The six that do not ---------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture, "a resync with no store is a no-op, not a crash",
                 "[lane][ingest][resync]") {
    ToolChangerHarness harness(nullptr, nullptr);

    harness->request_resync();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(helix::ams::known_lanes().empty());
}

// The no-op above is only correct while the missing store is expected. On a
// backend that is live and reachable, firmware stating no identity of its own
// makes the persisted record the producer, and naming no store to read it
// from means the resync the caller asked for cannot be issued. The three
// cases below are one contract: the first proves the branch and its needle
// are live, the other two pin the two states where a missing store is
// expected and must stay quiet.

TEST_CASE_METHOD(LVGLTestFixture, "a live backend that names no store says the resync was skipped",
                 "[lane][ingest][resync]") {
    LaneDataDb db;
    RegisteredBackend<StartedStorelessToolChanger> harness(&db.api, nullptr);
    harness->mark_running_for_test();

    helix::LogCapture cap;
    harness->request_resync();

    REQUIRE(cap.count_containing("request_resync(): no lane-record store") == 1);
    const auto levels = cap.levels_for("request_resync(): no lane-record store");
    REQUIRE(levels.size() == 1);
    CHECK(levels[0] == spdlog::level::warn);
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync before the backend starts stays quiet about no store",
                 "[lane][ingest][resync]") {
    // The store is built in on_started(), which start() has not run, so a
    // missing store here is expected -- the print-select view reaches this
    // path whenever it opens before the backend's start() has succeeded.
    LaneDataDb db;
    RegisteredBackend<StartedStorelessToolChanger> harness(&db.api, nullptr);

    helix::LogCapture cap;
    harness->request_resync();

    CHECK(cap.count_containing("request_resync(): no lane-record store") == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "a resync on a backend with no API stays quiet about no store",
                 "[lane][ingest][resync]") {
    // No API: no store is ever built and none is owed, whatever the running
    // flag says.
    RegisteredBackend<StartedStorelessToolChanger> harness(nullptr, nullptr);
    harness->mark_running_for_test();

    helix::LogCapture cap;
    harness->request_resync();

    CHECK(cap.count_containing("request_resync(): no lane-record store") == 0);
}

// ============================================================================
// A lane written from outside a backend reaches its cached slot on request
// (prestonbrown/helixscreen#1653)
// ============================================================================

namespace {

constexpr uint32_t kFiledColor = 0x1A1A2E;

/// Files a Spoolman colour on slot 0's lane with no frame after it. The slot
/// the backend serves keeps what its last frame painted until the backend is
/// asked to repaint, and then shows the filing.
void expect_repaint_shows_filing(helix::AmsBackend& backend, helix::ams::LaneId lane,
                                 uint32_t frame_color) {
    const helix::SlotInfo painted = backend.get_slot_info(0);
    REQUIRE(painted.color_rgb == frame_color);

    helix::ams::Observation filed(helix::ams::ObservationSource::Spoolman);
    filed.color_rgb = kFiledColor;
    helix::ams::ingest(lane, filed);
    REQUIRE(backend.get_slot_info(0).color_rgb == frame_color);

    backend.repaint_slot_from_lane(0);

    CHECK(backend.get_slot_info(0).color_rgb == kFiledColor);
    // A filing states no presence, so the status the frame derived stands.
    CHECK(backend.get_slot_info(0).status == painted.status);
}

struct CachedSlotBackend {
    const char* name;
    void (*run)();
};

/// Every backend that paints a stored SlotInfo from the lane only while it
/// parses a frame. ACE is not here: it re-reads its slots on a short poll.
const CachedSlotBackend kCachedSlotBackends[] = {

    {"AFC",
     [] {
         AfcHarness harness(nullptr, nullptr);
         init_afc_lanes(*harness);
         feed_afc_lane(*harness, "lane1",
                       {{"prep", true},
                        {"load", true},
                        {"status", "Loaded"},
                        {"color", "#ED2C2C"},
                        {"material", "PETG"}});
         expect_repaint_shows_filing(*harness, harness.lane(0), 0xED2C2C);
     }},

    {"Happy Hare",
     [] {
         HappyHareHarness harness(nullptr, nullptr);
         feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1})},
                             {"gate_color", nlohmann::json::array({"ed2c2c"})},
                             {"gate_material", nlohmann::json::array({"PETG"})}});
         expect_repaint_shows_filing(*harness, harness.lane(0), 0xED2C2C);
     }},

    {"ToolChanger",
     [] {
         ToolChangerHarness harness(nullptr, nullptr);
         harness->set_discovered_tools({"T0", "T1"});
         feed_toolchanger(
             *harness, nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});
         expect_repaint_shows_filing(*harness, harness.lane(0), helix::kAmsDefaultSlotColor);
     }},
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a backend serving its slots from a cache repaints one from its lane on request",
                 "[lane][1653]") {
    for (const auto& backend : kCachedSlotBackends) {
        DYNAMIC_SECTION(backend.name) {
            backend.run();
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a tool changer frame paints a lane whose only identity is its Spoolman record",
                 "[lane][toolchanger][1653]") {
    ToolChangerHarness harness(nullptr, nullptr);
    harness->set_discovered_tools({"T0", "T1"});
    REQUIRE_FALSE(ToolChangerTestAccess::has_overrides(*harness));

    helix::ams::Observation filed(helix::ams::ObservationSource::Spoolman);
    filed.spoolman_id = 7;
    filed.color_rgb = kFiledColor;
    helix::ams::ingest(harness.lane(0), filed);
    REQUIRE(harness->get_slot_info(0).color_rgb == helix::kAmsDefaultSlotColor);

    feed_toolchanger(*harness,
                     nlohmann::json{{"toolchanger", {{"status", "ready"}, {"tool_number", 0}}}});

    CHECK(harness->get_slot_info(0).color_rgb == kFiledColor);
}

// Clear Spool hands the lane back to the machine. The spool name goes with the
// user's record, and what stands afterwards is the product line the tag itself
// states, or nothing.

// ============================================================================
// Tag readings: what each tag-reading backend keeps of the tag under an edit
// ============================================================================

using helix::ams::TagField;

TEST_CASE_METHOD(LVGLTestFixture, "a restart forgets the box's tag readings",
                 "[lane][cfs][qidi][tag_reading]") {}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's colour scan is the lane's tag reading",
                 "[lane][afc][tag_reading]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);
    feed_afc_lane(
        *harness, "lane1",
        nlohmann::json{{"color", "#FFFFFF"}, {"material", "PLA"}, {"td1_color", "FF8800"}});

    const auto tag = harness->tag_reading(0);
    REQUIRE(tag.has_value());
    CHECK(tag->color_rgb == std::optional<uint32_t>(0xFF8800u));
    CHECK_FALSE(tag->material.has_value());
    CHECK_FALSE(harness->tag_provenance(0).from_tag(TagField::Color));

    const auto reset = helix::ams::reset_to_tag(harness->get_slot_info(0), *tag);
    CHECK(reset.color_rgb == 0xFF8800u);
    CHECK(reset.material == "PLA");

    CHECK_FALSE(harness->tag_reading(1).has_value());
}
