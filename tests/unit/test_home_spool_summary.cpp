// SPDX-License-Identifier: GPL-3.0-or-later
#include "home_spool_summary.h"

#include <limits>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("Home spool amount preserves unknown, empty and estimated readings",
          "[vorondeck][home][spool-summary]") {
    HomeSpoolAmount amount;
    CHECK(home_spool_summary(amount, {}).empty());
    amount = {850, 1000, -1, 1.04, 1.75};
    CHECK(home_spool_summary(amount, {}) == "85% · 850 / 1000 g · ~ 340 m");
    amount.reported_metres = 321;
    CHECK(home_spool_summary(amount, {}) == "85% · 850 / 1000 g · 321 m");
    CHECK(home_spool_summary(amount, {false, false, false}).empty());
    CHECK(home_spool_summary(amount, {false, true, false}) == "850 / 1000 g");
    CHECK(home_spool_summary(amount, {true, false, true}) == "85% · 321 m");

    amount = {0, 1000, -1, 1.04, 1.75};
    CHECK(home_spool_summary(amount, {}) == "0% · 0 / 1000 g · ~ 0 m");
    amount = {850, -1, -1, 0, 0};
    CHECK(home_spool_summary(amount, {}) == "850 g");
    amount.remaining_g = std::numeric_limits<double>::quiet_NaN();
    CHECK(home_spool_summary(amount, {}).empty());
    amount = {-1, 1000, 120, 1.04, 1.75};
    CHECK(home_spool_summary(amount, {}) == "120 m");
    amount = {1200, 1000, -1, 0, 0};
    CHECK(home_spool_summary(amount, {true, false, false}) == "100%");
    CHECK(home_spool_summary({850, 1000, 340, 0, 0}, {false, true, true}, false) ==
          "850 g · 340 m");
    for (double remaining : {0.0, 500.0, 1000.0}) {
        amount = {remaining, 1000, -1, 0, 0};
        CHECK(home_spool_fill_level(amount) == Catch::Approx(remaining / 1000));
    }
    CHECK(home_spool_fill_level({-1, 1000, -1, 0, 0}) == -1);
    CHECK(home_spool_fill_level({500, -1, -1, 0, 0}) == -1);
    CHECK(home_spool_fill_level({std::numeric_limits<double>::quiet_NaN(), 1000, -1, 0, 0}) == -1);
}
