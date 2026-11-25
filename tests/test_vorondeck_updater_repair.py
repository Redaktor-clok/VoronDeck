# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('repair', ROOT / 'assets/updates/repair-vorondeck-updater.py')
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)


class UpdaterRepair(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name).resolve()
        self.base = self.home / '.local/share/vorondeck'
        self.root = self.base / 'releases/1.0.2-beta.2-fixture'
        self.bundle = self.root / 'assets/updates/manager'
        files = ['scripts/' + name for name in (
            'moonraker-vorondeck.py', 'moonraker-vorondeck-runtime.py', 'install-vorondeck.py',
            'vorondeck-bootstrap.py', 'vorondeck_distribution.py', 'vorondeck_installer_i18n.py',
            'setup-vorondeck-updates.py')]
        files += ['assets/updates/check-vorondeck-update.py', 'assets/updates/vorondeck_public_releases.py']
        hashes = {}
        for name in files:
            path = self.bundle / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, path)
            hashes[path.relative_to(self.root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
        (self.root / 'vorondeck-release.json').write_text(json.dumps({
            'schema': 1, 'project': 'VoronDeck', 'files': hashes}))
        (self.base / 'current').symlink_to(self.root, target_is_directory=True)
        self.module = self.home / 'moonraker/moonraker/components/vorondeck_update.py'
        self.module.parent.mkdir(parents=True)
        self.old = self.home / 'VoronDeck-beta/scripts/moonraker-vorondeck.py'
        self.old.parent.mkdir(parents=True)
        self.old.write_text('# old installed updater')
        self.module.symlink_to(self.old)
        self.restarts = 0
        self.healthy = True
        self.busy = False
        self.printing = False
        self.heater = 0
        self.snapshots = []

    def request(self, path, data=None):
        if path == 'machine/update/status':
            return {'busy': self.busy, 'version_info': {'vorondeck': {
                'vorondeck_release_selection': bool(self.restarts and self.healthy),
                'vorondeck_update_progress': int(bool(self.restarts and self.healthy))}}}
        if path == 'server/info':
            return {'failed_components': []}
        if path == 'machine/services/restart':
            self.assertEqual(data, {'service': 'moonraker'})
            self.restarts += 1
            self.snapshots.append(self.module.resolve())
            return 'ok'
        if path == 'printer/objects/query?print_stats&idle_timeout&heaters':
            return {'status': {'print_stats': {'state': 'printing' if self.printing else 'standby'},
                               'idle_timeout': {'state': 'Idle'},
                               'heaters': {'available_heaters': ['extruder']}}}
        if path == 'printer/objects/query?extruder':
            return {'status': {'extruder': {'target': self.heater}}}
        raise AssertionError(path)

    def run_repair(self):
        repair.repair(self.root, self.home, 7125, request=self.request, sleep=lambda _: None, attempts=2)

    def test_legacy_updater_uses_verified_bundle_and_repair_is_idempotent(self):
        self.run_repair()
        expected = self.bundle / 'scripts/moonraker-vorondeck.py'
        self.assertEqual(self.module.resolve(), expected)
        self.assertEqual(self.snapshots, [expected])
        self.assertEqual(self.old.read_text(), '# old installed updater')
        self.assertEqual(len(list((self.base / 'backups').glob('updater-*/module.json'))), 1)
        self.run_repair()
        self.assertEqual(self.restarts, 1)

    def test_failed_load_restores_previous_module(self):
        self.healthy = False
        with self.assertRaises(RuntimeError):
            self.run_repair()
        self.assertEqual(self.module.resolve(), self.old)
        self.assertEqual(self.restarts, 2)
        self.assertEqual(self.snapshots[-1], self.old)

    def test_current_link_with_old_loaded_module_still_restarts(self):
        self.run_repair()
        request = self.request
        def old_loaded_module(path, data=None):
            result = request(path, data)
            if path == 'machine/update/status' and self.restarts == 1:
                result['version_info']['vorondeck']['vorondeck_update_progress'] = 0
            return result
        repair.repair(self.root, self.home, 7125, request=old_loaded_module,
                      sleep=lambda _: None, attempts=2)
        self.assertEqual(self.restarts, 2)

    def test_busy_printing_and_heat_refuse_before_mutation(self):
        for attr, value in [('busy', True), ('printing', True), ('heater', 60)]:
            with self.subTest(attr=attr):
                setattr(self, attr, value)
                with self.assertRaises(ValueError):
                    self.run_repair()
                setattr(self, attr, False if attr != 'heater' else 0)
                self.assertEqual(self.module.resolve(), self.old)
                self.assertEqual(self.restarts, 0)

    def test_tampered_bundle_refused_before_loading(self):
        (self.bundle / 'scripts/moonraker-vorondeck.py').write_text('raise RuntimeError("tampered")')
        with self.assertRaises(ValueError):
            self.run_repair()
        self.assertEqual(self.module.resolve(), self.old)
        self.assertEqual(self.restarts, 0)

    def test_unowned_module_refused(self):
        other = self.home / 'custom.py'
        other.write_text('# another updater')
        self.module.unlink()
        self.module.symlink_to(other)
        with self.assertRaises(ValueError):
            self.run_repair()
        self.assertEqual(self.module.resolve(), other)
        self.assertEqual(self.restarts, 0)

    def test_stale_release_cannot_replace_active_manager(self):
        (self.base / 'current').unlink()
        (self.base / 'current').symlink_to(self.old.parent, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.run_repair()
        self.assertEqual(self.restarts, 0)


if __name__ == '__main__':
    unittest.main()
