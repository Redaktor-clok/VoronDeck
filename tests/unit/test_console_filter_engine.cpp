// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_dir_guard.h"
#include "console_filter_engine.h"
#include "printer_detector.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::ConsoleFilterEngine;

namespace {

// Real samples captured from a Creality K2 Plus running stock firmware (Apr 2026).
// These are the format strings emitted by motor_control_wrapper.cpython-39.so via
// gcode.respond_info() — Klipper prepends "// " when broadcasting on the GCode console.
constexpr const char* kK2NoiseSamples[] = {
    "// data_send:b'\\x83\\x05\\x00\\x06\\x02F' "
    "recv_result:b'\\xf7\\x83\\x07\\x00\\x06\\x00\\x00\\x96C\\t'",
    "// "
    "sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c-\xe8\xaf\xbb\xe5\x8f\x96:b'"
    "\\x00\\x00\\x96C' value:300.0",
    "// "
    "sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c-"
    "\xe5\x86\x99\xe5\x85\xa5\xe6\x88\x90\xe5\x8a\x9f:b'F' value:100.0",
    "// send float_bytes:b'\\x00\\x00\\xc8B'",
    "// recv_result:b'\\xf7\\x83\\x07\\x00\\x06B`\\xe5<\\x9c'",
};

// Lines that must NEVER be filtered (real GCode responses).
constexpr const char* kGcodeKeepSamples[] = {
    "ok",
    "ok T:210.0 /210.0 B:60.0 /60.0",
    "// probe at 50.000,50.000 is z=0.123",
    "!! Move out of range",
    "Klipper state: Printer is ready",
    "// Done",
};

} // namespace

// ============================================================================
// Pattern parsing
// ============================================================================

TEST_CASE("ConsoleFilter: parse accepts well-formed specs", "[console_filter]") {
    ConsoleFilterEngine::Type type;
    std::string text;

    REQUIRE(ConsoleFilterEngine::parse("prefix:// foo", type, text));
    CHECK(type == ConsoleFilterEngine::Type::Prefix);
    CHECK(text == "// foo");

    REQUIRE(ConsoleFilterEngine::parse("substring:LOAD_AI_DEAL", type, text));
    CHECK(type == ConsoleFilterEngine::Type::Substring);
    CHECK(text == "LOAD_AI_DEAL");

    REQUIRE(ConsoleFilterEngine::parse("regex:^// config_ioRemap", type, text));
    CHECK(type == ConsoleFilterEngine::Type::Regex);
    CHECK(text == "^// config_ioRemap");
}

