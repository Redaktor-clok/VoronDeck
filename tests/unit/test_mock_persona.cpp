// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_persona.h"
#include "test_helpers/mock_personas.h"

#include <algorithm>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::mock::PrinterType;

namespace {
constexpr PrinterType kAllTypes[] = {
    PrinterType::VORON_24,           PrinterType::VORON_TRIDENT,  PrinterType::GENERIC_COREXY,
    PrinterType::GENERIC_BEDSLINGER, PrinterType::MULTI_EXTRUDER, PrinterType::DELTA,
};
} // namespace

TEST_CASE("Persona ids are unique and every type has a persona", "[mock][persona]") {
    std::set<std::string_view> ids;
    for (const auto& p : helix::mock::kPersonas) {
        CHECK(ids.insert(p.id).second);
        CHECK_FALSE(p.display_name.empty());
    }
    for (PrinterType t : kAllTypes) {
        bool named = false;
        for (const auto& p : helix::mock::kPersonas)
            named = named || p.type == t;
        INFO("PrinterType " << static_cast<int>(t));
        CHECK(named);
    }
    CHECK(helix::mock::kPersonas[0].id == "voron_24");
}

TEST_CASE("personas_one_per_type lists every type exactly once", "[mock][persona]") {
    const auto personas = helix::test::personas_one_per_type();
    CHECK(personas.size() == std::size(kAllTypes));
    for (PrinterType t : kAllTypes) {
        INFO("PrinterType " << static_cast<int>(t));
        CHECK(std::count_if(personas.begin(), personas.end(),
                            [t](const auto* p) { return p->type == t; }) == 1);
    }
}

TEST_CASE("resolve_persona: unset and empty are the silent default", "[mock][persona]") {
    bool recognised = false;
    CHECK(helix::mock::resolve_persona(nullptr, &recognised).id == "voron_24");
    CHECK(recognised);
    CHECK(helix::mock::resolve_persona("", &recognised).id == "voron_24");
    CHECK(recognised);
}

TEST_CASE("resolve_persona: exact, case-sensitive match", "[mock][persona]") {
    bool recognised = false;
    CHECK(helix::mock::resolve_persona("voron_24", &recognised).type == PrinterType::VORON_24);
    CHECK(recognised);
    CHECK(helix::mock::resolve_persona("AD5M", &recognised).id == "voron_24");
    CHECK_FALSE(recognised);
    CHECK(helix::mock::resolve_persona("ad5m ", &recognised).id == "voron_24");
    CHECK_FALSE(recognised);
}

TEST_CASE("persona_ids lists the table in order", "[mock][persona]") {
    const std::string ids = helix::mock::persona_ids();
    CHECK(ids.rfind("voron_24, voron_trident, ", 0) == 0);
    for (const auto& p : helix::mock::kPersonas) {
        CHECK(ids.find(std::string(p.id)) != std::string::npos);
    }
    CHECK(ids.find(", ,") == std::string::npos);
}

TEST_CASE("effective_mock_ams: explicit wins, else the persona default", "[mock][persona][ams]") {
    using helix::mock::effective_mock_ams;
    CHECK(effective_mock_ams("AFC", "creator5") == "afc");
    CHECK(effective_mock_ams("none", "creator5") == "none");
    CHECK(effective_mock_ams(nullptr, "creator5").empty());
    CHECK(effective_mock_ams("", "creator5").empty());
    CHECK(effective_mock_ams(nullptr, "voron_24").empty());
    CHECK(effective_mock_ams(nullptr, "nonsense").empty());
    CHECK(effective_mock_ams(nullptr, nullptr).empty());
    CHECK(effective_mock_ams(nullptr, "ad5x").empty());
    CHECK(effective_mock_ams("afc", "ad5x") == "afc");
    CHECK(effective_mock_ams(nullptr, "k2").empty());
    CHECK(effective_mock_ams(nullptr, "snapmaker_u1").empty());
}

TEST_CASE("A persona's own probe stands in for the default cartographer", "[mock][persona]") {
    for (const auto& p : helix::mock::kPersonas) {
        const auto d = helix::mock::descriptor(p.type);
        INFO(p.id);
        if (!d.probe.empty())
            CHECK((d.omit & helix::mock::default_object::CARTOGRAPHER) != 0);
    }
}

TEST_CASE("is_hardware_persona follows the descriptor", "[mock][persona]") {
    CHECK_FALSE(helix::mock::is_hardware_persona("creator5_zmod"));
    CHECK_FALSE(helix::mock::is_hardware_persona("creator5"));
    CHECK_FALSE(helix::mock::is_hardware_persona("CREATOR5_ZMOD"));
    CHECK_FALSE(helix::mock::is_hardware_persona(""));
}
