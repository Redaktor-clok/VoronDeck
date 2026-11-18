// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "ams_backend_mock.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(HelixTestFixture, "AFC mock physical units preserve global lanes through 24",
                 "[ams][mock-unit-counts]") {
    const auto counts = GENERATE(std::vector<int>{8}, std::vector<int>{16},
                                std::vector<int>{4, 4, 4, 4},
                                std::vector<int>{4, 4, 4, 4, 4, 4}, std::vector<int>{8, 8, 8});
    int total = 0;
    for (int count : counts)
        total += count;
    helix::AmsBackendMock mock(total);
    mock.set_afc_mode(true);
    const auto before = mock.get_system_info();
    REQUIRE(mock.set_unit_slot_counts(counts));
    const auto info = mock.get_system_info();
    REQUIRE(info.total_slots == total);
    REQUIRE(info.units.size() == counts.size());
    int global = 0;
    for (size_t unit = 0; unit < counts.size(); ++unit) {
        REQUIRE(info.units[unit].slot_count == counts[unit]);
        CHECK(info.units[unit].first_slot_global_index == global);
        for (int local = 0; local < counts[unit]; ++local, ++global) {
            const auto& slot = info.units[unit].slots[local];
            CHECK(slot.slot_index == local);
            CHECK(slot.global_index == global);
            CHECK(slot.material == before.get_slot_global(global)->material);
            CHECK(mock.spool_display_number(global) == global + 1);
            CHECK(mock.get_slot_info(global).global_index == global);
        }
    }
}

TEST_CASE_METHOD(HelixTestFixture, "AFC mock rejects incomplete unit grouping without mutation",
                 "[ams][mock-unit-counts]") {
    helix::AmsBackendMock mock(16);
    mock.set_afc_mode(true);
    for (const auto counts : {std::vector<int>{}, std::vector<int>{4, 4},
                             std::vector<int>{0, 16}, std::vector<int>{-1, 17},
                             std::vector<int>{64, 64}}) {
        CHECK_FALSE(mock.set_unit_slot_counts(counts));
        CHECK(mock.get_system_info().units.size() == 1);
        CHECK(mock.get_system_info().total_slots == 16);
    }
}
