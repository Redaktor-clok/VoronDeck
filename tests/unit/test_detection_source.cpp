// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_fixtures.h"
#include "../test_helpers/update_queue_test_access.h"
#include "detection_source.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using helix::detection::DetectionEvent;
using helix::detection::DetectionKind;
using json = nlohmann::json;

TEST_CASE("DetectionEvent defaults are inert", "[detection][source]") {
    DetectionEvent e;
    REQUIRE(e.kind == DetectionKind::Unknown);
    REQUIRE(e.attributable == false);
    REQUIRE(e.already_paused == false);
    REQUIRE(e.source_id.empty());
    REQUIRE_FALSE(e.confidence.has_value());
}
