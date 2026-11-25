# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - Cross-Compilation Module
# Handles cross-compilation for embedded ARM targets
#
# Usage:
#   make                       # Native build (SDL)
#   make PLATFORM_TARGET=pi    # Cross-compile for Raspberry Pi (aarch64, DRM+GLES)
#   make PLATFORM_TARGET=pi-fbdev  # Cross-compile for Pi (aarch64, fbdev fallback)
#   make PLATFORM_TARGET=pi32  # Cross-compile for Raspberry Pi (armhf, DRM+GLES)
#   make PLATFORM_TARGET=pi32-fbdev  # Cross-compile for Pi (armhf, fbdev fallback)
#   make PLATFORM_TARGET=pi-both  # Pi 64-bit: compile once, link DRM + fbdev
#   make PLATFORM_TARGET=pi32-both  # Pi 32-bit: compile once, link DRM + fbdev
#   make PLATFORM_TARGET=x86   # Build for x86_64 Debian SBCs (DRM+GLES)
#   make PLATFORM_TARGET=x86-fbdev  # Build for x86_64 Debian SBCs (fbdev fallback)
#   make PLATFORM_TARGET=x86-both   # x86_64: compile once, link DRM + fbdev
#   make PLATFORM_TARGET=x86-desktop # x86_64 Linux desktop: SDL window
#   make pi-docker             # Docker-based Pi build (64-bit, DRM+GLES)
#   make pi-fbdev-docker       # Docker-based Pi build (64-bit, fbdev fallback)
#   make pi-all-docker         # Docker-based Pi build (both variants)
#   make pi32-docker           # Docker-based Pi build (32-bit, DRM+GLES)
#   make pi32-fbdev-docker     # Docker-based Pi build (32-bit, fbdev fallback)
#   make pi32-all-docker       # Docker-based Pi build (both 32-bit variants)
#   make x86-docker            # Docker-based x86_64 build (DRM+GLES)
#   make x86-fbdev-docker      # Docker-based x86_64 build (fbdev fallback)
#   make x86-all-docker        # Docker-based x86_64 build (both variants)
#   make x86-desktop-docker    # Docker-based x86_64 desktop build (SDL window)

# =============================================================================
# Docker Build Configuration
# =============================================================================

# NPROC_DOCKER_RUN: Number of parallel jobs for Docker builds
# Capped at 8 to prevent resource exhaustion in containerized environments
# Can be overridden via environment variable: NPROC_DOCKER_RUN=<value>
_NPROC_HOST := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
NPROC_DOCKER_RUN ?= $(shell echo $$(($(_NPROC_HOST) > 8 ? 8 : $(_NPROC_HOST))))

# =============================================================================
# Target Platform Definitions
# =============================================================================

# Note: We use PLATFORM_TARGET to avoid collision with Makefile's TARGET (binary path)
PLATFORM_TARGET ?= native

# VoronDeck ships on general-purpose Linux hosts, not vendor printer firmware.
VORONDECK_LINUX_TARGETS := native pi pi-fbdev pi-both pi32 pi32-fbdev pi32-both x86 x86-fbdev x86-both x86-desktop yocto
ifeq ($(filter $(PLATFORM_TARGET),$(VORONDECK_LINUX_TARGETS)),)
$(error Unsupported VoronDeck platform: $(PLATFORM_TARGET). Use a general-purpose Linux target)
endif
VORONDECK_VENDOR_TARGETS := ad5m ad5x mips k1 k2 cc1 snapmaker creator5 qidi
VORONDECK_VENDOR_GOALS := $(foreach goal,$(MAKECMDGOALS),$(if $(strip $(foreach vendor,$(VORONDECK_VENDOR_TARGETS),$(findstring $(vendor),$(goal)))),$(goal)))
ifneq ($(strip $(VORONDECK_VENDOR_GOALS)),)
$(error Unsupported vendor targets: $(VORONDECK_VENDOR_GOALS))
endif

ifeq ($(PLATFORM_TARGET),pi)
    # -------------------------------------------------------------------------
    # Raspberry Pi (Mainsail OS) - aarch64 / ARM64
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    # Include paths for cross-compilation:
    # - /usr/aarch64-linux-gnu/include: arm64 sysroot headers
    # - /usr/include/libdrm: drm.h (needed by xf86drmMode.h)
    # -Wno-error=conversion: LVGL headers have int32_t->float conversions that GCC 12 flags
    # -DHELIX_RELEASE_BUILD: Disables debug features like LV_USE_ASSERT_STYLE
    # -funwind-tables: Emit ARM unwind info (.ARM.exidx) so backtrace() can walk
    # the full call stack in crash reports. ~5-10% code size increase, zero runtime cost.
    # -fno-omit-frame-pointer: required for the crash handler's fp_walk_backtrace /
    # stack-scan fallback (libgcc backtrace() can't cross the signal frame on ARM).
    # Without it SIGABRT reports get garbage frames. Matches ad5x/cc1/k1/k2/snapmaker-u1.
    TARGET_CFLAGS := -march=armv8-a -fno-omit-frame-pointer -funwind-tables -I/usr/aarch64-linux-gnu/include -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    # The base build presents through DRM dumb buffers. ENABLE_EGL_RUNG below
    # adds the GPU presentation path to the same binary as a second compile of
    # the few files that read the EGL config; flipping this would make both
    # compiles EGL.
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    # SSL enabled for HTTPS/WSS support
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi
    # Strip binary for size - embedded targets don't need debug symbols
    STRIP_BINARY := yes
    FONT_TIERS := all
    # Also link the EGL presentation path, which the launcher probes for and prefers.
    ENABLE_EGL_RUNG := yes

else ifeq ($(PLATFORM_TARGET),pi-fbdev)
    # -------------------------------------------------------------------------
    # Raspberry Pi (aarch64) - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -fno-omit-frame-pointer -funwind-tables \
        -I/usr/aarch64-linux-gnu/include \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"fbdev\"
    DISPLAY_BACKEND := fbdev
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi-fbdev
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),pi-both)
    # -------------------------------------------------------------------------
    # Raspberry Pi (aarch64) - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/pi/bin/helix-screen (DRM) and build/pi-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= aarch64-linux-gnu-
    TARGET_ARCH := aarch64
    TARGET_TRIPLE := aarch64-linux-gnu
    TARGET_CFLAGS := -march=armv8-a -fno-omit-frame-pointer -funwind-tables -I/usr/aarch64-linux-gnu/include -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi
    STRIP_BINARY := yes
    FONT_TIERS := all
    PI_DUAL_LINK := yes
    ENABLE_EGL_RUNG := yes

else ifeq ($(PLATFORM_TARGET),pi32)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (MainsailOS armhf) - armv7-a hard-float
    # -------------------------------------------------------------------------
    # For 32-bit Raspberry Pi OS / MainsailOS (armv7l).
    # Covers Pi 2/3/4/5 running 32-bit userland.
    # Same strategy as 64-bit Pi: dynamic linking, static OpenSSL, DRM display.
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    # -funwind-tables: Emit ARM unwind info (.ARM.exidx) so backtrace() can walk
    # the full call stack in crash reports. ~5-10% code size increase, zero runtime cost.
    # -fno-omit-frame-pointer: required for the crash handler's fp_walk_backtrace /
    # stack-scan fallback (libgcc backtrace() can't cross the signal frame on ARM).
    # Without it SIGABRT reports get garbage frames. Matches ad5x/cc1/k1/k2/snapmaker-u1.
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-omit-frame-pointer -funwind-tables \
        -I/usr/arm-linux-gnueabihf/include -I/usr/include/libdrm \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_PI32 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi32
    STRIP_BINARY := yes
    FONT_TIERS := all
    # Also link the EGL presentation path, which the launcher probes for and prefers.
    ENABLE_EGL_RUNG := yes

else ifeq ($(PLATFORM_TARGET),pi32-fbdev)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (armhf) - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-omit-frame-pointer -funwind-tables \
        -I/usr/arm-linux-gnueabihf/include \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_PI32 -DHELIX_BINARY_VARIANT=\"fbdev\"
    DISPLAY_BACKEND := fbdev
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi32-fbdev
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),pi32-both)
    # -------------------------------------------------------------------------
    # Raspberry Pi 32-bit (armhf) - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/pi32/bin/helix-screen (DRM) and build/pi32-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    TARGET_ARCH := armv7-a
    TARGET_TRIPLE := arm-linux-gnueabihf
    TARGET_CFLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-omit-frame-pointer -funwind-tables \
        -I/usr/arm-linux-gnueabihf/include -I/usr/include/libdrm \
        -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_PI32 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := pi32
    STRIP_BINARY := yes
    FONT_TIERS := all
    PI_DUAL_LINK := yes
    ENABLE_EGL_RUNG := yes

else ifeq ($(PLATFORM_TARGET),x86)
    # -------------------------------------------------------------------------
    # x86_64 Debian SBCs - DRM/KMS display
    # -------------------------------------------------------------------------
    # Native x86_64 build inside a Bullseye Docker container for glibc 2.31
    # compatibility. Same dynamic-linking strategy as Pi: system glibc,
    # static OpenSSL. Runs on both Debian Bullseye and Bookworm x86_64.
    CROSS_COMPILE :=
    TARGET_ARCH := x86_64
    TARGET_TRIPLE := x86_64-linux-gnu
    # -funwind-tables: Emit unwind info so backtrace() can walk the full
    # call stack in crash reports. Negligible code size cost, zero runtime cost.
    TARGET_CFLAGS := -march=x86-64 -funwind-tables -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_X86 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := x86
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),x86-fbdev)
    # -------------------------------------------------------------------------
    # x86_64 Debian SBCs - fbdev only, no GL dependencies
    # Fallback for systems without EGL/GLES2/GBM libraries.
    # -------------------------------------------------------------------------
    CROSS_COMPILE :=
    TARGET_ARCH := x86_64
    TARGET_TRIPLE := x86_64-linux-gnu
    TARGET_CFLAGS := -march=x86-64 -funwind-tables \
        -Wno-error=conversion -Wno-error=sign-conversion \
        -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_X86 -DHELIX_BINARY_VARIANT=\"fbdev\"
    DISPLAY_BACKEND := fbdev
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := x86-fbdev
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),x86-both)
    # -------------------------------------------------------------------------
    # x86_64 Debian SBCs - Dual-link mode: compile once, link DRM + fbdev
    # Produces both build/x86/bin/helix-screen (DRM) and build/x86-fbdev/bin/helix-screen (fbdev)
    # in a single compilation pass. Used by CI to cut build time in half.
    # -------------------------------------------------------------------------
    CROSS_COMPILE :=
    TARGET_ARCH := x86_64
    TARGET_TRIPLE := x86_64-linux-gnu
    TARGET_CFLAGS := -march=x86-64 -funwind-tables -I/usr/include/libdrm -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_X86 -DHELIX_BINARY_VARIANT=\"drm\"
    DISPLAY_BACKEND := drm
    ENABLE_OPENGLES := no
    ENABLE_SDL := no
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    ENABLE_SSL := yes
    HELIX_HAS_SYSTEMD := yes
    BUILD_SUBDIR := x86
    STRIP_BINARY := yes
    FONT_TIERS := all
    PI_DUAL_LINK := yes

