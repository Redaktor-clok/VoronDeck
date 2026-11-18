// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_eta_estimator.h"
#include "../catch_amalgamated.hpp"
#include <climits>
using namespace helix;
TEST_CASE("ETA: file time owns the estimate", "[print][eta]") {
    PrintEtaEstimator e;
    CHECK(e.remaining_seconds({0, 0, 600, 0}) == 600);
    CHECK(e.remaining_seconds({50, 9000, 600, 0}) == 300);
    CHECK(e.remaining_seconds({50, 1, 600, 0}) == 300);
    CHECK(e.remaining_seconds({100, 60, 600, 0}) == 0);
    CHECK(e.remaining_seconds({-5, 0, 600, 0}) == 600);
    CHECK(e.remaining_seconds({110, 0, 600, 0}) == 0);
    e.reset();
    CHECK(e.remaining_seconds({25, 500, 800, 0}) == 600);
}
TEST_CASE("ETA: only a measured preparation remainder is added before printing", "[print][eta]") {
    PrintEtaEstimator e;
    CHECK(e.remaining_seconds({0, 0, 600, 120}) == 720);
    CHECK(e.remaining_seconds({0, 0, 600, -1}) == 600);
    CHECK(e.remaining_seconds({50, 100, 600, 120}) == 300);
}
TEST_CASE("ETA: missing file time stays unknown", "[print][eta]") {
    PrintEtaEstimator e;
    CHECK(e.remaining_seconds({0, 0, 0, 120}) == -1);
    CHECK(e.remaining_seconds({50, 500, 0, 120}) == -1);
    CHECK(e.remaining_seconds({99, 500, -5, 0}) == -1);
    CHECK(e.remaining_seconds({100, 500, 0, 0}) == 0);
    CHECK(e.remaining_seconds({50, 0, INT_MAX, 0}) == 1073741823);
    CHECK(e.remaining_seconds({0, 0, INT_MAX, 100}) == INT_MAX);
}
