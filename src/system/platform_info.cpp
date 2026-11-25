// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_info.h"

#include "board_identity.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/utsname.h>

namespace helix {

// -1 = use compile-time default, 0 = force non-Android, 1 = force Android
static int s_platform_override = -1;

bool is_android_platform() {
    if (s_platform_override >= 0) {
        return s_platform_override != 0;
    }
#ifdef __ANDROID__
    return true;
#else
    return false;
#endif
}

void set_platform_override(int override_value) {
    s_platform_override = override_value;
}

// -1 = use compile-time default, 0 = force self-managed, 1 = force externally managed
static int s_external_updates_default_override = -1;

bool platform_defaults_to_external_updates() {
    if (s_external_updates_default_override >= 0) {
        return s_external_updates_default_override != 0;
    }
    return board::current_info().firmware_manages_updates;
}

void set_external_updates_default_override(int override_value) {
    s_external_updates_default_override = override_value;
}

bool platform_host_power_supported() {
    return !is_android_platform();
}

// -1 = use compile-time default, 0 = force "beside the printer", 1 = force "on it"
static int s_printer_embedded_override = -1;

bool is_printer_embedded() {
    if (s_printer_embedded_override >= 0) {
        return s_printer_embedded_override != 0;
    }
    return board::current() != board::Board::Generic;
}

void set_printer_embedded_override(int override_value) {
    s_printer_embedded_override = override_value;
}

std::string host_arch_string() {
    struct utsname uts {};
    const std::string kernel =
        (uname(&uts) == 0 && uts.machine[0] != '\0') ? uts.machine : "unknown";
    // sizeof(void*) is the only reliable runtime check for binary bitness:
    // compile-time __aarch64__/__arm__ macros tell us the ABI family but not
    // 32-vs-64-bit on every supported target. " · " is U+00B7 MIDDLE DOT.
    constexpr int bin_bits = sizeof(void*) * 8;
    return kernel + " \xc2\xb7 " + std::to_string(bin_bits) + "-bit userspace";
}

} // namespace helix
