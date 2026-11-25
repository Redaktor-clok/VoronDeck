// SPDX-License-Identifier: GPL-3.0-or-later
// Vendor state pushed by the HelixPrint plugin: the push-vs-poll rule over
// every plugin state, the tracker's resync and fallback, and the AD5X IFS and
// ACE backends applying a push through the same function as their poll.

#include "../fake_moonraker_client.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "app_globals.h"
#include "helix_plugin_features.h"
#include "printer_state.h"
#include "vendor_state_push.h"

#include <chrono>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

PluginFeatureEntry entry(int version, bool available, bool enabled) {
    PluginFeatureEntry e;
    e.version = version;
    e.available = available;
    e.enabled = enabled;
    e.reason = available ? "" : "no_vendor";
    return e;
}

/// The source a backend picks for feature `vendor_x` at contract 1.
VendorStateSource source_for(bool plugin, const PluginFeatureMap& features) {
    return vendor_state_source(plugin_feature_state(plugin, features, "vendor_x", 1));
}

json notification(const json& message) {
    return json{{"jsonrpc", "2.0"},
                {"method", kVendorStateNotifyMethod},
                {"params", json::array({message})}};
}

/// The plugin as PrinterState holds it, applied the way a probe applies it.
void set_plugin(bool installed, PluginFeatureMap features) {
    auto& plugin = get_printer_state().plugin_status_state();
    plugin.set_installed(installed);
    plugin.set_features(std::move(features));
}

struct PluginStateFixture : LVGLUITestFixture {
    ~PluginStateFixture() override {
        set_plugin(false, {});
    }
};

constexpr const char* kJsonA = R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})";
constexpr const char* kJsonB = R"({"FFMInfo":{"ffmColor1":"#00FF00","ffmType1":"PETG"}})";

json ace_slots(const char* material) {
    return json{{"slots",
                 {{{"index", 0}, {"color", "#FF0000"}, {"material", material}, {"status", "ready"}},
                  {{"index", 1}, {"color", "#00FF00"}, {"material", "PETG"}, {"status", "ready"}},
                  {{"index", 2}, {"color", "#0000FF"}, {"material", "ABS"}, {"status", "ready"}},
                  {{"index", 3}, {"color", "#FFFFFF"}, {"material", "TPU"}, {"status", "ready"}}}}};
}

} // namespace

// ============================================================================
// The rule
// ============================================================================

TEST_CASE("vendor state: push only while the plugin's feature is ready", "[plugin_vendor]") {
    SECTION("no plugin") {
        CHECK(source_for(false, {{"vendor_x", entry(1, true, true)}}) == VendorStateSource::Poll);
    }
    SECTION("a plugin without the features map") {
        CHECK(source_for(true, {}) == VendorStateSource::Poll);
    }
    SECTION("disabled") {
        CHECK(source_for(true, {{"vendor_x", entry(1, true, false)}}) == VendorStateSource::Poll);
    }
    SECTION("unavailable on this printer") {
        CHECK(source_for(true, {{"vendor_x", entry(1, false, false)}}) == VendorStateSource::Poll);
    }
    SECTION("a contract below the minimum") {
        CHECK(source_for(true, {{"vendor_x", entry(0, true, true)}}) == VendorStateSource::Poll);
    }
    SECTION("ready") {
        CHECK(source_for(true, {{"vendor_x", entry(1, true, true)}}) == VendorStateSource::Push);
    }
}

TEST_CASE("vendor state: a notification yields only its own vendor's payload", "[plugin_vendor]") {
    const json payload = {{"content", "x"}};
    CHECK(vendor_state_payload(notification({{"vendor", "ad5x"}, {"payload", payload}}), "ad5x") ==
          payload);
    CHECK_FALSE(
        vendor_state_payload(notification({{"vendor", "ace"}, {"payload", payload}}), "ad5x"));
    CHECK_FALSE(vendor_state_payload(notification({{"vendor", "ad5x"}}), "ad5x"));
    CHECK_FALSE(vendor_state_payload(notification(json::array()), "ad5x"));
    CHECK_FALSE(vendor_state_payload(json{{"params", json::array()}}, "ad5x"));
    CHECK_FALSE(vendor_state_payload(json::object(), "ad5x"));
}

TEST_CASE("vendor state: the tracker owes a read on each start of a push", "[plugin_vendor]") {
    VendorStateTracker tracker;
    CHECK_FALSE(tracker.pushing());

    CHECK_FALSE(tracker.update(PluginFeatureState::Absent, 1, 7));
    CHECK_FALSE(tracker.pushing());

    // The push starts: one read now, then none while nothing changes
    CHECK(tracker.update(PluginFeatureState::Ready, 1, 7));
    CHECK(tracker.pushing());
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 1, 7));

    // A later probe (a reconnect, a restarted plugin) may have missed a change
    CHECK(tracker.update(PluginFeatureState::Ready, 2, 7));
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 2, 7));

    // The plugin is gone: back to polling, nothing owed
    CHECK_FALSE(tracker.update(PluginFeatureState::Absent, 3, 7));
    CHECK_FALSE(tracker.pushing());
    CHECK_FALSE(tracker.update(PluginFeatureState::Disabled, 3, 7));
    CHECK_FALSE(tracker.pushing());

    CHECK(tracker.update(PluginFeatureState::Ready, 3, 7));
    CHECK(tracker.pushing());
}

TEST_CASE("vendor state: a Ready from before a reconnect waits for the next probe",
          "[plugin_vendor]") {
    VendorStateTracker tracker;
    REQUIRE(tracker.update(PluginFeatureState::Ready, 4, 1));

    // Reconnected; the feature map is still the old connection's
    CHECK_FALSE(tracker.update(PluginFeatureState::Ready, 4, 2));
    CHECK_FALSE(tracker.pushing());

    // Discovery probes on the new connection
    CHECK(tracker.update(PluginFeatureState::Ready, 5, 2));
    CHECK(tracker.pushing());
}

TEST_CASE("vendor state: reset goes back to polling", "[plugin_vendor]") {
    VendorStateTracker tracker;
    REQUIRE(tracker.update(PluginFeatureState::Ready, 1, 0));
    tracker.reset();
    CHECK_FALSE(tracker.pushing());
    // Nothing remembered: the same probe starts a push and owes a read again
    CHECK(tracker.update(PluginFeatureState::Ready, 1, 0));
}

TEST_CASE("vendor state: the backends keep a slow backstop poll while pushing", "[plugin_vendor]") {
    using std::chrono::seconds;
    CHECK(kVendorPushBackstop == seconds(30));
}

// ============================================================================
// AD5X IFS
// ============================================================================

// ============================================================================
// ACE
// ============================================================================
