# SPDX-License-Identifier: GPL-3.0-or-later
"""Capability probes using the standard Moonraker file manager API."""

from __future__ import annotations

import sys
from typing import Any, Callable, Iterable, Optional

ProbeAnswer = Optional[bool]
NativeFormatProbe = Callable[[Any], ProbeAnswer]


def _file_manager_module(file_manager: Any) -> Any:
    return sys.modules.get(type(file_manager).__module__)


# Moonraker's VALID_GCODE_EXTS where its module cannot be read.
DEFAULT_PRINT_EXTS = (".gcode", ".g", ".gco", ".ufp", ".nc")


def print_extensions(file_manager: Any) -> tuple:
    """The extensions Moonraker lists as G-code and starts on an upload's
    print=true: its file manager module's VALID_GCODE_EXTS, lower-cased."""
    exts = getattr(_file_manager_module(file_manager), "VALID_GCODE_EXTS", None)
    if not exts:
        return DEFAULT_PRINT_EXTS
    return tuple(str(e).lower() for e in exts)


def valid_gcode_exts_probe(file_manager: Any) -> ProbeAnswer:
    """Generic: the file manager module's VALID_GCODE_EXTS, which decides what
    Moonraker scans, lists as G-code and lets Klipper start."""
    exts = getattr(_file_manager_module(file_manager), "VALID_GCODE_EXTS", None)
    if exts is None:
        return None
    return ".3mf" in exts


NATIVE_FORMAT_PROBES = (valid_gcode_exts_probe,)


def prints_3mf_natively(
    file_manager: Any, probes: Iterable[NativeFormatProbe] = NATIVE_FORMAT_PROBES
) -> ProbeAnswer:
    """Whether this Moonraker prints .3mf itself: True if any probe says so,
    False if one says it does not and none says it does, None if none can
    tell."""
    answers = [probe(file_manager) for probe in probes]
    if any(a is True for a in answers):
        return True
    if any(a is False for a in answers):
        return False
    return None
