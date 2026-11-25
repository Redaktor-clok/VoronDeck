// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_identity.h"
#include "platform_table.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("Linux board identity does not select proprietary hardware", "[board][vorondeck]") {
    using namespace helix::board;
    for (const char* key :
         {"pi", "pi32", "x86", "yocto", "ad5m", "ad5x", "k1", "k2", "snapmaker-u1"}) {
        CAPTURE(key);
        CHECK(resolve(key, "/nonexistent-probe-root") == Board::Generic);
    }
    const auto& host = info(Board::Generic);
    CHECK(host.local_camera_probe_safe);
    CHECK_FALSE(host.has_jz_pwm_piezo);
    CHECK_FALSE(host.firmware_dims_backlight_after_boot);
    CHECK_FALSE(host.firmware_manages_updates);
}

TEST_CASE("Release platforms retain Linux architectures", "[board][vorondeck]") {
    for (const char* key : {"pi", "pi32", "x86", "x86-desktop"}) {
        CAPTURE(key);
        REQUIRE(helix::platform::find(key));
    }
    for (const char* key : {"ad5m", "ad5x", "mips", "k1", "k2", "cc1", "snapmaker-u1"}) {
        CAPTURE(key);
        CHECK(helix::platform::find(key) == nullptr);
    }
}