else ifeq ($(PLATFORM_TARGET),x86-desktop)
    # -------------------------------------------------------------------------
    # x86_64 Linux desktops - the app in an SDL window beside the user's desktop
    # -------------------------------------------------------------------------
    # Its own compile, never a relink of x86-both: HELIX_DISPLAY_SDL selects
    # LV_USE_SDL in lv_conf.h, so every LVGL object differs from the DRM build.
    # Built in the same Bullseye container as x86. SDL2 comes from lib/sdl2 and
    # links statically; it dlopens the system's X11/Wayland/PulseAudio/ALSA
    # libraries at runtime, so the binary needs no SDL package installed. The
    # 3D G-code renderer resolves GLES2 the same way (gles2_loader.h): with
    # no GLES on the machine it fails init and the viewer draws in 2D.
    CROSS_COMPILE :=
    TARGET_ARCH := x86_64
    TARGET_TRIPLE := x86_64-linux-gnu
    TARGET_CFLAGS := -march=x86-64 -funwind-tables -Wno-error=conversion -Wno-error=sign-conversion -DHELIX_RELEASE_BUILD -DHELIX_PLATFORM_X86_DESKTOP -DHELIX_BINARY_VARIANT=\"sdl\"
    DISPLAY_BACKEND := sdl
    ENABLE_OPENGLES := no
    ENABLE_SDL := yes
    ENABLE_GLES_3D := yes
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := no
    ENABLE_SSL := yes
    BUILD_SUBDIR := x86-desktop
    STRIP_BINARY := yes
    FONT_TIERS := all

else ifeq ($(PLATFORM_TARGET),yocto)
    # -------------------------------------------------------------------------
    # Yocto / BitBake cross-build (OpenCentauri COSMOS → Centauri Carbon 1 today;
    # generic enough for any future Yocto machine/BSP).
    #
    # Bitbake's EXTRA_OEMAKE passes CC/CXX/AR/LD/STRIP/RANLIB as full paths and
    # populates CFLAGS/CXXFLAGS/LDFLAGS/PKG_CONFIG env with the target sysroot,
    # --march, security, and optimization flags. We add nothing that could
    # conflict — the recipe's DEPENDS supplies every vendored library we'd
    # otherwise build from submodule (libhv, spdlog, fmt, alsa-lib, libdrm,
    # libusb, libnl, wpa-supplicant, etc.). See docs/devel/printers/ELEGOO_CENTAURI_CARBON_YOCTO.md.
    # -------------------------------------------------------------------------
    # Bitbake passes CROSS_COMPILE=yocto- as a sentinel; clear it so the
    # prefix-based CC/CXX override below does not run (bitbake's CC already
    # has the full path + machine flags baked in).
    override CROSS_COMPILE :=
    TARGET_ARCH := yocto
    TARGET_TRIPLE := yocto
    # Bitbake drives CFLAGS/LDFLAGS via env; don't add platform flags here.
    TARGET_CFLAGS :=
    TARGET_LDFLAGS :=
    ENABLE_SSL := yes
    DISPLAY_BACKEND := fbdev
    ENABLE_SDL := no
    ENABLE_GLES_3D := no
    # Same board as cc1 (Centauri Carbon 1), so it carries savers on the same evidence.
    ENABLE_SCREENSAVER := yes
    ENABLE_EVDEV := yes
    BUILD_SUBDIR := yocto
    # Don't strip — bitbake's package split handles debug/strip via FILES:${PN}-dbg.
    STRIP_BINARY := no
    # CC1 has 112MB RAM; trim fonts like the existing cc1 target.
    FONT_TIERS := micro tiny
    # Skip all in-tree submodule dep builds — Yocto recipe provides via DEPENDS.
    YOCTO_BUILD := yes

else ifeq ($(PLATFORM_TARGET),native)
    # -------------------------------------------------------------------------
    # Native desktop build (macOS / Linux)
    # -------------------------------------------------------------------------
    CROSS_COMPILE :=
    TARGET_ARCH := $(shell uname -m)
    TARGET_TRIPLE :=
    TARGET_CFLAGS :=
    DISPLAY_BACKEND := sdl
    ENABLE_SSL := yes
    ENABLE_SDL := yes
    ENABLE_EVDEV := no
    BUILD_SUBDIR :=

else
    $(error Unknown PLATFORM_TARGET: $(PLATFORM_TARGET). Valid options: native, pi, pi32, x86, x86-desktop, yocto)
endif

# pi32 compiles against Debian armhf (armv7) headers, whose shared_ptr uses the
# atomic lock policy. Raspberry Pi OS 32-bit is Raspbian, built for armv6, whose
# libstdc++.so.6 uses the mutex policy: a control block 24 bytes larger, with the
# refcount at a different offset. Every refcount the app inlines on an object the
# shared library owns (a std::filesystem::directory_iterator, for one) then lands
# on the wrong word, and on freed memory once the library drops it - glibc's
# "malloc(): unsorted double linked list corrupted" (#1732). Carrying our own
# libstdc++ makes the inlined code and the library one build.
ifneq ($(filter pi32 pi32-fbdev pi32-both,$(PLATFORM_TARGET)),)
    TARGET_LDFLAGS += -static-libstdc++ -static-libgcc
endif

# The Pi and x86 targets drop unreferenced code and data at link time, as every
# embedded target above does: each function and object gets its own section and
# the linker keeps only what is reachable from the entry point, .init_array and
# the dynamic exports. That is safe because nothing resolves an app symbol by
# name at runtime: the binaries export none (no -rdynamic), the Bluetooth plugin
# imports only from its own libraries, and Lua bindings and XML callbacks are
# registered as function pointers.
ifneq ($(filter pi pi-fbdev pi-both pi32 pi32-fbdev pi32-both x86 x86-fbdev x86-both x86-desktop,$(PLATFORM_TARGET)),)
    TARGET_CFLAGS += -ffunction-sections -fdata-sections
    TARGET_LDFLAGS += -Wl,--gc-sections
endif

# The Pi and x86 DRM builds load libdrm, libinput, libEGL, libGLESv2 and libgbm
# at runtime instead of linking them (mk/implib.mk), so the one binary starts on
# a system without them and falls back to fbdev. include/gpu_libs.h is the
# runtime gate every call into them goes through.
ifneq ($(filter pi pi-both pi32 pi32-both x86 x86-both,$(PLATFORM_TARGET)),)
    DLOPEN_GPU_LIBS := yes
endif

# =============================================================================
# Cross-Compiler Configuration
# =============================================================================

ifneq ($(CROSS_COMPILE),)
    # Override compilers for cross-compilation
    CC := $(CROSS_COMPILE)gcc
    CXX := $(CROSS_COMPILE)g++
    # Use gcc-ar and gcc-ranlib for LTO compatibility (they load the LTO plugin)
    AR := $(CROSS_COMPILE)gcc-ar
    STRIP := $(CROSS_COMPILE)strip
    RANLIB := $(CROSS_COMPILE)gcc-ranlib
    LD := $(CROSS_COMPILE)ld

    # Re-apply ccache wrapping: the CC/CXX assignments above clobber the
    # ccache prefix the main Makefile sets (~line 143), so without this
    # every cross-compile is a cold build. Observed symptom: CI ccache stats
    # showed 0 hits / 0 misses / 0 kB after ~60 minute Pi builds.
    ifneq ($(shell command -v ccache 2>/dev/null),)
        CC := ccache $(CC)
        CXX := ccache $(CXX)
    endif
endif

# Override build directories when BUILD_SUBDIR is set (cross-compilation or
# native platform targets like x86-both where CROSS_COMPILE is empty).
# When SANITIZE=address, suffix with -asan so instrumented builds don't
# clobber the regular build tree (different objects, different libhv.a).
ifneq ($(BUILD_SUBDIR),)
    ifeq ($(SANITIZE),address)
        BUILD_SUBDIR := $(BUILD_SUBDIR)-asan
    endif
    ifeq ($(SANITIZE),thread)
        BUILD_SUBDIR := $(BUILD_SUBDIR)-tsan
    endif
    BUILD_DIR := build/$(BUILD_SUBDIR)
    BIN_DIR := $(BUILD_DIR)/bin
    OBJ_DIR := $(BUILD_DIR)/obj
endif

# Print platform info when cross-compiling or targeting a specific platform
ifneq ($(PLATFORM_TARGET),native)
ifneq ($(PLATFORM_TARGET),)
    $(info )
    $(info ========================================)
    $(info Building for: $(PLATFORM_TARGET))
    $(info Architecture: $(TARGET_ARCH))
    $(info Compiler: $(CC))
    $(info Output: $(BUILD_DIR))
    $(info ========================================)
    $(info )
endif
endif

# =============================================================================
# Target-Specific Flags
# =============================================================================

ifdef TARGET_CFLAGS
    CFLAGS += $(TARGET_CFLAGS)
    CXXFLAGS += $(TARGET_CFLAGS)
    SUBMODULE_CFLAGS += $(TARGET_CFLAGS)
    SUBMODULE_CXXFLAGS += $(TARGET_CFLAGS)
