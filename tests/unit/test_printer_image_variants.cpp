// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_image_variants.cpp
 * @brief The image_variants matcher: which database image a detected printer shows
 */

#include "printer_image_variants.h"

#include "../catch_amalgamated.hpp"

using helix::AmsType;
using helix::printer_images::HardwareFacts;
using helix::printer_images::is_known_variant_condition;
using helix::printer_images::is_valid_variant_condition_value;
using helix::printer_images::select_image;
using helix::printer_images::variant_matches;
using nlohmann::json;

namespace {

const HardwareFacts kNoHardware{};
const HardwareFacts kAfc{{AmsType::AFC}};
const HardwareFacts kTopFitted{{}, true};
const HardwareFacts kChamberHeater{{}, false, true};

json entry_with(json variants) {
    return json{{"image", "plain.png"}, {"image_variants", std::move(variants)}};
}

} // namespace

TEST_CASE("select_image: an entry without variants shows its image", "[printer_image_variants]") {
    const json entry{{"image", "plain.png"}};
    CHECK(select_image(entry, kNoHardware) == "plain.png");
    CHECK(select_image(entry, kAfc) == "plain.png");
    CHECK(select_image(json::object(), kAfc).empty());
}

TEST_CASE("select_image: a matching variant replaces the image", "[printer_image_variants]") {
    const json entry =
        entry_with({{{"when", {{"filament_system", "afc"}}}, {"image", "with-afc.png"}}});
    CHECK(select_image(entry, kAfc) == "with-afc.png");
}

TEST_CASE("select_image: no matching variant falls back to the image", "[printer_image_variants]") {
    const json entry =
        entry_with({{{"when", {{"filament_system", "afc"}}}, {"image", "with-afc.png"}}});
    CHECK(select_image(entry, kNoHardware) == "plain.png");
    CHECK(select_image(entry, HardwareFacts{{AmsType::OPENAMS}}) == "plain.png");
}

TEST_CASE("select_image: the first matching variant wins", "[printer_image_variants]") {
    const json entry = entry_with({
        {{"when", {{"filament_system", "openams"}}}, {"image", "with-openams.png"}},
        {{"when", {{"filament_system", "afc"}}}, {"image", "first-afc.png"}},
        {{"when", {{"filament_system", "afc"}}}, {"image", "second-afc.png"}},
    });
    CHECK(select_image(entry, kAfc) == "first-afc.png");
    // Any detected backend of the kind counts, not only the first.
    CHECK(select_image(entry, HardwareFacts{{AmsType::HAPPY_HARE, AmsType::AFC}}) ==
          "first-afc.png");
}

TEST_CASE("select_image: a variant without an image is skipped", "[printer_image_variants]") {
    const json entry = entry_with({
        {{"when", {{"filament_system", "afc"}}}},
        {{"when", {{"filament_system", "afc"}}}, {"image", "with-afc.png"}},
    });
    CHECK(select_image(entry, kAfc) == "with-afc.png");
}

TEST_CASE("variant_matches: an unknown condition key never matches", "[printer_image_variants]") {
    // An entry naming a condition this build cannot evaluate shows its plain image.
    CHECK_FALSE(variant_matches({{"enclosure", true}}, kAfc));
    CHECK_FALSE(variant_matches({{"filament_system", "afc"}, {"unit_count", 2}}, kAfc));
    const json entry = entry_with({{{"when", {{"unit_count", 2}}}, {"image", "two-units.png"}}});
    CHECK(select_image(entry, kAfc) == "plain.png");
}

TEST_CASE("variant_matches: malformed conditions never match", "[printer_image_variants]") {
    CHECK_FALSE(variant_matches(json("afc"), kAfc));
    CHECK_FALSE(variant_matches(json::array(), kAfc));
    CHECK_FALSE(variant_matches({{"filament_system", 6}}, kAfc));
    CHECK_FALSE(variant_matches({{"filament_system", "not-a-system"}}, kNoHardware));
    // "none" names no filament system, so it cannot be used to match a bare printer.
    CHECK_FALSE(variant_matches({{"filament_system", "none"}}, kNoHardware));
    // A tool changer is a kind of toolhead, not a filament system.
    CHECK_FALSE(variant_matches({{"filament_system", "tool_changer"}},
                                HardwareFacts{{AmsType::TOOL_CHANGER}}));
    CHECK_FALSE(is_valid_variant_condition_value("filament_system", "tool_changer"));
}

TEST_CASE("variant_matches: an empty condition set matches", "[printer_image_variants]") {
    CHECK(variant_matches(json::object(), kNoHardware));
}