TEST_CASE("ConsoleFilter: parse rejects malformed specs", "[console_filter]") {
    ConsoleFilterEngine::Type type;
    std::string text;

    CHECK_FALSE(ConsoleFilterEngine::parse("", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse("nocolon", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse(":missing-kind", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse("prefix:", type, text));
    CHECK_FALSE(ConsoleFilterEngine::parse("unknown_kind:foo", type, text));
}

// ============================================================================
// Add / remove
// ============================================================================

TEST_CASE("ConsoleFilter: add accepts valid, rejects invalid", "[console_filter]") {
    ConsoleFilterEngine eng;
    CHECK(eng.add("prefix:// data_send:"));
    CHECK(eng.add("substring:LOAD_AI_DEAL"));
    CHECK(eng.add("regex:^// config_ioRemap"));
    CHECK(eng.size() == 3);

    CHECK_FALSE(eng.add("not-a-spec"));
    CHECK_FALSE(eng.add("regex:[invalid("));
    CHECK(eng.size() == 3);
}

TEST_CASE("ConsoleFilter: remove drops only exact (type, text) match", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    eng.add("substring:// data_send:"); // same text, different type
    eng.add("prefix:// recv_result:");
    REQUIRE(eng.size() == 3);

    eng.remove("prefix:// data_send:");
    CHECK(eng.size() == 2);

    // The substring entry with the same text remains.
    CHECK(eng.should_filter("xx // data_send:foo"));
}

TEST_CASE("ConsoleFilter: empty engine never filters", "[console_filter]") {
    ConsoleFilterEngine eng;
    for (const char* line : kK2NoiseSamples) {
        CHECK_FALSE(eng.should_filter(line));
    }
}

// ============================================================================
// Matching semantics
// ============================================================================

TEST_CASE("ConsoleFilter: prefix match anchored at start only", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// foo");
    CHECK(eng.should_filter("// foo bar"));
    CHECK_FALSE(eng.should_filter("xx // foo bar"));
    CHECK_FALSE(eng.should_filter("// fo"));
}

TEST_CASE("ConsoleFilter: substring match anywhere", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("substring:LOAD_AI_DEAL");
    CHECK(eng.should_filter("// LOAD_AI_DEAL: ..."));
    CHECK(eng.should_filter("xxx LOAD_AI_DEAL xxx"));
    CHECK_FALSE(eng.should_filter("// load_ai_deal"));
}

TEST_CASE("ConsoleFilter: regex match", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("regex:^// config_ioRemap oid=[0-9]+");
    CHECK(eng.should_filter("// config_ioRemap oid=42 src_pin=PB1"));
    CHECK_FALSE(eng.should_filter("// config_ioRemap"));
    CHECK_FALSE(eng.should_filter("xx // config_ioRemap oid=1"));
}

// ============================================================================
// K2 noise preset (the original motivating use case)
// ============================================================================

TEST_CASE("ConsoleFilter: K2 firmware-noise preset suppresses all known samples",
          "[console_filter][k2]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    eng.add("prefix:// recv_result:");
    eng.add("prefix:// send float_bytes:");
    // U+0073 prefixes plus the Chinese 系/参/数/操/作 prefix bytes — anything starting
    // with "// sys" + Chinese characters that the firmware uses for these messages.
    eng.add("prefix:// sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c");

    for (const char* sample : kK2NoiseSamples) {
        INFO("sample: " << sample);
        CHECK(eng.should_filter(sample));
    }
}

TEST_CASE("ConsoleFilter: K2 preset never suppresses real GCode responses",
          "[console_filter][k2]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    eng.add("prefix:// recv_result:");
    eng.add("prefix:// send float_bytes:");
    eng.add("prefix:// sys\xe5\x8f\x82\xe6\x95\xb0\xe6\x93\x8d\xe4\xbd\x9c");

    for (const char* line : kGcodeKeepSamples) {
        INFO("line: " << line);
        CHECK_FALSE(eng.should_filter(line));
    }
}

TEST_CASE("ConsoleFilter: shipped entries preserve mesh results and valid unique patterns",
          "[console_filter][preset]") {
    const auto names = PrinterDetector::get_list_names();
    REQUIRE(names.size() > 2);
    for (const auto& printer : names) {
        INFO(printer);
        auto specs = PrinterDetector::get_console_filter_patterns(printer);
        ConsoleFilterEngine eng;
        for (const auto& spec : specs)
            REQUIRE(eng.add(spec));
        const auto before = specs.size();
        std::sort(specs.begin(), specs.end());
        specs.erase(std::unique(specs.begin(), specs.end()), specs.end());
        CHECK(specs.size() == before);
        CHECK_FALSE(eng.should_filter("// probe at 50.000,50.000 is z=0.123"));
        CHECK_FALSE(eng.should_filter("// Mesh Bed Leveling Complete"));
        CHECK_FALSE(eng.should_filter("// Bed Mesh state has been saved to profile [default]"));
    }
}

TEST_CASE("ConsoleFilter: user entries share and deduplicate filter sets",
          "[console_filter][preset]") {
    struct ReloadDatabase {
        ~ReloadDatabase() {
            PrinterDetector::reload();
        }
    } restore;
    helix::ConfigDirGuard cfg("console_filter_sets");
    const auto directory = cfg.dir / "printer_database.d";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "filters.json") << R"({
        "console_filter_sets":{"test_noise":{"patterns":["prefix:// debug:","substring:TRACE_NOISE"]}},
        "printers":[
            {"id":"test_filter_a","name":"Test Filter A","console_filters":["test_noise"],"console_filter_patterns":["prefix:// debug:"]},
            {"id":"test_filter_b","name":"Test Filter B","console_filters":["test_noise"]}
        ]
    })";
    PrinterDetector::reload();
    const auto a = PrinterDetector::get_console_filter_patterns("Test Filter A");
    const auto b = PrinterDetector::get_console_filter_patterns("Test Filter B");
    REQUIRE(a.size() == 2);
    CHECK(a == b);
    ConsoleFilterEngine eng;
    for (const auto& spec : a)
        REQUIRE(eng.add(spec));
    CHECK(eng.should_filter("// debug: frame"));
    CHECK(eng.should_filter("// frame TRACE_NOISE end"));
    for (const char* line : kGcodeKeepSamples)
        CHECK_FALSE(eng.should_filter(line));
}

