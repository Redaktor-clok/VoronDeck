// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_start_controller.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_mock.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// A file whose routing is known writes that routing in the pre-print send,
// not the previous job's crossover.