TEST_CASE("variant condition vocabulary", "[printer_image_variants]") {
    CHECK(is_known_variant_condition("filament_system"));
    CHECK_FALSE(is_known_variant_condition("enclosure"));
    CHECK(is_valid_variant_condition_value("filament_system", "afc"));
    CHECK(is_valid_variant_condition_value("filament_system", "happy_hare"));
    CHECK_FALSE(is_valid_variant_condition_value("filament_system", "none"));
    CHECK_FALSE(is_valid_variant_condition_value("filament_system", 6));
    CHECK_FALSE(is_valid_variant_condition_value("enclosure", true));
    CHECK(is_known_variant_condition("enclosure_top"));
    CHECK(is_valid_variant_condition_value("enclosure_top", true));
    CHECK(is_valid_variant_condition_value("enclosure_top", false));
    CHECK_FALSE(is_valid_variant_condition_value("enclosure_top", 1));
    CHECK_FALSE(is_valid_variant_condition_value("enclosure_top", "true"));
}

TEST_CASE("select_image: an enclosure_top variant follows the fitted top",
          "[printer_image_variants]") {
    const json entry =
        entry_with({{{"when", {{"enclosure_top", true}}}, {"image", "with-top.png"}}});
    CHECK(select_image(entry, kTopFitted) == "with-top.png");
    CHECK(select_image(entry, kNoHardware) == "plain.png");

    // false names the bare printer, which includes one that reports no top at all.
    CHECK(variant_matches({{"enclosure_top", false}}, kNoHardware));
    CHECK_FALSE(variant_matches({{"enclosure_top", false}}, kTopFitted));
}

TEST_CASE("variant_matches: enclosure_top combines with the other conditions",
          "[printer_image_variants]") {
    const HardwareFacts afc_and_top{{AmsType::AFC}, true};
    const json both{{"filament_system", "afc"}, {"enclosure_top", true}};
    CHECK(variant_matches(both, afc_and_top));
    CHECK_FALSE(variant_matches(both, kAfc));
    CHECK_FALSE(variant_matches(both, kTopFitted));
}

TEST_CASE("variant_matches: an enclosure_top value that is not a boolean never matches",
          "[printer_image_variants]") {
    CHECK_FALSE(variant_matches({{"enclosure_top", 1}}, kTopFitted));
    CHECK_FALSE(variant_matches({{"enclosure_top", "true"}}, kTopFitted));
    CHECK_FALSE(variant_matches({{"enclosure_top", nullptr}}, kNoHardware));
    CHECK_FALSE(variant_matches({{"enclosure_top", 0}}, kNoHardware));
}

TEST_CASE("select_image: a chamber_heater variant follows the heater", "[printer_image_variants]") {
    const json entry =
        entry_with({{{"when", {{"chamber_heater", true}}}, {"image", "with-heater.png"}}});
    CHECK(select_image(entry, kChamberHeater) == "with-heater.png");
    CHECK(select_image(entry, kNoHardware) == "plain.png");
    CHECK(select_image(entry, kTopFitted) == "plain.png");
    CHECK(variant_matches({{"chamber_heater", false}}, kNoHardware));
    CHECK_FALSE(variant_matches({{"chamber_heater", false}}, kChamberHeater));
}

TEST_CASE("variant_matches: a chamber_heater value that is not a boolean never matches",
          "[printer_image_variants]") {
    CHECK(is_known_variant_condition("chamber_heater"));
    CHECK(is_valid_variant_condition_value("chamber_heater", true));
    CHECK(is_valid_variant_condition_value("chamber_heater", false));
    CHECK_FALSE(is_valid_variant_condition_value("chamber_heater", 1));
    CHECK_FALSE(is_valid_variant_condition_value("chamber_heater", "true"));
    CHECK_FALSE(variant_matches({{"chamber_heater", 1}}, kChamberHeater));
    CHECK_FALSE(variant_matches({{"chamber_heater", nullptr}}, kNoHardware));
}

TEST_CASE("select_image: two variants naming one image show it when either fact holds",
          "[printer_image_variants]") {
    const json entry = entry_with({
        {{"when", {{"enclosure_top", true}}}, {"image", "with-top.png"}},
        {{"when", {{"chamber_heater", true}}}, {"image", "with-top.png"}},
    });
    CHECK(select_image(entry, kTopFitted) == "with-top.png");
    CHECK(select_image(entry, kChamberHeater) == "with-top.png");
    CHECK(select_image(entry, HardwareFacts{{}, true, true}) == "with-top.png");
    CHECK(select_image(entry, kNoHardware) == "plain.png");
}

TEST_CASE("Hardware facts change only with image-relevant hardware", "[printer_image_variants]") {
    HardwareFacts facts;
    CHECK(facts == HardwareFacts{});
    facts.filament_systems = {AmsType::OPENAMS};
    CHECK(facts != HardwareFacts{});
    auto other = facts;
    CHECK(facts == other);
    other.enclosure_top = !facts.enclosure_top;
    CHECK(facts != other);
    other = facts;
    other.has_chamber_heater = !facts.has_chamber_heater;
    CHECK(facts != other);
}
