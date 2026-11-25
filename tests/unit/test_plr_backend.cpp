// SPDX-License-Identifier: GPL-3.0-or-later
#include "plr_backend.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("Recovery requires a ready generic plugin", "[plr][backend]") {
    helix::PlrCapabilitySignals caps;
    CHECK(helix::plr_select_backend(caps) == helix::PlrBackendType::NONE);
    caps.snapshot_valid = true;
    caps.probe_capable = true;
    caps.resume_macro = true;
    caps.interrupted_flag = true;
    CHECK(helix::plr_select_backend(caps) == helix::PlrBackendType::NONE);
    caps.plugin_backend = helix::PlrPluginBackend::Generic;
    caps.plr_feature = helix::PluginFeatureState::Ready;
    CHECK(helix::plr_select_backend(caps) == helix::PlrBackendType::GENERIC);
    CHECK_FALSE(helix::plr_offer_availability(caps, {}).available);
    caps.plugin_offer_available = true;
    caps.plugin_recovery_file = "part.gcode";
    const auto offer = helix::plr_offer_availability(caps, {});
    CHECK(offer.available);
    CHECK(offer.recovery_file == "part.gcode");
}

TEST_CASE("Generic recovery uses the plugin RPC without firmware motion commands",
          "[plr][backend]") {
    const auto plan = helix::plr_build_plan(helix::PlrBackendType::GENERIC, "part.gcode", {});
    CHECK(plan.resume_gcode.empty());
    CHECK(plan.discard_gcode.empty());
    CHECK(plan.resume_rpc == std::string(helix::kPlrPlanRpc));
    CHECK(plan.discard_rpc_method == std::string(helix::kPlrDiscardRpc));
    CHECK(plan.resume_allowed());
    CHECK(plan.discard_available());
}

TEST_CASE("Retired firmware recovery cannot produce a resume or discard command",
          "[plr][backend]") {
    for (const auto backend : {helix::PlrBackendType::SNAPMAKER, helix::PlrBackendType::QIDI,
                               helix::PlrBackendType::CREALITY, helix::PlrBackendType::NONE}) {
        const auto plan = helix::plr_build_plan(backend, "part.gcode", {});
        CHECK_FALSE(plan.resume_allowed());
        CHECK_FALSE(plan.discard_available());
    }
}

TEST_CASE("Recovery filename validation rejects command separators", "[plr][backend]") {
    CHECK(helix::plr_is_safe_recovery_filename("folder/my part.gcode"));
    for (const auto name : {"", "a\nM112", "a;M112", "a\"b", "a#b", "a=b", "a*b", "a\\b"}) {
        CAPTURE(name);
        CHECK_FALSE(helix::plr_is_safe_recovery_filename(name));
    }
}