endif

# =============================================================================
# HELIX_MAX_FONT_TIER - compile-time maximum font tier linked for this platform
# =============================================================================
# Derived from FONT_TIERS (set per-platform above). Used by theme_manager to
# distinguish expected-missing fonts (pruned by tier) from unexpected-missing
# fonts (build bug). Tier numbers: micro=0, tiny=1, small=2, medium=3, large=4,
# xlarge=5, xxlarge=6.
ifeq ($(FONT_TIERS),all)
    HELIX_MAX_FONT_TIER := 6
else ifneq ($(filter xxlarge,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 6
else ifneq ($(filter xlarge,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 5
else ifneq ($(filter large,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 4
else ifneq ($(filter medium,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 3
else ifneq ($(filter small,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 2
else ifneq ($(filter tiny,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 1
else ifneq ($(filter micro,$(FONT_TIERS)),)
    HELIX_MAX_FONT_TIER := 0
else
    HELIX_MAX_FONT_TIER := 6
endif
CFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)
CXXFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)
SUBMODULE_CFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)
SUBMODULE_CXXFLAGS += -DHELIX_MAX_FONT_TIER=$(HELIX_MAX_FONT_TIER)

# =============================================================================
# Size flags for device builds
# =============================================================================
# -fno-rtti  on every non-native target. The codebase is RTTI-free by policy
#            and lint-enforced (tests/shell/test_code_lint.bats); a grep for
#            non-comment typeid/dynamic_cast across src/ and include/ returns
#            zero. The ESP32 firmware builds the same way via ESP-IDF's
#            CONFIG_COMPILER_CXX_RTTI. Applied to our C++ only, NOT to
#            SUBMODULE_CXXFLAGS -- libhv throws, and its catch clauses want
#            typeinfo for the thrown types. Native host builds keep RTTI.
ifneq ($(PLATFORM_TARGET),native)
    CXXFLAGS += -fno-rtti
endif

# The memory-constrained boards also drop asserts. On these devices
# helix-screen's file-backed text competes for page cache with Klipper: measured
# on a CC1, Klipper takes 3675 major faults and Moonraker 5526 while helix-screen
# takes 68, because helix-screen's working set is what drives the reclaim. A
# Klipper stalled on flash IO is a "Timer too close".
#
# -DNDEBUG   drops assert() and nlohmann's JSON_ASSERT. LVGL's asserts are
#            controlled separately by LV_USE_ASSERT_* and are unaffected.
# ad5m-br is the same AD5M hardware built with the buildroot-provided
# toolchain (kmod) rather than the Docker cross-toolchain, so it sits in the
# same 110-128MB class as ad5m and cc1 and gets the same treatment.
ifneq (,$(filter cc1 ad5m ad5m-br,$(PLATFORM_TARGET)))
    CFLAGS += -DNDEBUG
    CXXFLAGS += -DNDEBUG
    SUBMODULE_CFLAGS += -DNDEBUG
    SUBMODULE_CXXFLAGS += -DNDEBUG
endif

# For size-optimized targets, override -O2 with -Os
# (GCC uses last optimization flag, but this makes it explicit)





ifdef TARGET_LDFLAGS
    LDFLAGS += $(TARGET_LDFLAGS)
endif

# Apply AddressSanitizer flags AFTER target/submodule flag merge so the main
# binary AND in-tree subprojects (LVGL, helix-xml, ThorVG) all get
# instrumented. For cross-compile, statically link libasan so the deployed
# binary is self-contained and the device doesn't need libasan installed.
# Strip is disabled because ASAN reports rely on symbol info in the binary.
ifeq ($(SANITIZE),address)
    CFLAGS += $(SANITIZE_FLAGS)
    CXXFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CXXFLAGS += $(SANITIZE_FLAGS)
    LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        LDFLAGS += -static-libasan
    endif
    STRIP_BINARY := no
endif

# Same treatment for ThreadSanitizer. Instrumenting the submodules matters here
# as much as it does for ASAN: a race is only reported when TSAN sees BOTH
# accesses, so an uninstrumented LVGL or libhv turns a real race into silence.
ifeq ($(SANITIZE),thread)
    CFLAGS += $(SANITIZE_FLAGS)
    CXXFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CFLAGS += $(SANITIZE_FLAGS)
    SUBMODULE_CXXFLAGS += $(SANITIZE_FLAGS)
    LDFLAGS += $(SANITIZE_FLAGS)
    ifneq ($(CROSS_COMPILE),)
        LDFLAGS += -static-libtsan
    endif
    STRIP_BINARY := no
endif

# Display backend defines (used by display_backend.cpp and lv_conf.h for conditional compilation)
# Must be added to SUBMODULE_*FLAGS as well for LVGL driver compilation
ifeq ($(DISPLAY_BACKEND),drm)
    CFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    CXXFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CXXFLAGS += -DHELIX_DISPLAY_DRM -DHELIX_DISPLAY_FBDEV
    ifeq ($(DLOPEN_GPU_LIBS),yes)
        CFLAGS += -DHELIX_DLOPEN_GPU_LIBS
        CXXFLAGS += -DHELIX_DLOPEN_GPU_LIBS
    endif
    # GPU-accelerated rendering via EGL/OpenGL ES (Pi targets)
    ifeq ($(ENABLE_OPENGLES),yes)
        CFLAGS += -DHELIX_ENABLE_OPENGLES
        CXXFLAGS += -DHELIX_ENABLE_OPENGLES
        SUBMODULE_CFLAGS += -DHELIX_ENABLE_OPENGLES
        SUBMODULE_CXXFLAGS += -DHELIX_ENABLE_OPENGLES
    endif
    # NanoVG draw unit. Rasterizes on the GPU, so it needs the EGL presentation
    # path above; it cannot render onto a dumb-buffer or fbdev framebuffer.
    # It also grows lv_layer_t and lv_draw_task_t, so objects built with it
    # cannot be linked against objects built without it.
    ifeq ($(ENABLE_NANOVG),yes)
        CFLAGS += -DHELIX_ENABLE_NANOVG
        CXXFLAGS += -DHELIX_ENABLE_NANOVG
        SUBMODULE_CFLAGS += -DHELIX_ENABLE_NANOVG
        SUBMODULE_CXXFLAGS += -DHELIX_ENABLE_NANOVG
    endif
    # DRM backend linker flags are added in Makefile's cross-compile section
else ifeq ($(DISPLAY_BACKEND),fbdev)
    CFLAGS += -DHELIX_DISPLAY_FBDEV
    CXXFLAGS += -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CFLAGS += -DHELIX_DISPLAY_FBDEV
    SUBMODULE_CXXFLAGS += -DHELIX_DISPLAY_FBDEV
else ifeq ($(DISPLAY_BACKEND),sdl)
    CFLAGS += -DHELIX_DISPLAY_SDL
    CXXFLAGS += -DHELIX_DISPLAY_SDL
    SUBMODULE_CFLAGS += -DHELIX_DISPLAY_SDL
    SUBMODULE_CXXFLAGS += -DHELIX_DISPLAY_SDL
endif

# Evdev input support
ifeq ($(ENABLE_EVDEV),yes)
    CFLAGS += -DHELIX_INPUT_EVDEV
    CXXFLAGS += -DHELIX_INPUT_EVDEV
    SUBMODULE_CFLAGS += -DHELIX_INPUT_EVDEV
    SUBMODULE_CXXFLAGS += -DHELIX_INPUT_EVDEV
endif

# NOTE: LV_COLOR_DEPTH is platform-conditional in lv_conf.h.
# Constrained FBDEV devices (AD5M, CC1, K1/MIPS, AD5X, K2, U1) use 16 (RGB565).
# Pi (DRM) and desktop (SDL) use 32 (ARGB8888).

# =============================================================================
# Cross-Compilation Build Targets
# =============================================================================

.PHONY: pi pi-both pi32 pi32-both x86 x86-both pi-docker pi32-docker x86-docker x86-fbdev-docker x86-all-docker x86-desktop-docker docker-toolchains docker-toolchain-x86 cross-info ensure-docker ensure-buildx maybe-stop-colima

# Persistent ccache for Docker builds — bind-mounts a host directory so the
# cache survives across container runs (the container is --rm).  Per-platform
# subdirectories avoid cross-toolchain collisions.  The host dir is created
# on first use with the current user's ownership (no root permission issues).
DOCKER_CCACHE_BASE ?= $(HOME)/.cache/helixscreen-ccache
docker-ccache-args = -v "$(DOCKER_CCACHE_BASE)/$(1)":/ccache -e CCACHE_DIR=/ccache
ensure-ccache-dir = @mkdir -p "$(DOCKER_CCACHE_BASE)/$(1)"

# Worktree cross-build support.
# scripts/setup-worktree.sh symlinks the unpatched lib/<submodule> entries to the
# main checkout (absolute paths) so a worktree builds fast without duplicating ~GB
# of submodules. But Docker only bind-mounts $(CURDIR) at /src, so those absolute
# symlinks dangle inside the container — the dependency check reports a missing
# library and relative includes resolve to the wrong tree. Also bind-mount the real
# directory each of them points into, so every lib/* symlink resolves identically
# inside and outside the container.
#
# Ask every lib/ entry where it really lives rather than probing one of them:
# lvgl, libhv, lua and helix-xml are private per-worktree checkouts that live under
# $(CURDIR), so a single-entry probe reads a worktree full of symlinks as a normal
# checkout and mounts nothing. Empty for a normal checkout, where every entry is
# already under $(CURDIR).
WORKTREE_LIB_REAL := $(sort $(foreach e,$(wildcard lib/*),\
    $(if $(findstring $(CURDIR)/,$(realpath $(e))/),,$(patsubst %/,%,$(dir $(realpath $(e)))))))
DOCKER_WORKTREE_MOUNT := $(foreach p,$(WORKTREE_LIB_REAL),-v "$(p)":"$(p)")

# Build provenance. The same mount boundary hides the git metadata: a worktree's
# .git is a FILE reading "gitdir: $(MAIN)/.git/worktrees/<name>", and that path
# is not under $(CURDIR), so git cannot resolve HEAD inside the container.
# scripts/gen-git-hash.sh fell back to "unknown" and the deployed binary could
# not say which commit produced it — silently, because the build still succeeds.
# Resolve it on the host, where git always works, and hand it to the container.
# Recursive '=' so a native build never pays for the git call.
# $(or) short-circuits, so an inherited HELIX_GIT_HASH (mk/remote.mk sets it over
# ssh; a release pipeline can too) skips the git call entirely — which matters
# because the build host may have no .git at all.
HELIX_GIT_HASH_HOST = $(or $(HELIX_GIT_HASH),$(shell git -c safe.directory='*' rev-parse --short HEAD 2>/dev/null))
DOCKER_GIT_HASH_ENV = $(if $(HELIX_GIT_HASH_HOST),-e HELIX_GIT_HASH=$(HELIX_GIT_HASH_HOST))

# Everything a container needs from the host beyond -v "$(CURDIR)":/src. Every
# `docker run` below must pass this — tests/shell/test_build_provenance.bats
# fails the build if one does not, so a new platform target cannot regress
# provenance or worktree support by omission.
# Forward the remote-control choice into the container. The inner make cannot
# work it out for itself: HELIX_PACKAGING is a target-specific variable, so it
# exists only in the outer make's recipe expansion and never crosses the
# `docker run` boundary.
#
# The caller's own value wins when there is one. `$(if $(ENABLE_REMOTE_CONTROL),
# ...)` cannot detect that and looks like it can: the outer make runs with
# PLATFORM_TARGET unset, i.e. native, where the Makefile defaults the variable,
# so it is never empty. $(origin) is what distinguishes "the user chose this"
# (command line / environment / override) from "a makefile defaulted it" (file /
# default / undefined).
CROSS_REMOTE_CONTROL_DEFAULT = $(if $(filter 1,$(HELIX_PACKAGING)),no,yes)
DOCKER_REMOTE_CONTROL = ENABLE_REMOTE_CONTROL=$(if $(filter-out default file undefined,$(origin ENABLE_REMOTE_CONTROL)),$(ENABLE_REMOTE_CONTROL),$(CROSS_REMOTE_CONTROL_DEFAULT))

# Forwards ENABLE_SCREENSAVER into a Docker build when it was given on the command line, for
# measurement builds on boards that do not ship screensavers.
DOCKER_SCREENSAVER = $(if $(filter command line,$(origin ENABLE_SCREENSAVER)),ENABLE_SCREENSAVER=$(ENABLE_SCREENSAVER))

# Same forwarding for the diagnostic-upload gate (see the
# ENABLE_DIAGNOSTIC_UPLOADS block in Makefile). Inside the container only the
# define matters — the runtime switch is env-based — so what crosses is the
# compile default: yes under HELIX_PACKAGING=1 (official builds are marked),
# no otherwise. A dev build deployed from this tree gets its upload switch
# stamped on the device by sync-device-features instead.
CROSS_DIAG_UPLOADS_DEFAULT = $(if $(filter 1,$(HELIX_PACKAGING)),yes,no)
DOCKER_DIAG_UPLOADS = ENABLE_DIAGNOSTIC_UPLOADS=$(if $(filter-out default file undefined,$(origin ENABLE_DIAGNOSTIC_UPLOADS)),$(ENABLE_DIAGNOSTIC_UPLOADS),$(CROSS_DIAG_UPLOADS_DEFAULT))

# Same forwarding for the mock backends, which packaged builds leave out. Only
# a "no" (or the caller's own value) crosses: a command-line ENABLE_MOCKS=yes
# would beat the `ENABLE_MOCKS := no` the lean targets above set for dev builds.
DOCKER_MOCKS = $(if $(filter-out default file undefined,$(origin ENABLE_MOCKS)),ENABLE_MOCKS=$(ENABLE_MOCKS),$(if $(filter 1,$(HELIX_PACKAGING)),ENABLE_MOCKS=no))

# SKIP_COMPILE_COMMANDS: the container's fragments name /src, so a merge there
# writes a tree-root compile_commands.json the host's syntax check cannot use.
# --init: without it the container's make is PID 1, which no make is written to
# be. Every orphan in the build reparents to it and stays a zombie whenever it is
# not sitting in wait() (prestonbrown/helixscreen#1583).
DOCKER_HOST_CONTEXT = --init $(DOCKER_WORKTREE_MOUNT) $(DOCKER_GIT_HASH_ENV) -e SKIP_COMPILE_COMMANDS=1

# Direct cross-compilation (requires toolchain installed)
# A sub-make given -j leaves its parent's jobserver, so under a live jobpool
# (MAKEFLAGS carries the pool) it inherits the pool instead of taking -j$(NPROC).
SUBMAKE_J = $(if $(and $(findstring --jobserver-auth,$(MAKEFLAGS)),$(shell scripts/helix-claim pool 2>/dev/null)),,-j$(NPROC))
pi:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi (aarch64)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=pi $(SUBMAKE_J) all

pi32:
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi 32-bit (armv7-a)...$(RESET)"
	$(Q)$(MAKE) PLATFORM_TARGET=pi32 $(SUBMAKE_J) all

# Docker-based cross-compilation (recommended)
# SKIP_OPTIONAL_DEPS=1 skips npm, clang-format, python venv, and other development tools

# Helper target to ensure Docker daemon is running
# On macOS with Colima, automatically starts it with resources based on host hardware
# Allocates ~50% of system RAM (min 6GB, max 16GB) and ~50% of CPUs (min 2, max 8)
# Handles zombie Colima state (VM running but Docker socket dead) with automatic restart
.PHONY: ensure-docker
ensure-docker:
	@if docker info >/dev/null 2>&1; then \
		exit 0; \
	fi; \
	if [ "$(UNAME_S)" = "Darwin" ]; then \
		if command -v colima >/dev/null 2>&1; then \
			TOTAL_RAM_GB=$$(( $$(sysctl -n hw.memsize) / 1073741824 )); \
			TOTAL_CPUS=$$(sysctl -n hw.ncpu); \
			COLIMA_RAM=$$(( TOTAL_RAM_GB / 2 )); \
			COLIMA_CPUS=$$(( TOTAL_CPUS / 2 )); \
			[ $$COLIMA_RAM -lt 6 ] && COLIMA_RAM=6; \
			[ $$COLIMA_RAM -gt 16 ] && COLIMA_RAM=16; \
			[ $$COLIMA_CPUS -lt 2 ] && COLIMA_CPUS=2; \
			[ $$COLIMA_CPUS -gt 8 ] && COLIMA_CPUS=8; \
			echo "$(YELLOW)Docker not running. Starting Colima ($${COLIMA_RAM}GB RAM, $${COLIMA_CPUS} CPUs)...$(RESET)"; \
			echo "$(CYAN)  (Based on host: $${TOTAL_RAM_GB}GB RAM, $${TOTAL_CPUS} CPUs)$(RESET)"; \
			if colima list 2>/dev/null | grep -q "default"; then \
				CURRENT_RAM=$$(colima list 2>/dev/null | awk '/default/ {gsub(/GiB/,""); print $$5}'); \
				if [ "$$CURRENT_RAM" != "" ] && [ "$$CURRENT_RAM" -lt "$$COLIMA_RAM" ]; then \
					echo "$(YELLOW)⚠ Existing Colima VM has $${CURRENT_RAM}GB RAM (need $${COLIMA_RAM}GB)$(RESET)"; \
					echo "$(YELLOW)  Run 'colima delete' then retry to resize$(RESET)"; \
				fi; \
			fi; \
			colima start --memory $$COLIMA_RAM --cpu $$COLIMA_CPUS && echo "$(GREEN)✓ Colima started$(RESET)"; \
			if docker info >/dev/null 2>&1; then \
				exit 0; \
			fi; \
			echo "$(YELLOW)Docker socket not responding after start. Restarting Colima...$(RESET)"; \
			colima stop 2>/dev/null || true; \
			sleep 2; \
			colima start --memory $$COLIMA_RAM --cpu $$COLIMA_CPUS; \
			if docker info >/dev/null 2>&1; then \
				echo "$(GREEN)✓ Colima restarted successfully$(RESET)"; \
				exit 0; \
			fi; \
			echo "$(RED)Docker still not responding after Colima restart.$(RESET)"; \
			echo "$(YELLOW)Try manually: colima delete && colima start$(RESET)"; \
			exit 1; \
		elif [ -e "/Applications/Docker.app" ]; then \
			echo "$(RED)Docker Desktop is installed but not running.$(RESET)"; \
			echo "$(YELLOW)Please start Docker Desktop from your Applications folder.$(RESET)"; \
			exit 1; \
		else \
			echo "$(RED)Docker is not installed.$(RESET)"; \
			echo "$(YELLOW)Install with: brew install colima docker docker-buildx && colima start$(RESET)"; \
			exit 1; \
		fi; \
	else \
		echo "$(RED)Docker daemon is not running.$(RESET)"; \
		echo "$(YELLOW)Start it with: sudo systemctl start docker$(RESET)"; \
		exit 1; \
	fi

# Helper target to ensure Docker BuildKit/buildx is available
# BuildKit is Docker's modern image builder with better caching and parallel builds
# The legacy builder is deprecated and will be removed in a future Docker release
.PHONY: ensure-buildx
ensure-buildx: ensure-docker
	@if docker buildx version >/dev/null 2>&1; then \
		exit 0; \
	fi; \
	echo "$(YELLOW)Docker BuildKit (buildx) not found.$(RESET)"; \
	echo "The legacy Docker builder is deprecated and will be removed."; \
	if [ "$(UNAME_S)" = "Darwin" ]; then \
		echo "$(YELLOW)Install with: brew install docker-buildx$(RESET)"; \
	else \
		echo "$(YELLOW)See: https://docs.docker.com/go/buildx/$(RESET)"; \
	fi; \
	exit 1

pi-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

# AddressSanitizer build for the Pi (DRM). Output lands in build/pi-asan/.
# Uses -static-libasan (via mk/cross.mk SANITIZE block) so the binary is
# self-contained — no libasan install needed on the device. For sharper stack
# traces, override OPT: `make pi-asan-docker OPT=1`.
pi-asan-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi with AddressSanitizer via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi-asan)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi-asan) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi SANITIZE=address SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi-fbdev-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi fbdev fallback via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi-fbdev)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi-fbdev) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi-fbdev SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi-all-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi (DRM + fbdev) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi; \
	fi
	$(call ensure-ccache-dir,pi)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi) helixscreen/toolchain-pi \
		make PLATFORM_TARGET=pi-both SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling for Raspberry Pi 32-bit via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32 SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

# AddressSanitizer build for the Pi 32-bit (DRM). Output lands in build/pi32-asan/.
# Uses -static-libasan (via mk/cross.mk SANITIZE block) so the binary is
# self-contained — no libasan install needed on the device. For sharper stack
# traces, override OPT: `make pi32-asan-docker OPT=1`.
pi32-asan-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit with AddressSanitizer via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32-asan)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32-asan) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32 SANITIZE=address SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-fbdev-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit fbdev fallback via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32-fbdev)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32-fbdev) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32-fbdev SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

pi32-all-docker: ensure-docker
	@echo "$(CYAN)$(BOLD)Cross-compiling Pi 32-bit (DRM + fbdev) via Docker...$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-pi32 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-pi32; \
	fi
	$(call ensure-ccache-dir,pi32)
	$(Q)scripts/pool-docker.sh docker run --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,pi32) helixscreen/toolchain-pi32 \
		make PLATFORM_TARGET=pi32-both SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima

# Copy the toolchain image's CA bundle to build/<plat>/certs, where release-*
# and deploy-* pick it up as the HTTPS fallback for devices without system certs.
# $(1) = platform key (also the toolchain image suffix)
define extract-ca-certs
	@mkdir -p build/$(1)/certs
	@docker run --rm helixscreen/toolchain-$(1) cat /etc/ssl/certs/ca-certificates.crt > build/$(1)/certs/ca-certificates.crt 2>/dev/null \
		&& echo "$(GREEN)✓ CA certificates extracted$(RESET)" \
		|| echo "$(YELLOW)⚠ Could not extract CA certificates (HTTPS may rely on device certs)$(RESET)"
endef

# ad5x is a spelling of the unified mips target: same toolchain image, same
# binary out of build/mips. Kept so `make ad5x-docker` muscle memory and the
# docs that name it keep working.
# Cross-build the static armv7 ustreamer (MJPEG camera server) for K2.
# Runs the vendored build script inside the K2 toolchain image. Idempotent:
# scripts/ustreamer/build-ustreamer-k2.sh skips the build if a valid static
# ARM binary already sits at build/k2/bin/ustreamer (FORCE_REBUILD=1 to force).
# Source tarballs are cached under build/.cache/ustreamer; checksums verified.
# Output lands at build/k2/bin/ustreamer so release-k2 picks it up.
# One recipe for every build in the x86 toolchain image.
# $(1) = banner, $(2) = ccache dir, $(3) = PLATFORM_TARGET.
define x86-docker-build
	@echo "$(CYAN)$(BOLD)$(1)$(RESET)"
	@if ! docker image inspect helixscreen/toolchain-x86 >/dev/null 2>&1; then \
		echo "$(YELLOW)Docker image not found. Building toolchain first...$(RESET)"; \
		$(MAKE) docker-toolchain-x86; \
	fi
	$(call ensure-ccache-dir,$(2))
	$(Q)scripts/pool-docker.sh docker run --platform linux/amd64 --rm --user $$(id -u):$$(id -g) -v "$(CURDIR)":/src $(DOCKER_HOST_CONTEXT) -w /src $(call docker-ccache-args,$(2)) helixscreen/toolchain-x86 \
		make PLATFORM_TARGET=$(3) SKIP_OPTIONAL_DEPS=1 $(DOCKER_REMOTE_CONTROL) $(DOCKER_DIAG_UPLOADS) $(DOCKER_MOCKS) -j$(NPROC_DOCKER_RUN)
	@$(MAKE) --no-print-directory maybe-stop-colima
endef

x86-docker: ensure-docker
	$(call x86-docker-build,Building for x86_64 Debian via Docker...,x86,x86)

x86-fbdev-docker: ensure-docker
	$(call x86-docker-build,Building x86_64 fbdev fallback via Docker...,x86-fbdev,x86-fbdev)

x86-all-docker: ensure-docker
	$(call x86-docker-build,Building x86_64 (DRM + fbdev) via Docker...,x86,x86-both)

x86-desktop-docker: ensure-docker
	$(call x86-docker-build,Building x86_64 Linux desktop (SDL) via Docker...,x86-desktop,x86-desktop)

# Stop Colima after build to free up RAM (macOS only)
# Only stops if Colima is running and we're on macOS
.PHONY: maybe-stop-colima
maybe-stop-colima:
	@if [ "$(UNAME_S)" = "Darwin" ] && command -v colima >/dev/null 2>&1; then \
		if colima status >/dev/null 2>&1; then \
			echo "$(CYAN)Stopping Colima to free up RAM...$(RESET)"; \
			colima stop && echo "$(GREEN)✓ Colima stopped$(RESET)"; \
		fi; \
	fi

# Build Docker toolchain images
docker-toolchains: docker-toolchain-pi docker-toolchain-pi32 docker-toolchain-x86
	@echo "$(GREEN)$(BOLD)All Docker toolchains built successfully$(RESET)"

docker-toolchain-pi: ensure-buildx
	@echo "$(CYAN)Building Raspberry Pi toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-pi -f docker/Dockerfile.pi docker/

docker-toolchain-pi32: ensure-buildx
	@echo "$(CYAN)Building Raspberry Pi 32-bit toolchain Docker image...$(RESET)"
	$(Q)docker buildx build -t helixscreen/toolchain-pi32 -f docker/Dockerfile.pi32 docker/

docker-toolchain-x86: ensure-buildx
	@echo "$(CYAN)Building x86_64 Debian toolchain Docker image...$(RESET)"
	$(Q)docker buildx build --platform linux/amd64 -t helixscreen/toolchain-x86 -f docker/Dockerfile.x86 docker/

# Display cross-compilation info (alias for help-cross)
cross-info: help-cross

# Cross-compilation help
.PHONY: help-cross
help-cross:
	@echo "VoronDeck Linux builds / Сборки Linux"
	@echo "  pi-docker / pi32-docker    ARM64 / ARM32 SBC (Orange Pi, Raspberry Pi, others)"
	@echo "  x86-docker                x86_64 Linux (DRM+GLES)"
	@echo "  x86-desktop-docker        x86_64 Linux (windowed)"
	@echo "  docker-toolchains         Linux toolchain images"
	@echo "  release-all / package-all Linux packages"
	@echo "  ./install.sh              VoronDeck installer / Установщик VoronDeck"
	@echo "  Platform: $(PLATFORM_TARGET), display: $(DISPLAY_BACKEND)"

# =============================================================================
# Common Deployment Settings
# =============================================================================

# Rsync flags for asset sync: delete stale files, checksum-based skip, exclude junk
DEPLOY_RSYNC_FLAGS := -avzz --delete --checksum
# Runtime state files: generated at runtime (crash dumps, telemetry queues, tool
# mappings). Shipping these from a dev machine to a target overwrites the
# target's real state with dev-machine state — or worse, leaves a stale
# crash_report.txt on a healthy device that triggers the "previously crashed"
# modal on next boot. These are gitignored but gitignore doesn't affect rsync.
DEPLOY_RUNTIME_EXCLUDES := --exclude='crash.txt' --exclude='crash_*.txt' --exclude='crash_report.txt' --exclude='.crash_restart_count' --exclude='telemetry_*.json' --exclude='tool_spools.json' --exclude='filament_slot_overrides.json' --exclude='user_filaments.json' --exclude='crash_history.json' --exclude='.helix-screen.lock'
DEPLOY_ASSET_EXCLUDES := --exclude='test_gcodes' --exclude='gcode' --exclude='.DS_Store' --exclude='*.pyc' --exclude='settings*.json' --exclude='helixconfig*.json' --exclude='helixscreen.env' --exclude='._*' \
	--exclude='assets/fonts/*.c' --exclude='assets/fonts/*.ttf' --exclude='assets/fonts/*.otf' --exclude='assets/fonts/.clang-format' \
	--exclude='*.icns' --exclude='mdi-icon-metadata.json.gz' --exclude='moonraker-plugin/tests' \
	$(DEPLOY_RUNTIME_EXCLUDES)
# Zero the owner/group recorded in every archive we create.
#
# A bare `tar -czvf` stamps each entry with the build machine's numeric uid/gid
# (1001 for the GitHub Actions runner, 1000 for a local build host). Root
# extracts with --same-owner by default on both GNU and BusyBox tar, so the
# install lands owned by a uid that has no /etc/passwd entry on the printer.
#
# GNU tar and bsdtar spell this differently and neither accepts the other's
# form, and release packaging runs on both (Linux in CI, macOS locally) — so
# probe once here rather than guessing.
TAR_OWNER_FLAGS := $(shell if tar --owner=0 --group=0 -cf /dev/null -T /dev/null >/dev/null 2>&1; \
	then echo --owner=0 --group=0 --numeric-owner; \
	else echo --uid 0 --gid 0 --numeric-owner; fi)

# Tar-compatible excludes (same patterns, different syntax)
DEPLOY_TAR_EXCLUDES := --exclude='test_gcodes' --exclude='gcode' --exclude='.DS_Store' --exclude='*.pyc' --exclude='settings*.json' --exclude='helixconfig*.json' --exclude='helixscreen.env' --exclude='._*' \
	--exclude='assets/fonts/*.c' --exclude='assets/fonts/*.ttf' --exclude='assets/fonts/*.otf' --exclude='assets/fonts/.clang-format' \
	--exclude='*.icns' --exclude='mdi-icon-metadata.json.gz' --exclude='moonraker-plugin/tests' \
	$(DEPLOY_RUNTIME_EXCLUDES)
# Exclude tracker files (MOD) on platforms without HELIX_HAS_TRACKER
# rsync syntax:
DEPLOY_NO_TRACKER := --exclude='*.mod'
# tar syntax:
DEPLOY_TAR_NO_TRACKER := --exclude='*.mod'
DEPLOY_ASSET_DIRS := ui_xml assets config moonraker-plugin

# Binaries helix-screen now carries itself: the EGL rung and the fbdev rung.
# An older deploy's copies would otherwise sit in bin/ as dead weight.
# $(1) = SSH target, $(2) = install dir.
define deploy-remove-retired-binaries
	@ssh $(1) "rm -f $(2)/bin/helix-screen-egl $(2)/bin/helix-screen-fbdev"
endef

# Common deploy recipe (called with: $(call deploy-common,SSH_TARGET,DEPLOY_DIR,BIN_DIR))
# Usage: $(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
# Turn on the runtime switches that match what the binary being deployed
# actually contains.
#
# ENABLE_REMOTE_CONTROL is a BUILD flag: yes for every developer cross build,
# no only under HELIX_PACKAGING=1 (CROSS_REMOTE_CONTROL_DEFAULT above), so a
# packaged release install has no ctl server while a dev deploy has one.
# Having it compiled in and left off is not a
# state anyone wants: the server still only listens under --remote, and the
# SysV/systemd units exec helix-launcher.sh with no arguments, so without
# HELIX_REMOTE_CONTROL=1 in the device's helixscreen.env the flag silently buys
# nothing and diagnosing the printer means walking to it. `make deploy-*` is a
# separate make invocation with no PLATFORM_TARGET, so it cannot re-derive the
# build's choice — $(BIN_DIR)/.build-features, written by the link rule in
# mk/rules.mk, carries it across.
#
# Opt back out with HELIX_REMOTE_CONTROL=0 in the device env file; the key is
# only rewritten when the value differs, so that choice sticks across redeploys.
#
#   $(1) ssh target   $(2) device deploy dir   $(3) host bin dir just deployed
define sync-device-features
	@if [ "$$(sed -n 's/^remote_control=//p' $(3)/.build-features 2>/dev/null)" = "yes" ]; then \
		echo "$(DIM)Binary has the remote-control server — enabling it on the device...$(RESET)"; \
		scripts/device-env-set.sh "$(1)" "$(2)/config/helixscreen.env" HELIX_REMOTE_CONTROL 1; \
		echo "$(DIM)  drive it: ssh $(1) 'cd $(2) && ./bin/helix-screen ctl navigate settings'$(RESET)"; \
	fi
	@# Diagnostic uploads default OFF in every non-packaging binary (the
	@# compile-time gate is the only thing a fork build cannot tell from ours),
	@# so the dev deploy turns the runtime switch on here — this flow is how
	@# our test rigs are deployed, and helixscreen.env is excluded from every
	@# rsync/tar, so nothing else writes this key. Official packages never run
	@# this target: their binaries carry the marker at build time
	@# (HELIX_PACKAGING=1). Opt back out with HELIX_DIAGNOSTIC_UPLOADS=0 in the
	@# device env; note a later deploy restamps it to 1, same as remote
	@# control.
	@echo "$(DIM)Dev deploy — enabling diagnostic uploads on the device...$(RESET)"
	@scripts/device-env-set.sh "$(1)" "$(2)/config/helixscreen.env" HELIX_DIAGNOSTIC_UPLOADS 1
endef

# Deploy the platform hook file the init script sources at boot.
#
#   $(1) ssh target, $(2) deploy dir, $(3) hook key (assets/config/platform/hooks-<key>.sh)
#
# The init script sources $(2)/platform/hooks.sh. Without that file every hook
# stays the no-op stub it declares: platform_pre_start() never exports
# HELIX_CACHE_DIR, so the cache falls back down the cascade, and
# platform_stop_competing_uis() leaves the stock UI running. Both are silent,
# which is why this verifies the file landed instead of trusting the copy.
define deploy-platform-hooks
	@if [ -z "$(3)" ]; then \
		echo "$(DIM)No platform hook key - skipping hooks$(RESET)"; \
	elif [ ! -f assets/config/platform/hooks-$(3).sh ]; then \
		echo "$(RED)Platform hook not in tree: assets/config/platform/hooks-$(3).sh$(RESET)"; \
		exit 1; \
	else \
		echo "$(DIM)Deploying platform hooks ($(3))...$(RESET)"; \
		ssh $(1) "mkdir -p $(2)/platform"; \
		cat assets/config/platform/hooks-$(3).sh | ssh $(1) "cat > $(2)/platform/hooks.sh && chmod +x $(2)/platform/hooks.sh"; \
		ssh $(1) "test -s $(2)/platform/hooks.sh" || { \
			echo "$(RED)Platform hooks did not land on the device$(RESET)"; exit 1; }; \
	fi
endef

# Fail with a hint unless the binaries a deploy ships have been built.
# $(1) = host bin dir, $(2) = make target that builds it, $(3) = binary names
define require-built
	@for f in $(3); do test -f $(1)/$$f || { echo "$(RED)Error: $(1)/$$f not found. Run 'make $(2)' first.$(RESET)"; exit 1; }; done
endef

define deploy-common
	@echo "$(CYAN)Deploying HelixScreen to $(1):$(2)...$(RESET)"
	@# Generate pre-rendered splash images if missing (all small-display platforms use the same files)
	@if [ ! -f build/assets/images/prerendered/splash-logo-medium.bin ]; then \
		echo "$(DIM)Generating pre-rendered splash images...$(RESET)"; \
		$(MAKE) gen-images; \
	fi
	@if [ ! -d build/assets/images/printers/prerendered ] || [ -z "$$(ls -A build/assets/images/printers/prerendered/*.bin 2>/dev/null)" ]; then \
		echo "$(DIM)Generating pre-rendered printer images...$(RESET)"; \
		$(MAKE) gen-printer-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/benchy_thumbnail_white.bin ]; then \
		echo "$(DIM)Generating placeholder images...$(RESET)"; \
		$(MAKE) gen-placeholder-images; \
	fi
	@if [ ! -f build/assets/images/prerendered/splash-3d-dark-small.bin ]; then \
		echo "$(DIM)Generating 3D splash images...$(RESET)"; \
		$(MAKE) gen-splash-3d; \
	fi
	@# Stop every running instance before anything is synced. A systemd unit
	@# that cannot be stopped (non-root ssh user, sudo wants a password) ends
	@# the deploy here: killing a Restart=always unit's process only makes
	@# systemd respawn it from a half-synced bin/.
	@ssh $(1) 'sh -s' < scripts/deploy-stop-app-remote.sh; rc=$$?; \
	if [ $$rc -eq 3 ]; then \
		echo "$(RED)Refusing to deploy: the helixscreen systemd service on $(1) is running and could not be stopped.$(RESET)"; \
		echo "$(RED)Syncing now would let systemd restart it from a half-copied install.$(RESET)"; \
		echo "Fix one of:"; \
		echo "  - allow passwordless sudo for systemctl for the ssh user on the device, e.g."; \
		echo "      echo '<user> ALL=(root) NOPASSWD: /usr/bin/systemctl' | sudo tee /etc/sudoers.d/helixscreen-deploy"; \
		echo "  - or stop it with a TTY so sudo can prompt, then rerun the deploy:"; \
		echo "      ssh -t $(1) 'sudo systemctl stop helixscreen-update.path helixscreen && sudo systemctl stop helixscreen-update.path'"; \
		exit 1; \
	elif [ $$rc -ne 0 ]; then \
		echo "$(RED)Could not stop HelixScreen on $(1) (ssh exit $$rc) - not syncing$(RESET)"; \
		exit 1; \
	fi
	ssh $(1) "mkdir -p $(2)/bin"
	ssh $(1) "rm -f $(2)/*.xml 2>/dev/null || true"
	@# Sync binaries and launcher to bin/
	rsync -avzz --progress $(3)/helix-screen $(3)/helix-splash $(1):$(2)/bin/
	@if [ -f $(3)/helix-watchdog ]; then rsync -avzz $(3)/helix-watchdog $(1):$(2)/bin/; fi
	$(call deploy-remove-retired-binaries,$(1),$(2))
	@# Sync Bluetooth plugin if built (runtime-loaded via dlopen, same dir as binary)
	@if [ -f $(3)/libhelix-bluetooth.so ]; then \
		echo "$(DIM)Deploying Bluetooth plugin...$(RESET)"; \
		rsync -avzz $(3)/libhelix-bluetooth.so $(1):$(2)/bin/; \
	fi
	rsync -avzz scripts/helix-launcher.sh $(1):$(2)/bin/
	@# Sync installer script (needed for auto-updates)
	@# Sync assets (--delete removes stale files)
	rsync $(DEPLOY_RSYNC_FLAGS) $(DEPLOY_ASSET_EXCLUDES) $(DEPLOY_ASSET_DIRS) $(1):$(2)/
	@# Sync pre-rendered images
	@if [ -d build/assets/images/prerendered ]; then \
		rsync $(DEPLOY_RSYNC_FLAGS) build/assets/images/prerendered/ $(1):$(2)/assets/images/prerendered/; \
	fi
	@if [ -d build/assets/images/printers/prerendered ]; then \
		rsync $(DEPLOY_RSYNC_FLAGS) build/assets/images/printers/prerendered/ $(1):$(2)/assets/images/printers/prerendered/; \
	fi
	@# Fix ownership - rsync preserves macOS uid:gid which prevents writes on target.
	@# A plain chown is a no-op once every file already belongs to the connecting
	@# user - true for most of the fleet, where SSH lands as a non-root account
	@# that can only chown what it already owns - so escalation only fires when
	@# that fails: a root-connected target, or root-owned leftovers from an
	@# earlier install. -n (non-interactive) means a device with no passwordless
	@# sudo warns and lets the deploy continue instead of hanging here and
	@# never reaching the restart step below.
	@echo "$(DIM)Fixing file ownership...$(RESET)"
	@ssh $(1) "if [ \$$(id -u) -eq 0 ]; then chown -R \$$(id -u):\$$(id -g) $(2)/config 2>/dev/null; true; else chown -R \$$(id -u):\$$(id -g) $(2); fi" 2>/dev/null \
		|| ssh $(1) "sudo -n chown -R \$$(id -u):\$$(id -g) $(2)" 2>/dev/null \
		|| echo "$(YELLOW)⚠ Could not fix ownership of $(2) (sudo needs a password) - continuing$(RESET)"
	$(call sync-device-features,$(1),$(2),$(3))
	$(call deploy-platform-hooks,$(1),$(2),$(4))
endef

# =============================================================================
# Pi Deployment Configuration
# =============================================================================

# Pi deployment settings (can override via environment or command line)
# Example: make deploy-pi PI_HOST=192.168.1.50 PI_USER=pi
# PI_USER defaults to empty (uses SSH config or current user)
# PI_DEPLOY_DIR defaults to ~/helixscreen (full app directory)
PI_HOST ?= 192.168.1.113
PI_USER ?=
PI_DEPLOY_DIR ?= ~/helixscreen

# Build SSH target: user@host or just host if no user specified
ifdef PI_USER
    PI_SSH_TARGET := $(PI_USER)@$(PI_HOST)
else
    PI_SSH_TARGET := $(PI_HOST)
endif

# =============================================================================
# Pi Deployment Targets
# =============================================================================

# Restart helix-screen on the Pi (64- and 32-bit share the device) and re-arm
# the update watcher that deploy-common stopped.
define pi-restart
	@echo "$(CYAN)Restarting helix-screen on $(PI_HOST)...$(RESET)"
	@# Prefer systemd restart (picks up Environment= vars and keeps logs in journal).
	@# Use -n (non-interactive sudo) to fail fast rather than hang on a password prompt.
	@# Fallback uses ssh -f which forks into background AFTER authentication; without -f,
	@# ssh keeps the connection open as long as the remote child has stdout attached,
	@# even with </dev/null >/dev/null and a trailing &, and the make target hangs.
	@ssh $(PI_SSH_TARGET) "sudo -n systemctl start helixscreen" 2>/dev/null \
		|| ssh -f $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && setsid ./bin/helix-launcher.sh </dev/null >/dev/null 2>&1" \
		|| echo "$(YELLOW)⚠ Failed to restart helix-screen (set up passwordless sudo for systemctl or install the systemd unit)$(RESET)"
	@ssh $(PI_SSH_TARGET) "sudo -n systemctl start helixscreen-update.path" 2>/dev/null || true
	@echo "$(GREEN)✓ helix-screen restarted$(RESET)"
endef

.PHONY: deploy-pi deploy-pi-fg pi-ssh pi-test

# Deploy full application to Pi and restart in background
deploy-pi:
	$(call require-built,build/pi/bin,pi-docker,helix-screen helix-splash)
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
	@echo "$(GREEN)✓ Deployed to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	$(call pi-restart)
	@echo "$(DIM)Logs: ssh $(PI_SSH_TARGET) 'sudo journalctl -u helixscreen -f'$(RESET)"

# Deploy and run in foreground with debug logging (for interactive debugging)
deploy-pi-fg:
	$(call require-built,build/pi/bin,pi-docker,helix-screen helix-splash)
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi/bin)
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) (foreground, debug mode)...$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug --log-dest=console"

# Convenience: SSH into the Pi
pi-ssh:
	ssh $(PI_SSH_TARGET)

# Full cycle: build + deploy + run in foreground
pi-test: pi-docker deploy-pi-fg

# =============================================================================
# Pi AddressSanitizer Deployment Targets
# =============================================================================
# Deploys the SANITIZE=address build (build/pi-asan/bin/) and starts the
# binary with ASAN_OPTIONS configured for the wizard-transition investigation.
# Suppressions file is shipped to the device alongside the binary.

.PHONY: deploy-pi-asan deploy-pi-asan-fg pi-asan-test

# Pi-side ASAN runtime config. detect_leaks=0 because LSan trips on every
# normal LVGL image cache entry; abort_on_error=1 so the first UAF takes the
# process down with a coredump-style report; fast_unwind_on_malloc=0 makes
# stack traces reliable at the cost of speed (worth it for a stress run).
#
# asan.supp holds LeakSanitizer (`leak:`) rules, which belong in LSAN_OPTIONS,
# NOT ASAN_OPTIONS:suppressions= — feeding a leak-only file to ASAN's own
# suppression parser makes it abort at startup with "failed to parse
# suppressions" (hit while reproducing 783DVYKD, 2026-06-16). Keep them split.
# (LSAN_OPTIONS is only consulted when detect_leaks=1, but homing the file there
# keeps it correct if leak detection is re-enabled.)
PI_ASAN_OPTIONS ?= detect_leaks=0:abort_on_error=1:fast_unwind_on_malloc=0:print_stacktrace=1:halt_on_error=1:strip_path_prefix=/src/
PI_LSAN_OPTIONS ?= suppressions=$(PI_DEPLOY_DIR)/asan.supp

deploy-pi-asan:
	$(call require-built,build/pi-asan/bin,pi-asan-docker,helix-screen)
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi-asan/bin)
	@echo "$(CYAN)Shipping ASAN suppressions...$(RESET)"
	$(Q)rsync -avzz tests/asan.supp $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/
	@echo "$(GREEN)✓ Deployed ASAN build to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	@echo "$(YELLOW)Note: do NOT systemd-restart — ASAN binaries need ASAN_OPTIONS in env.$(RESET)"

# Run the ASAN binary in the foreground with ASAN_OPTIONS injected.
# Output stays attached to your terminal so you see the ASAN report immediately
# when (if) the wizard navigation provokes a heap error.
deploy-pi-asan-fg: deploy-pi-asan
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) under AddressSanitizer...$(RESET)"
	@echo "$(DIM)ASAN_OPTIONS=$(PI_ASAN_OPTIONS) LSAN_OPTIONS=$(PI_LSAN_OPTIONS)$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ASAN_OPTIONS='$(PI_ASAN_OPTIONS)' LSAN_OPTIONS='$(PI_LSAN_OPTIONS)' ./bin/helix-launcher.sh --debug --log-dest=console"

# Full cycle: ASAN build + deploy + run in foreground.
pi-asan-test: pi-asan-docker deploy-pi-asan-fg

# =============================================================================
# Pi 32-bit Deployment Targets
# =============================================================================
# Shares PI_HOST/PI_USER/PI_DEPLOY_DIR with 64-bit Pi (same device, different binary arch)

.PHONY: deploy-pi32 deploy-pi32-fg deploy-pi32-bin pi32-test

# Deploy full application to Pi (32-bit) and restart in background
deploy-pi32:
	$(call require-built,build/pi32/bin,pi32-docker,helix-screen helix-splash)
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32/bin)
	@echo "$(GREEN)✓ Deployed to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	$(call pi-restart)
	@echo "$(DIM)Logs: ssh $(PI_SSH_TARGET) 'sudo journalctl -u helixscreen -f'$(RESET)"

# Deploy and run in foreground with debug logging (for interactive debugging)
deploy-pi32-fg:
	$(call require-built,build/pi32/bin,pi32-docker,helix-screen helix-splash)
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32/bin)
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) (foreground, debug mode)...$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ./bin/helix-launcher.sh --debug --log-dest=console"

# Deploy binaries only (fast, for quick iteration)
deploy-pi32-bin:
	$(call require-built,build/pi32/bin,pi32-docker,helix-screen)
	@echo "$(CYAN)Deploying binaries only to $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin...$(RESET)"
	ssh $(PI_SSH_TARGET) "killall helix-watchdog helix-screen helix-splash 2>/dev/null; sleep 0.5; killall -9 helix-watchdog helix-screen helix-splash 2>/dev/null; while pidof helix-screen helix-splash helix-watchdog >/dev/null 2>&1; do sleep 0.2; done; true"
	ssh $(PI_SSH_TARGET) "mkdir -p $(PI_DEPLOY_DIR)/bin"
	rsync -avzz --progress build/pi32/bin/helix-screen build/pi32/bin/helix-splash $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin/
	@if [ -f build/pi32/bin/helix-watchdog ]; then rsync -avzz build/pi32/bin/helix-watchdog $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/bin/; fi
	@echo "$(GREEN)✓ Binaries deployed$(RESET)"
	$(call sync-device-features,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32/bin)
	$(call pi-restart)

# Full cycle: build + deploy + run in foreground
pi32-test: pi32-docker deploy-pi32-fg

.PHONY: deploy-pi32-asan deploy-pi32-asan-fg pi32-asan-test

# Deploy the pi32 ASAN binary. Shares PI_ASAN_OPTIONS with the 64-bit Pi target.
deploy-pi32-asan:
	$(call require-built,build/pi32-asan/bin,pi32-asan-docker,helix-screen)
	$(call deploy-common,$(PI_SSH_TARGET),$(PI_DEPLOY_DIR),build/pi32-asan/bin)
	@echo "$(CYAN)Shipping ASAN suppressions...$(RESET)"
	$(Q)rsync -avzz tests/asan.supp $(PI_SSH_TARGET):$(PI_DEPLOY_DIR)/
	@echo "$(GREEN)✓ Deployed ASAN build to $(PI_HOST):$(PI_DEPLOY_DIR)$(RESET)"
	@echo "$(YELLOW)Note: do NOT systemd-restart — ASAN binaries need ASAN_OPTIONS in env.$(RESET)"

# Run the pi32 ASAN binary in the foreground with ASAN_OPTIONS injected so the
# heap error report lands on your terminal as soon as the boot/print-history
# load provokes it.
deploy-pi32-asan-fg: deploy-pi32-asan
	@echo "$(CYAN)Starting helix-screen on $(PI_HOST) under AddressSanitizer...$(RESET)"
	@echo "$(DIM)ASAN_OPTIONS=$(PI_ASAN_OPTIONS) LSAN_OPTIONS=$(PI_LSAN_OPTIONS)$(RESET)"
	ssh -t $(PI_SSH_TARGET) "cd $(PI_DEPLOY_DIR) && ASAN_OPTIONS='$(PI_ASAN_OPTIONS)' LSAN_OPTIONS='$(PI_LSAN_OPTIONS)' ./bin/helix-launcher.sh --debug --log-dest=console"

# Full cycle: ASAN build + deploy + run in foreground.
pi32-asan-test: pi32-asan-docker deploy-pi32-asan-fg

# =============================================================================
# Release Packaging
# =============================================================================
# Creates distributable tar.gz archives for each platform
# Includes: binaries, ui_xml, config, assets (fonts/images only, no test files)

RELEASE_DIR := releases
VERSION := $(shell cat VERSION.txt 2>/dev/null || echo "dev")
# Release filenames include 'v' prefix to match git tag convention (v0.9.3)
RELEASE_VERSION := v$(VERSION)

# Top-level asset dirs to include in release tarballs, derived from the shared
# packaging manifest (scripts/gen-packaging-manifest.sh) minus test-data dirs
# that are too large to ship. This replaces a hand-maintained whitelist, which
# is prone to silently missing a directory — assets/config and assets/sounds
# (tracker music used by snake game and settings UI) both slipped through it.
#
# Adding a new assets/<foo>/ directory to the source tree will now ship it
# automatically. To explicitly exclude something, add it to RELEASE_ASSETS_EXCLUDE.
RELEASE_ASSETS_ALL := $(shell scripts/gen-packaging-manifest.sh 2>/dev/null | grep -E '^assets/[^/]+$$')
RELEASE_ASSETS_EXCLUDE := assets/test_gcodes assets/test_timelapse
RELEASE_ASSETS := $(filter-out $(RELEASE_ASSETS_EXCLUDE),$(RELEASE_ASSETS_ALL))

# Root-level asset FILES (directly under assets/, not in a subdirectory). The
# RELEASE_ASSETS dir loop only copies directories, so any loose file must be
# listed here explicitly. filaments.json is the unified filament catalog,
# loaded on demand by FilamentCatalog (src/printer/filament_catalog.cpp).
# (mdi-icon-metadata.json.gz is intentionally NOT shipped — dev-only
# icon-search tooling, see release-clean-assets below.)
RELEASE_ASSET_FILES := assets/filaments.json

# Clean up release assets: remove files that are compiled into the binary or dev-only
# .c font files are compiled into the binary at build time
# .ttf/.otf source fonts are only used by font regen scripts, not at runtime (~35 MB savings)
# .icns is a macOS icon format, not used on embedded targets
# mdi-icon-metadata is dev-only (icon search tooling)
# .clang-format is dev-only
define release-clean-assets
	@find $(1)/assets/fonts -name '*.c' -delete 2>/dev/null || true
	@find $(1)/assets/fonts -name '*.ttf' -delete 2>/dev/null || true
	@find $(1)/assets/fonts -name '*.otf' -delete 2>/dev/null || true
	@find $(1)/assets/fonts -name '.clang-format' -delete 2>/dev/null || true
	@find $(1)/assets -name '*.icns' -delete 2>/dev/null || true
	@find $(1)/assets -name 'mdi-icon-metadata.json.gz' -delete 2>/dev/null || true
	@# A test fixture with no reference anywhere in the tree, which therefore
	@# reaches no screen. tests/shell/test_platform_manifest_gate.bats fails if it
	@# gains a real consumer, so stripping it here cannot quietly break a caller.
	@rm -f "$(1)/assets/images/orcaslicer test cube.PNG" 2>/dev/null || true
	@# assets/sounds is ~600 KB of MOD tracker modules, playable only where
	@# the tracker player is compiled in. AD5M has sound but deliberately no
	@# tracker (its single core busy-waits and kills prints); CC1/K2 have
	@# neither; the unified MIPS build carries the tracker for the AD5X (the
	@# K1 never finds a backend, so it never plays there). Which platform that
	@# is gets asked of the manifest below,
	@# because this recipe runs on the HOST: PLATFORM_TARGET is unset here and
	@# defaults to native, so a TRACKER_CXXFLAGS test answers for the machine
	@# doing the packaging rather than the printer receiving it.
	@# Drop art the platform's panel can never ask for: splash classes other than
	@# the one its resolution selects, printer renders at the other size, and the
	@# source PNGs once every printer has a render at the size being kept.
	@# gen-splash-3d-<platform> already narrows what gets BUILT, but the copy above
	@# takes whatever build/ happens to hold, so this is the step that bounds the
	@# payload. A platform whose panel is unbounded keeps every size class and the
	@# source PNGs; one whose manifest names candidate boards ships their union.
	$(if $(2),@$(PLATFORM_MANIFEST) prune-assets $(2) $(1))
endef

# PII / runtime-state files that must NEVER ship in a release tarball.
# Personal config (settings.json, helixconfig*.json) is handled per-target
# because some targets ship a curated default. This list is for things that
# get auto-generated at runtime and have no business in a public release.
# tests/shell/test_runtime_file_lists.bats checks this against DEPLOY_RUNTIME_EXCLUDES
# and the other runtime-file lists.
define release-strip-pii
	@rm -f $(1)/config/telemetry_device.json \
	       $(1)/config/telemetry_queue.json \
	       $(1)/config/tool_spools.json \
	       $(1)/config/filament_slot_overrides.json \
	       $(1)/config/user_filaments.json \
	       $(1)/config/crash*.txt \
	       $(1)/config/.crash_restart_count \
	       $(1)/config/crash_history.json \
	       $(1)/config/.helix-screen.lock \
	       2>/dev/null || true
endef

# Developer showcase panels (see ENABLE_DEV_PANELS in Makefile). Their .cpp files
# are filtered out of APP_SRCS and their XML registrations compiled out for every
# cross target, so these layouts are unreachable in a release build — nothing
# registers or instantiates them. Ship the asset half gated the same way the code
# half is.
#
# Unconditional on purpose: release-* recipes run in the top-level (native) make,
# where ENABLE_DEV_PANELS defaults to yes, while the binaries they package come
# from separate cross sub-makes where it defaults to no. Keying the strip off the
# ambient $(ENABLE_DEV_PANELS) would therefore never fire.
DEV_PANEL_XML := glyphs_panel.xml

# Stage ui_xml/ + config/ + moonraker-plugin/ into a release tree, minus the
# dev-panel layouts and the plugin's dev-only files. moonraker-plugin must ride
# every release: HelixPluginInstaller (src/system/helix_plugin_installer.cpp)
# resolves <install>/moonraker-plugin/install.sh next to bin/helix-screen, and
# without it the in-UI plugin install/uninstall fails on every release install.
# Args: $(1) = staged release root (e.g. $(RELEASE_DIR)/helixscreen)
define release-copy-xml-config
	@cp -r ui_xml config moonraker-plugin $(1)/
	@rm -f $(addprefix $(1)/ui_xml/,$(DEV_PANEL_XML))
	@rm -rf $(1)/moonraker-plugin/tests $(1)/moonraker-plugin/TESTING_PLAN.md
	@find $(1)/moonraker-plugin -type d -name __pycache__ -prune -exec rm -rf {} +
	@# Minify the STAGED copy only -- never ui_xml/ in the source tree. The XML
	@# engine keeps a verbatim copy of every component's <view> source text alive
	@# for the whole session (lv_xml_component.c extract_view_content: it is
	@# re-parsed on each lv_xml_create, so it cannot be freed), which makes shipped
	@# comments and indentation permanently resident heap on every device. Fails
	@# loudly rather than shipping unminified: this is not an optional nicety on
	@# the 114 MB boards, and a silent skip is how the FONT_TIERS trim went
	@# unnoticed for months.
	@python3 scripts/minify_xml_tree.py $(1)/ui_xml
endef

# Release packages use the same verified packager as GitHub Actions.
.PHONY: installer release-pi release-pi32 release-x86 release-x86-desktop release-all release-clean
installer:
	@sh install.sh --help

define release-package
	@mkdir -p $(RELEASE_DIR)
	python3 scripts/vorondeck-package.py --binary build/$(1)/bin/helix-screen --version $(VERSION) --output $(RELEASE_DIR)/VoronDeck-$(VERSION)-$(1).tar.gz
endef

release-pi: | build/pi/bin/helix-screen
	$(call release-package,pi)
release-pi32: | build/pi32/bin/helix-screen
	$(call release-package,pi32)
release-x86: | build/x86/bin/helix-screen
	$(call release-package,x86)
release-x86-desktop: | build/x86-desktop/bin/helix-screen
	$(call release-package,x86-desktop)
release-all: release-pi release-pi32 release-x86 release-x86-desktop
release-clean:
	@rm -rf $(RELEASE_DIR)

# Aliases for package-* — trigger the full build + package workflow.
# The legacy scripts/package.sh wrapper was deleted; these targets are now
# the single entry point for building release artifacts.
.PHONY: package-pi package-pi32 package-x86 package-x86-desktop package-all

# THE marker that separates a production build from a developer one. Everything
# a developer builds gets the helixctl server (see the ENABLE_REMOTE_CONTROL
# block in Makefile); what ships does not. Pattern-specific variables are
# inherited by prerequisites, so setting it here reaches the *-docker target
# each package-* depends on, and DOCKER_REMOTE_CONTROL carries it into the
# container.
#
# Not a substitute for the check in release-*: CI builds through a bare
# `make PLATFORM_TARGET=...` inside its own container and never runs package-*,
# so the marker alone would not have caught a release assembled from a
# developer build. The stamped binary is the thing that gets verified.
package-%: HELIX_PACKAGING := 1
package-%: ENABLE_MOCKS := no
package-%: ENABLE_REMOTE_CONTROL := no
package-%: ENABLE_DIAGNOSTIC_UPLOADS := no
# Every package depends on gen-splash-3d-<its own platform>. That target reads
# the platform's panel geometry from assets/config/platforms.json and generates
# exactly the classes the panel can select, so this list carries no resolution
# knowledge of its own.
package-pi: pi-all-docker gen-images gen-splash-3d-pi gen-printer-images release-pi
package-pi32: pi32-all-docker gen-images gen-splash-3d-pi32 gen-printer-images release-pi32
# Board-spelled entries of the same unified package: identical build and
# release, narrowed only to that board's splash classes.
package-x86: x86-all-docker gen-images gen-splash-3d-x86 gen-printer-images release-x86
package-x86-desktop: x86-desktop-docker gen-images gen-splash-3d-x86-desktop gen-printer-images release-x86-desktop
package-all: package-pi package-pi32 package-x86 package-x86-desktop

# Convenience alias (verb-target → target-verb)
.PHONY:
