# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import io
import json

SPEC = importlib.util.spec_from_file_location(
    'updates_setup', Path(__file__).resolve().parents[1] / 'scripts/setup-vorondeck-updates.py')
updater_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(updater_module)


class Services:
    def __init__(self):
        self.calls = []

    def action(self, action, service):
        self.calls.append((action, service))


class UpdaterSetupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name).resolve()
        self.repo = self.home / 'VoronDeck-beta'
        (self.repo / 'scripts').mkdir(parents=True)
        for name in ('moonraker-vorondeck.py', 'moonraker-vorondeck-runtime.py'):
            (self.repo / 'scripts' / name).write_text('# fixture\n')
        self.components = self.home / 'moonraker/moonraker/components'
        self.components.mkdir(parents=True)
        self.module = self.components / 'vorondeck_update.py'
        self.base = self.home / '.local/share/vorondeck'
        self.config = self.home / 'printer_data/config/VoronDeck'
        self.config.mkdir(parents=True)
        self.main = self.config.parent / 'moonraker.conf'
        self.main.write_bytes(b'[server]\nhost: 0.0.0.0\n[authorization]\napi_key: keep-secret\n')
        self.dedicated = self.config.parent / 'VoronDeck-updates.conf'
        self.allowlist = self.home / 'printer_data/moonraker.asvc'
        self.allowlist.write_bytes(b'klipper\ncustom-service')
        self.printer = self.config.parent / 'printer.cfg'
        self.printer.write_bytes(b'untouched printer configuration')
        self.services = Services()
        self.idle_calls = 0

    def idle(self):
        self.idle_calls += 1

    def install(self, **kwargs):
        options = dict(home=self.home, probe=lambda port: True, sleep=lambda _: None, attempts=2)
        options.update(kwargs)
        return updater_module.setup(self.repo, self.base, self.config, self.services,
                                  options.pop('idle_check', self.idle), **options)

    def test_success_preserves_config_and_is_idempotent(self):
        original = self.main.read_bytes()
        backup = self.install()
        self.assertTrue((backup / 'manifest.json').is_file())
        self.assertEqual(self.module.resolve(), self.repo / 'scripts/moonraker-vorondeck.py')
        self.assertTrue(self.main.read_bytes().startswith(original))
        self.assertEqual(self.main.read_text().count('[include VoronDeck-updates.conf]'), 1)
        self.assertEqual(self.allowlist.read_bytes(), b'klipper\ncustom-service\nvorondeck\n')
        self.assertEqual(self.printer.read_bytes(), b'untouched printer configuration')
        paths = (self.main, self.dedicated, self.module, self.allowlist)
        before = [p.lstat().st_mtime_ns for p in paths]
        self.assertIsNone(self.install())
        self.assertEqual(before, [p.lstat().st_mtime_ns for p in paths])
        self.assertEqual(self.services.calls, [('restart', 'moonraker')])
        self.assertEqual(self.idle_calls, 2)

    def test_failed_probe_rolls_back_exact_contents_and_absence(self):
        original_main, original_allow = self.main.read_bytes(), self.allowlist.read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'Previous configuration restored'):
            self.install(probe=lambda _: False)
        self.assertEqual(self.main.read_bytes(), original_main)
        self.assertEqual(self.allowlist.read_bytes(), original_allow)
        self.assertFalse(self.module.is_symlink())
        self.assertFalse(self.dedicated.exists())
        self.assertEqual(self.services.calls, [('restart', 'moonraker')] * 2)

    def test_failed_channel_change_restores_owned_module_and_config(self):
        self.install()
        before = self.dedicated.read_bytes()
        target = self.module.readlink()
        with self.assertRaises(RuntimeError):
            self.install(channel='stable', probe=lambda _: False)
        self.assertEqual(self.dedicated.read_bytes(), before)
        self.assertEqual(self.module.readlink(), target)

    def test_idle_guard_blocks_before_any_write(self):
        def busy():
            raise ValueError('Printer is busy')
        with self.assertRaisesRegex(ValueError, 'busy'):
            self.install(idle_check=busy)
        self.assertFalse(self.base.exists())
        self.assertFalse(self.module.is_symlink())
        self.assertEqual(self.services.calls, [])

    def test_second_idle_guard_rolls_back_without_restart(self):
        def became_busy():
            self.idle_calls += 1
            if self.idle_calls == 2:
                raise ValueError('Printer became busy')
        before = self.main.read_bytes()
        with self.assertRaises(RuntimeError):
            self.install(idle_check=became_busy)
        self.assertEqual(self.main.read_bytes(), before)
        self.assertFalse(self.dedicated.exists())
        self.assertEqual(self.services.calls, [])

    def test_interruption_restores_configuration(self):
        before = self.main.read_bytes()
        with patch.object(self.services, 'action', side_effect=[KeyboardInterrupt(), None]):
            with self.assertRaisesRegex(RuntimeError, 'Previous configuration restored'):
                self.install()
        self.assertEqual(self.main.read_bytes(), before)
        self.assertFalse(self.module.exists())
        self.assertFalse(self.dedicated.exists())

    def test_foreign_module_and_dedicated_config_rejected(self):
        for path in (self.module, self.dedicated):
            with self.subTest(path=path):
                path.write_bytes(b'foreign')
                with self.assertRaisesRegex(ValueError, 'Refusing'):
                    self.install()
                self.assertEqual(path.read_bytes(), b'foreign')
                path.unlink()
        self.assertFalse(self.base.exists())

    def test_symlinked_config_is_rejected_without_target_change(self):
        outside = self.home / 'other.conf'
        outside.write_bytes(b'external')
        self.main.unlink()
        self.main.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'Linked'):
            self.install()
        self.assertEqual(outside.read_bytes(), b'external')

    def test_existing_glob_include_does_not_add_duplicate(self):
        self.main.write_bytes(b'[server]\n[include *.conf]\n')
        self.install()
        self.assertEqual(self.main.read_bytes(), b'[server]\n[include *.conf]\n')

    def test_conflicting_section_is_rejected(self):
        self.main.write_bytes(b'[update_manager vorondeck]\ntype: git_repo\n')
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            self.install()
        self.assertEqual(self.services.calls, [])

    def test_missing_source_does_not_modify_configuration(self):
        self.components.rmdir()
        with self.assertRaisesRegex(ValueError, 'Moonraker source not found'):
            self.install()
        self.assertFalse(self.dedicated.exists())

    def test_outside_base_and_invalid_channel_rejected(self):
        self.base = self.home.parent / 'not-our-base'
        with self.assertRaisesRegex(ValueError, 'inside'):
            self.install()
        with self.assertRaisesRegex(ValueError, 'Invalid'):
            self.install(channel='nightly')
        self.assertFalse(self.dedicated.exists())

    def test_probe_requires_both_loaded_component_and_updater(self):
        def responses(components, versions, failed=None):
            return [io.BytesIO(json.dumps({'result': payload}).encode()) for payload in (
                {'components': components, 'failed_components': failed or []},
                {'version_info': versions})]
        cases = [([], {'vorondeck': {}}, [], False),
                 (['vorondeck_update'], {}, [], False),
                 (['vorondeck_update'], {'vorondeck': {}}, ['vorondeck_update'], False),
                 (['vorondeck_update'], {'vorondeck': {}}, [], True)]
        for components, versions, failed, expected in cases:
            with patch.object(updater_module, 'urlopen', side_effect=responses(components, versions, failed)):
                self.assertEqual(updater_module.probe_loaded(7125), expected)


if __name__ == '__main__':
    unittest.main()
