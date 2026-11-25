// SPDX-License-Identifier: GPL-3.0-or-later

// The ams_slot badge's number is the lane's SPOOL number, and a routing change
// re-numbers lanes far from the one that changed: a lane stops sharing a spool
// and every lane after it shifts, with no status change of its own. The badge
// must follow the spool table (the slots_version subject), not only this
// lane's own status.

#include "ui_ams_slot.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_state.h"

#include <fstream>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

json ace_fixture() {
    // Tests run from the repo root (make t and the documented direct run
    // alike), and a ccache-served object can carry a __FILE__ spelled
    // relative or from another tree, so the fixture opens cwd-relative first
    // and falls back to the __FILE__ derivation.
    std::ifstream f("tests/fixtures/multiace/head_mode_4ace.json");
    if (!f.is_open()) {
        const std::string src = __FILE__;
        f.open(src.substr(0, src.rfind("/tests/unit/")) +
               "/tests/fixtures/multiace/head_mode_4ace.json");
    }
    REQUIRE(f.is_open());
    json j;
    f >> j;
    return json{{"ace", j}};
}

lv_obj_t* create_ams_slot(lv_obj_t* parent, int slot_index) {
    const std::string index_str = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", index_str.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
}

std::string badge_text(lv_obj_t* slot) {
    lv_obj_t* label = lv_obj_find_by_name(slot, "slot_badge_label");
    REQUIRE(label != nullptr);
    return std::string(lv_label_get_text(label));
}

} // namespace
