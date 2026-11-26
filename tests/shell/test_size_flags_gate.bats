#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify RTTI and mock-code flags on supported Linux build targets.

load helpers

# Supported open Linux targets.
DEVICE_TARGETS="pi pi32 x86 x86-desktop"

# Development builds retain mocks and debug assertions; packaging removes mocks.
# The control below proves these assertions
# discriminate rather than pass on everything.
CONTROL_TARGET="pi"

mkvar() {
    local target="$1" var="$2"
    shift 2
    scratch_make -n PLATFORM_TARGET="$target" CROSS_COMPILE= CC=gcc CXX=g++ "$@" "print-var-$var" 2>/dev/null
}



@test "size flags: -fno-rtti reaches our C++ but never the submodules" {
    for t in $DEVICE_TARGETS; do
        mkvar "$t" CXXFLAGS | grep -q 'fno-rtti' || {
            echo "$t is missing -fno-rtti in CXXFLAGS:"
            mkvar "$t" CXXFLAGS
            return 1
        }
        # libhv throws, and its catch clauses want typeinfo for the thrown
        # types. -fno-rtti there is a link failure deep in a cross build.
        mkvar "$t" SUBMODULE_CXXFLAGS | grep -q 'fno-rtti' && {
            echo "$t applies -fno-rtti to SUBMODULE_CXXFLAGS, which breaks libhv's catch clauses:"
            mkvar "$t" SUBMODULE_CXXFLAGS
            return 1
        }
    done
    return 0
}

@test "size flags: a roomy target gets none of them" {
    mkvar "$CONTROL_TARGET" CXXFLAGS | grep -q 'DHELIX_ENABLE_MOCKS' || {
        echo "$CONTROL_TARGET should keep mocks; the assertions above are not discriminating:"
        mkvar "$CONTROL_TARGET" CXXFLAGS
        return 1
    }
    mkvar "$CONTROL_TARGET" CXXFLAGS | grep -q 'DNDEBUG' && {
        echo "$CONTROL_TARGET unexpectedly carries size flags:"
        mkvar "$CONTROL_TARGET" CXXFLAGS
        return 1
    }
    mkvar native CXXFLAGS | grep -q 'fno-rtti' && {
        echo "the native build must keep RTTI:"
        mkvar native CXXFLAGS
        return 1
    }
    return 0
}

@test "size flags: a packaged build leaves the mocks out on a roomy target" {
    mkvar "$CONTROL_TARGET" CXXFLAGS HELIX_PACKAGING=1 | grep -q 'DHELIX_ENABLE_MOCKS' && {
        echo "$CONTROL_TARGET still compiles the mocks under HELIX_PACKAGING=1:"
        mkvar "$CONTROL_TARGET" CXXFLAGS HELIX_PACKAGING=1
        return 1
    }
    return 0
}
