// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "../catch_amalgamated.hpp"
#include "ui_wizard.h"
#include "helix-xml/src/xml/lv_xml.h"
#include <string>

TEST_CASE_METHOD(LVGLUITestFixture, "Wizard headers preserve complete Russian strings",
                 "[wizard][wizard_header_text]") {
    ui_wizard_init_subjects();
    const std::pair<const char*, const char*> texts[] = {
        {"wizard_title", "Обнаружена мультифиламентная система"},
        {"wizard_subtitle", "Если Moonraker локальный или экран подключён к принтеру напрямую, подключение будет выполнено автоматически."},
    };
    for (const auto& [name, text] : texts) {
        CAPTURE(name);
        auto* subject = lv_xml_get_subject(nullptr, name);
        REQUIRE(subject);
        lv_subject_copy_string(subject, text);
        CHECK(std::string(lv_subject_get_string(subject)) == text);
    }
}
