// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi_backend.h"
#include "../catch_amalgamated.hpp"

TEST_CASE("WiFi association waits for an address without claiming a disconnect", "[wifi][ip-wait]") {
    WifiBackend::ConnectionStatus status;
    CHECK_FALSE(status.awaiting_ip());
    status.connected = true;
    CHECK(status.awaiting_ip());
    status.ip_address = "0.0.0.0";
    CHECK(status.awaiting_ip());
    status.ip_address = "::";
    CHECK(status.awaiting_ip());
    status.ip_address = "192.168.1.42";
    CHECK_FALSE(status.awaiting_ip());
    status.ip_address = "2001:db8::1";
    CHECK_FALSE(status.awaiting_ip());
    status.connected = false;
    status.ip_address.clear();
    CHECK_FALSE(status.awaiting_ip());
}
