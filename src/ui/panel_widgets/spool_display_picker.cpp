// SPDX-License-Identifier: GPL-3.0-or-later
#include "spool_display_picker.h"

#include "helix-xml/src/xml/lv_xml.h"

#include <cstdlib>

namespace helix {
SpoolDisplayPicker::SpoolDisplayPicker() {
    lv_xml_register_event_cb(nullptr, "on_spool_display_selected", [](lv_event_t* e) {
        auto* picker = ui::ContextMenu::active_as<SpoolDisplayPicker>();
        const auto* value = static_cast<const char*>(lv_event_get_user_data(e));
        if (picker && value)
            picker->dispatch_action(std::atoi(value));
    });
}
} // namespace helix
