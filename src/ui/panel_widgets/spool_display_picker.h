// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_context_menu.h"

namespace helix {

/// Shared tile gear choice. Backdrop dismissal keeps the current display.
class SpoolDisplayPicker : public ui::ContextMenu {
    HELIX_CONTEXT_MENU_KIND(SpoolDisplayPicker)
  public:
    SpoolDisplayPicker();

  protected:
    const char* xml_component_name() const override {
        return "spool_display_picker";
    }
    CardWidth card_width() const override {
        return {40, 220, 300};
    }
};

} // namespace helix
