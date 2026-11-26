# SPDX-License-Identifier: GPL-3.0-or-later
"""
Tests for the plugin's capability probes (helix_capabilities.py): each probe
on its own, then the dispatch that combines them.

Run with: pytest tests/test_helix_capabilities.py -v
"""

import sys
import types
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

from helix_capabilities import (
    NATIVE_FORMAT_PROBES,
    prints_3mf_natively,
    valid_gcode_exts_probe,
)


def file_manager_in(name: str, source: str):
    """A FileManager instance whose class lives in a module built from
    @p source, the way Moonraker's own file_manager module holds both."""
    module = types.ModuleType(name)
    exec(compile(source, name, "exec"), module.__dict__)
    sys.modules[name] = module
    return module.FileManager()


UPSTREAM = """
VALID_GCODE_EXTS = ['.gcode', '.g', '.gco', '.ufp', '.nc']
class FileManager:
    pass
"""

NO_LIST = """
class FileManager:
    pass
"""

NATIVE_3MF = UPSTREAM.replace(".nc'", ".nc', '.3mf'")


class TestValidGcodeExtsProbe:
    def test_upstream_list_says_no(self):
        assert valid_gcode_exts_probe(file_manager_in("cap_upstream", UPSTREAM)) is False

    def test_a_list_with_3mf_says_yes(self):
        assert valid_gcode_exts_probe(file_manager_in("cap_native_3mf", NATIVE_3MF)) is True

    def test_no_list_cannot_tell(self):
        assert valid_gcode_exts_probe(file_manager_in("cap_nolist_a", NO_LIST)) is None


class TestDispatch:
    def test_every_probe_is_registered(self):
        assert set(NATIVE_FORMAT_PROBES) == {valid_gcode_exts_probe}

    def test_upstream_moonraker_does_not(self):
        assert prints_3mf_natively(file_manager_in("cap_upstream_c", UPSTREAM)) is False

    def test_private_upload_methods_do_not_override_extensions(self):
        fm = file_manager_in("cap_private_method", UPSTREAM)
        fm._finish_3mf_upload = lambda: None
        assert prints_3mf_natively(fm) is False

    def test_nobody_can_tell(self):
        assert prints_3mf_natively(file_manager_in("cap_nolist_c", NO_LIST)) is None

    def test_combines_any_probes_it_is_given(self):
        fm = object()
        assert prints_3mf_natively(fm, (lambda f: None, lambda f: False)) is False
        assert prints_3mf_natively(fm, (lambda f: False, lambda f: True)) is True
        assert prints_3mf_natively(fm, ()) is None
