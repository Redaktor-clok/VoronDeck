// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every subscription backend answers "is this slot index valid" through
// AmsSubscriptionBackend::validate_slot_index (#1624). Driving each concrete
// backend through the base's name proves none of them shadows it with a copy of
// its own, and driving the edit entry points proves none of them skips it.

#include "../helix_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_toolchanger.h"
#include "ams_error.h"
#include "ams_types.h"

#include <memory>
#include <mutex>
#include <type_traits>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Reaches the base's protected validator and slot count through the concrete
/// backend's scope, so a backend-local validate_slot_index would be the one
/// called here.
template <class B> class SlotIndexProbe : public B {
  public:
    SlotIndexProbe() : B(nullptr, nullptr) {}

    AmsError check(int slot_index) const {
        return this->validate_slot_index(slot_index);
    }
    void set_total_slots(int n) {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->system_info_.total_slots = n;
    }
    void mark_running() {
        this->running_.store(true);
    }
    void clear_units() {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->system_info_.units.clear();
        this->system_info_.total_slots = 0;
    }
};

} // namespace

TEMPLATE_TEST_CASE("Every subscription backend bounds slot indices through the base validator",
                   "[ams][slot_index]", AmsBackendAfc, AmsBackendHappyHare, AmsBackendToolChanger) {
    HelixTestFixture fixture;
    SlotIndexProbe<TestType> backend;
    // 3 matches no backend's hardware constant, so a copy bounded by kNumTools
    // or kNumPorts answers differently from the base.
    backend.set_total_slots(3);

    CHECK(backend.check(0).success());
    CHECK(backend.check(2).success());
    CHECK(backend.check(-2).result == AmsResult::INVALID_SLOT);
    CHECK(backend.check(3).result == AmsResult::INVALID_SLOT);

    SECTION("every slot entry point refuses an index outside the range") {
        // A fresh backend per call, so one refusal's leftover state (a busy
        // action, a claim) cannot answer for the next. Where a backend checks
        // something else first (nothing loaded, operation unsupported, tool not
        // in the map) the refusal is that answer; elsewhere it is the validator's.
        auto fresh = [] {
            auto b = std::make_unique<SlotIndexProbe<TestType>>();
            b->set_total_slots(3);
            b->mark_running();
            return b;
        };
        auto refused_as_bad_slot = [](const AmsError& err, bool validator_answers) {
            CHECK_FALSE(err.success());
            if (validator_answers) {
                CHECK(err.result == AmsResult::INVALID_SLOT);
            }
        };
        constexpr bool eject_validates = std::is_same_v<TestType, AmsBackendAfc> ||
                                         std::is_same_v<TestType, AmsBackendHappyHare>;
        constexpr bool unload_validates = false;
        for (int bad : {-2, 3}) {
            CAPTURE(bad);
            refused_as_bad_slot(fresh()->load_filament(bad), true);
            refused_as_bad_slot(fresh()->eject_lane(bad), eject_validates);
            refused_as_bad_slot(fresh()->set_tool_mapping(0, bad), false);
        }
        // A negative unload index means "the loaded slot", so only past-the-end is bad.
        refused_as_bad_slot(fresh()->unload_filament(3), unload_validates);
    }

    SECTION("an edit to an index outside the range is refused as a bad slot") {
        CHECK(test::apply_edit(backend, -2, SlotInfo{}).result == AmsResult::INVALID_SLOT);
        CHECK(test::apply_edit(backend, 3, SlotInfo{}).result == AmsResult::INVALID_SLOT);
        CHECK(backend.sync_external_identity(-2, SlotInfo{}).result == AmsResult::INVALID_SLOT);
        CHECK(backend.sync_external_identity(3, SlotInfo{}).result == AmsResult::INVALID_SLOT);
    }
}

TEMPLATE_TEST_CASE("A backend with no slots discovered refuses as not connected",
                   "[ams][slot_index]", AmsBackendAfc, AmsBackendHappyHare, AmsBackendToolChanger) {
    SlotIndexProbe<TestType> backend;
    backend.set_total_slots(0);

    const AmsError err = backend.check(0);
    CHECK(err.result == AmsResult::NOT_CONNECTED);
    // The printer is online; only the filament system has nothing to report yet.
    CHECK(err.user_msg == "Multi-filament system not ready");
    if constexpr (std::is_same_v<TestType, AmsBackendToolChanger>) {
        CHECK(err.technical_msg == "No tools discovered");
    }
}
