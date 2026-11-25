// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_identity.h"

#include "platform_table.h"

namespace helix::board {

namespace {

constexpr Info kGeneric{Board::Generic, "generic", "", nullptr, true, false, false, false, false};

std::optional<Board> s_override;

} // namespace

Board resolve(const std::string& platform_key, const std::string& probe_root) {
    return Board::Generic;
}

Board current() {
    if (s_override)
        return *s_override;
    static const Board cached = resolve(platform::current_key(), "/");
    return cached;
}

const Info& info(Board board) {
    return kGeneric;
}

void set_current_override(std::optional<Board> board) {
    s_override = board;
}

} // namespace helix::board