TEST_CASE("ConsoleFilter: unknown and retired printer names resolve to no patterns",
          "[console_filter][preset]") {
    for (const char* name : {"No Such Printer 9000", "", "Creality K2 Plus", "Creality K1C"})
        CHECK(PrinterDetector::get_console_filter_patterns(name).empty());
}

TEST_CASE("ConsoleFilter: patterns are evaluated cheapest matcher first", "[console_filter]") {
    ConsoleFilterEngine eng;
    // Added worst-first on purpose; the engine must reorder.
    REQUIRE(eng.add("regex:^// zzz[0-9]"));
    REQUIRE(eng.add("substring:middle"));
    REQUIRE(eng.add("prefix:// aaa"));
    REQUIRE(eng.size() == 3);

    const auto types = eng.types_in_order();
    REQUIRE(types.size() == 3);
    CHECK(types[0] == ConsoleFilterEngine::Type::Prefix);
    CHECK(types[1] == ConsoleFilterEngine::Type::Substring);
    CHECK(types[2] == ConsoleFilterEngine::Type::Regex);

    // Reordering must not change what matches.
    CHECK(eng.should_filter("// aaa here"));
    CHECK(eng.should_filter("has middle in it"));
    CHECK(eng.should_filter("// zzz9"));
    CHECK_FALSE(eng.should_filter("// bbb"));
}

TEST_CASE("ConsoleFilter: empty line never matches a populated engine", "[console_filter]") {
    ConsoleFilterEngine eng;
    REQUIRE(eng.add("prefix:// debug:"));
    CHECK_FALSE(eng.should_filter(""));
}

TEST_CASE("ConsoleFilter: trailing CRLF does not break prefix matching", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// data_send:");
    CHECK(eng.should_filter("// data_send:foo\r\n"));
    CHECK(eng.should_filter("// data_send:bar\n"));
}

TEST_CASE("ConsoleFilter: substring match handles UTF-8 multi-byte correctly", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("substring:\xe5\x8f\x82\xe6\x95\xb0"); // 参数 (U+53C2 U+6570)
    CHECK(eng.should_filter("xx \xe5\x8f\x82\xe6\x95\xb0 xx"));
    // Partial codepoint at the boundary must NOT match. find() is byte-oriented
    // but since we're searching for a complete multi-byte sequence, byte-level
    // search aligns with codepoint boundaries provided no foreign bytes splice
    // into a complete codepoint mid-line.
    CHECK_FALSE(eng.should_filter("\xe5\x8f")); // truncated 参 — 2 of 3 bytes
}

TEST_CASE("ConsoleFilter: clear() then re-add() rebuilds correctly", "[console_filter]") {
    ConsoleFilterEngine eng;
    eng.add("prefix:// foo");
    REQUIRE(eng.should_filter("// foo bar"));

    eng.clear();
    CHECK(eng.empty());
    CHECK_FALSE(eng.should_filter("// foo bar"));

    eng.add("prefix:// baz");
    CHECK(eng.should_filter("// baz qux"));
    CHECK_FALSE(eng.should_filter("// foo bar"));
}
