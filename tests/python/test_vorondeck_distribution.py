"""Trust-boundary checks for the beta package and installer (no hardware commands)."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import struct
import subprocess
import tarfile
import tempfile
import unittest
import sys
import os
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('installer', ROOT / 'scripts/install-vorondeck.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)
spec_boot = importlib.util.spec_from_file_location('bootstrap', ROOT / 'scripts/vorondeck-bootstrap.py')
bootstrap = importlib.util.module_from_spec(spec_boot)
spec_boot.loader.exec_module(bootstrap)


class DistributionTests(unittest.TestCase):
    def bundle(self, folder, *, version='0.1.0-beta.1', arch='x86_64', member=None, symlink=False, duplicate=False, bad_hash=False, plugin_machine=None):
        elf = bytearray(64)
        elf[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', elf, 18, 62)
        payload = {'bin/helix-screen': bytes(elf), 'config/settings.json.template':
                   b'{"config_version":32,"wizard_completed":false}'}
        if plugin_machine is not None:
            struct.pack_into('<H', elf, 18, plugin_machine)
            payload['bin/libhelix-bluetooth.so'] = bytes(elf)
        manifest = {'schema': 1, 'project': 'VoronDeck', 'version': version,
                    'architecture': arch, 'files': {p: hashlib.sha256(d).hexdigest() for p, d in payload.items()}}
        if bad_hash:
            manifest['files']['bin/helix-screen'] = '0' * 64
        payload['vorondeck-release.json'] = json.dumps(manifest).encode()
        path = folder / 'package.tar.gz'
        with tarfile.open(path, 'w:gz') as archive:
            for name, data in payload.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
            if member:
                info = tarfile.TarInfo(member)
                if symlink:
                    info.type, info.linkname = tarfile.SYMTYPE, '/etc/passwd'
                archive.addfile(info)
            if duplicate:
                archive.addfile(tarfile.TarInfo('bin/helix-screen'))
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def test_clean_package_keeps_first_run(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.bundle(Path(directory))
            manifest, payload = installer.verify_package(*args, 'x86_64')
            self.assertEqual(manifest['version'], '0.1.0-beta.1')
            self.assertFalse(json.loads(payload['config/settings.json.template'])['wizard_completed'])
            self.assertNotIn('config/settings.json', payload)

    def test_stable_and_beta_versions_are_distinct(self):
        for version in ('1.0.0', '1.0.0-beta.1', '1.0.2-T.1', '1.0.0-preview', '../1.0.0'):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                args = self.bundle(Path(directory), version=version)
                if version in ('1.0.0', '1.0.0-beta.1', '1.0.2-T.1'):
                    manifest, _ = installer.verify_package(*args, 'x86_64')
                    self.assertEqual(manifest['version'], version)
                else:
                    with self.assertRaises(ValueError):
                        installer.verify_package(*args, 'x86_64')

    def test_archive_attacks_are_refused(self):
        for name, symlink in [('../outside', False), ('/tmp/outside', False),
                              ('assets/../../outside', False), ('assets/link', True),
                              ('config/settings.json', False), ('assets\\escape', False)]:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                args = self.bundle(Path(directory), member=name, symlink=symlink)
                with self.assertRaises(ValueError):
                    installer.verify_package(*args, 'x86_64')

    def test_optional_bluetooth_plugin_matches_target_abi(self):
        for machine in (62, 183):
            with self.subTest(machine=machine), tempfile.TemporaryDirectory() as directory:
                args = self.bundle(Path(directory), plugin_machine=machine)
                if machine == 62:
                    _, payload = installer.verify_package(*args, 'x86_64')
                    self.assertIn('bin/libhelix-bluetooth.so', payload)
                else:
                    with self.assertRaisesRegex(ValueError, 'Bluetooth plugin architecture'):
                        installer.verify_package(*args, 'x86_64')

    def test_duplicate_and_bad_manifest_hash_refused(self):
        for flags in ({'duplicate': True}, {'bad_hash': True}, {'arch': 'aarch64'}):
            with self.subTest(flags=flags), tempfile.TemporaryDirectory() as directory:
                args = self.bundle(Path(directory), **flags)
                with self.assertRaises(ValueError):
                    installer.verify_package(*args, 'x86_64')

    def test_wrong_archive_digest_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path, _ = self.bundle(Path(directory))
            with self.assertRaises(ValueError):
                installer.verify_package(path, '0' * 64, 'x86_64')

    def test_hardware_state_is_fail_closed(self):
        status = {'print_stats': {'state': 'standby'}, 'idle_timeout': {'state': 'Idle'},
                  'heaters': {'available_heaters': ['extruder']}, 'extruder': {'target': 0}}
        installer.assert_idle(status)
        for state in ['printing', 'paused', 'error', None]:
            with self.subTest(state=state), self.assertRaises(ValueError):
                installer.assert_idle({**status, 'print_stats': {'state': state}})
        for target in [200, None, True, float('nan')]:
            with self.subTest(target=target), self.assertRaises(ValueError):
                installer.assert_idle({**status, 'extruder': {'target': target}})
        with self.assertRaises(ValueError):
            installer.assert_idle({})

    def test_service_does_not_launch_old_installer_or_mock(self):
        text = installer.unit_text('orangepi', PurePosixPath('/home/orangepi/.local/share/vorondeck/current'),
                                   PurePosixPath('/home/orangepi/printer_data/config/VoronDeck'), ['video', 'input'])
        self.assertIn('HELIX_CONFIG_DIR=/home/orangepi/printer_data/config/VoronDeck', text)
        self.assertNotIn('--test', text)
        self.assertNotIn('ExecStartPre=+', text)
        self.assertNotIn('CAP_SYS_BOOT', text)
        self.assertIn('HELIX_DISABLE_AUTO_UPDATES=1', text)
        # The native settings restart exits cleanly under systemd.
        self.assertIn('Restart=always', text)
        with self.assertRaises(ValueError):
            installer.unit_text('user\nExecStart=/bad', Path('/safe'), Path('/safe'), [])

    def test_supported_elf_abis(self):
        for bits, machine, flags, expected in [(2, 183, 0, 'aarch64'), (2, 62, 0, 'x86_64'),
                                                (1, 40, 0x400, 'armhf'), (1, 40, 0, None),
                                                (2, 40, 0, None), (1, 3, 0, None)]:
            with self.subTest(expected=expected, machine=machine):
                data = bytearray(64)
                data[:6] = b'\x7fELF' + bytes([bits, 1])
                struct.pack_into('<H', data, 18, machine)
                struct.pack_into('<I', data, 36, flags)
                if expected:
                    self.assertEqual(installer.elf_arch(data), expected)
                else:
                    with self.assertRaises(ValueError):
                        installer.elf_arch(data)

    def test_openrc_uses_user_and_root_owned_pid(self):
        text = installer.Services('openrc').text('printer', PurePosixPath('/home/printer/current'),
                                                PurePosixPath('/home/printer/config'), [], 7126)
        self.assertTrue(text.startswith('#!/sbin/openrc-run\n'))
        self.assertIn('command_user="printer"', text)
        self.assertIn('pidfile="/run/vorondeck.pid"', text)
        self.assertIn('127.0.0.1:7126', text)
        self.assertIn('HELIX_DISABLE_AUTO_UPDATES=1', text)
        with self.assertRaises(ValueError):
            installer.Services('sysv')

    def test_console_claim_only_on_systemd_with_local_virtual_terminals(self):
        for kind in ('systemd', 'openrc'):
            for available in (False, True):
                with self.subTest(kind=kind, available=available), patch.object(
                        installer, 'local_vt_available', return_value=available):
                    text = installer.Services(kind).text('printer', PurePosixPath('/home/printer/current'),
                                                          PurePosixPath('/home/printer/config'), [], 7125)
                if kind == 'systemd' and available:
                    for line in ('Conflicts=getty@tty1.service', 'After=getty@tty1.service',
                                 'StandardInput=tty', 'TTYPath=/dev/tty1', 'TTYReset=yes',
                                 'TTYVTDisallocate=yes', 'StandardOutput=journal',
                                 'StandardError=journal', 'After=network.target plymouth-quit-wait.service'):
                        self.assertIn(line, text.splitlines())
                else:
                    self.assertNotIn('getty@tty1', text)
                    self.assertNotIn('StandardInput=tty', text)
                self.assertNotIn('tty-force', text)
                self.assertNotIn('CAP_DAC_OVERRIDE', text)
                self.assertNotIn('TTYVHangup', text)

    def test_console_detection_requires_both_character_devices(self):
        for values, expected in (([True, True], True), ([False], False), ([True, False], False)):
            with patch.object(installer.Path, 'is_char_device', side_effect=values) as device:
                self.assertEqual(installer.local_vt_available(), expected)
                self.assertEqual(device.call_count, len(values))

    def test_console_login_guard_accepts_only_idle_getty_and_managed_screen_pid(self):
        for output, allowed in (('1230 0 agetty\n', True), ('44 1000 helix-screen\n', True),
                                ('1230 1000 agetty\n', False), ('55 0 login\n', False),
                                ('56 1000 bash\n', False), ('99 1000 helix-screen\n', False),
                                ('malformed output\n', False)):
            replies = [subprocess.CompletedProcess([], 0, '44\n', '')] + [
                subprocess.CompletedProcess([], 0, '0\n', '') for _ in range(2)] + [
                subprocess.CompletedProcess([], 0, output, '')]
            with self.subTest(output=output), patch.object(installer.subprocess, 'run', side_effect=replies):
                if allowed:
                    installer.check_console_session()
                else:
                    with self.assertRaises(ValueError):
                        installer.check_console_session()

    def test_console_login_guard_refuses_process_query_errors(self):
        replies = [subprocess.CompletedProcess([], 0, '0\n', '') for _ in range(3)] + [
            subprocess.CompletedProcess([], 1, '', 'ps failed')]
        with patch.object(installer.subprocess, 'run', side_effect=replies):
            with self.assertRaisesRegex(ValueError, 'Cannot verify'):
                installer.check_console_session()

    def test_console_login_guard_accepts_empty_terminal(self):
        replies = [subprocess.CompletedProcess([], 0, '0\n', '') for _ in range(3)] + [
            subprocess.CompletedProcess([], 1, '', '')]
        with patch.object(installer.subprocess, 'run', side_effect=replies):
            installer.check_console_session()

    def test_release_selection_never_substitutes_abi(self):
        entry = {'architecture': 'armhf', 'libc': 'glibc', 'path': 'packages/armhf.tar.gz',
                 'sha256': 'a' * 64, 'size': 100}
        index = {'schema': 1, 'project': 'VoronDeck', 'packages': [entry]}
        self.assertEqual(bootstrap.select_package(index, 'armhf', 'glibc'), entry)
        for arch, libc in [('aarch64', 'glibc'), ('armhf', 'musl')]:
            with self.assertRaises(ValueError):
                bootstrap.select_package(index, arch, libc)
        for change in [{'path': '../escape.tar.gz'}, {'size': 100 * 1024**2}, {'sha256': 'invalid'}]:
            with self.subTest(change=change), self.assertRaises(ValueError):
                bootstrap.select_package({**index, 'packages': [{**entry, **change}]}, 'armhf', 'glibc')
        with self.assertRaises(ValueError):
            bootstrap.select_package({**index, 'packages': [entry, entry]}, 'armhf', 'glibc')

    def test_library_resolution_refuses_unknown_names(self):
        for manager in ('apt-get', 'dnf', 'pacman', 'apk', 'zypper'):
            with self.subTest(manager=manager):
                self.assertTrue(installer.dependency_packages(['libstdc++.so.6', 'libdrm.so.2'], manager))
                with self.assertRaises(ValueError):
                    installer.dependency_packages(['lib-not-a-package; command'], manager)

    def test_missing_libraries_require_confirmation_and_recheck(self):
        from subprocess import CompletedProcess
        missing = CompletedProcess([], 0, ' libstdc++.so.6 => not found\n', '')
        success = CompletedProcess([], 0, ' libstdc++.so.6 => /lib/libstdc++.so.6\n', '')
        with patch.object(installer.shutil, 'which', side_effect=lambda n: n if n in ('ldd', 'apt-get') else None), \
                patch.object(installer.subprocess, 'run', side_effect=[missing, success, success, success]) as run, \
                patch('builtins.input', return_value='y'):
            installer.check_dependencies(Path('/fixture'), allow_install=True)
            commands = [c.args[0] for c in run.call_args_list]
            self.assertEqual(commands[1], ['sudo', 'apt-get', 'update'])
            self.assertEqual(commands[2], ['sudo', 'apt-get', 'install', '-y', '--no-install-recommends', 'libstdc++6'])
            self.assertEqual(commands[3][0], 'ldd')
        with patch.object(installer.shutil, 'which', return_value='/usr/bin/ldd'), \
                patch.object(installer.subprocess, 'run', return_value=missing) as run, \
                patch('builtins.input', return_value='n'):
            with self.assertRaises(ValueError):
                installer.check_dependencies(Path('/fixture'), allow_install=True)
            self.assertEqual(run.call_count, 1)

    def test_stage_does_not_publish_partial_release(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, payload = installer.verify_package(*self.bundle(root), 'x86_64')
            destination = root / 'releases/v1'
            with patch.object(installer, 'check_dependencies', side_effect=ValueError('missing libc')):
                with self.assertRaises(ValueError):
                    installer.stage_release(destination, manifest, payload)
                self.assertFalse(destination.exists())
                self.assertEqual(list(destination.parent.iterdir()), [])
            with patch.object(installer, 'check_dependencies'):
                installer.stage_release(destination, manifest, payload)
                installer.stage_release(destination, manifest, payload)
                (destination / 'bin/helix-screen').write_bytes(b'corrupted')
                with self.assertRaises(ValueError):
                    installer.stage_release(destination, manifest, payload)


class FakeServices:
    def __init__(self, folder, kind='systemd', fail=None):
        self.kind, self.unit, self.fail = kind, folder / 'vorondeck.service', fail
        self.running = {'vorondeck': False, 'KlipperScreen': True, 'helixscreen': True}
        self.boot = dict.fromkeys(self.running, 'enabled' if kind == 'systemd' else ['default'])
        self.boot['vorondeck'] = 'not-found' if kind == 'systemd' else []
        self.calls = []

    def fault(self, operation):
        self.calls.append(operation)
        if self.fail == operation:
            self.fail = None
            raise RuntimeError('Injected failure: ' + operation)

    def active(self, name):
        return self.running[name]

    def boot_state(self, name):
        return self.boot[name]

    def action(self, action, name):
        self.fault(action + ':' + name)
        self.running[name] = action == 'start'
        if (action == 'start' and name == 'vorondeck' and self.unit.exists()
                and 'Conflicts=getty@tty1.service' in self.unit.read_text()):
            self.running['getty@tty1'] = False

    def install(self, source):
        self.fault('install')
        self.unit.write_bytes(source.read_bytes())

    def remove(self):
        self.fault('remove')
        self.unit.unlink(missing_ok=True)

    def set_boot(self, name, state):
        self.fault('boot:' + name)
        self.boot[name] = state


@unittest.skipUnless(os.name == 'posix', 'Real Linux filesystem transaction checks run in the VM')
class ActivationTests(unittest.TestCase):
    def execute(self, *, kind='systemd', failure=None, previous=False, restore=False,
                console=False, getty_active=True):
        with tempfile.TemporaryDirectory() as temporary, patch.object(installer.time, 'sleep'), \
                patch.object(installer, 'local_vt_available', return_value=True), \
                patch.object(installer, 'check_console_session'):
            home = Path(temporary)
            base, config, destination = home / 'app', home / 'config', home / 'app/releases/new'
            destination.mkdir(parents=True)
            config.mkdir()
            services = FakeServices(home, kind, failure)
            if console:
                services.running['getty@tty1'] = getty_active
                services.boot['getty@tty1'] = 'enabled-runtime'
            previous_settings = b'{"wizard_completed":true,"custom_layout":"keep"}'
            if previous:
                (base / 'current').symlink_to(base / 'releases/previous')
                (config / 'settings.json').write_bytes(previous_settings)
                services.unit.write_text(installer.UNIT_MARKER + '\nold service')
                services.running['vorondeck'] = True
                services.boot['vorondeck'] = 'enabled' if kind == 'systemd' else ['default', 'custom']
            original_running, original_boot = services.running.copy(), services.boot.copy()
            payload = {'config/settings.json.template': b'{"config_version":32}'}
            kwargs = dict(fresh=True, autostart=True, idle_check=lambda: None)
            service = installer.UNIT_MARKER + '\nnew service'
            if console:
                service += '\nConflicts=getty@tty1.service\nStandardInput=tty\nTTYPath=/dev/tty1\n'
            if failure:
                with self.assertRaisesRegex(RuntimeError, 'Injected failure'):
                    installer.activate_release(services, base, config, destination, payload,
                                               service, **kwargs)
            else:
                backup = installer.activate_release(services, base, config, destination, payload,
                                                    service, **kwargs)
                expected = {'vorondeck': True, 'KlipperScreen': False, 'helixscreen': False}
                if console:
                    expected['getty@tty1'] = False
                    self.assertEqual(json.loads((backup / 'switch.json').read_text())['getty_active'], getty_active)
                    self.assertEqual(services.boot['getty@tty1'], 'enabled-runtime')
                self.assertEqual(services.running, expected)
                self.assertFalse(json.loads((config / 'settings.json').read_text())['wizard_completed'])
                self.assertEqual((base / 'current').resolve(), destination)
                if not restore:
                    self.assertNotIn('start:getty@tty1', services.calls)
                    return
                installer.restore_switch(backup, services, base, config)
            self.assertEqual(services.running, original_running)
            self.assertEqual(services.boot, original_boot)
            if console:
                self.assertNotIn('boot:getty@tty1', services.calls)
                if 'start:getty@tty1' in services.calls:
                    restore_unit = max(i for i, call in enumerate(services.calls) if call in ('install', 'remove'))
                    self.assertGreater(services.calls.index('start:getty@tty1'), restore_unit)
            if previous:
                self.assertEqual((config / 'settings.json').read_bytes(), previous_settings)
                self.assertEqual(os.readlink(base / 'current'), str(base / 'releases/previous'))
                self.assertEqual(services.unit.read_text(), installer.UNIT_MARKER + '\nold service')
            else:
                self.assertFalse((base / 'current').is_symlink())
                self.assertFalse((config / 'settings.json').exists())
                self.assertFalse(services.unit.exists())

    def test_partial_failures_restore_both_init_systems(self):
        for kind in ('systemd', 'openrc'):
            for previous in (False, True):
                for failure in ('stop:helixscreen', 'install', 'start:vorondeck',
                                'boot:helixscreen', 'boot:vorondeck'):
                    with self.subTest(kind=kind, previous=previous, failure=failure):
                        self.execute(kind=kind, previous=previous, failure=failure)

    def test_success_and_explicit_rollback(self):
        for kind in ('systemd', 'openrc'):
            for previous in (False, True):
                for restore in (False, True):
                    with self.subTest(kind=kind, previous=previous, restore=restore):
                        self.execute(kind=kind, previous=previous, restore=restore)

    def test_console_takeover_restores_getty_active_state_only_on_rollback(self):
        self.execute(console=True, restore=False)
        for previous in (False, True):
            for getty_active in (False, True):
                for failure in (None, 'start:vorondeck', 'boot:helixscreen'):
                    with self.subTest(previous=previous, getty_active=getty_active, failure=failure):
                        self.execute(previous=previous, console=True, getty_active=getty_active,
                                     failure=failure, restore=True)

    def test_active_local_login_refuses_before_mutations(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(installer, 'local_vt_available', return_value=True), \
                patch.object(installer, 'check_console_session', side_effect=ValueError('local session')):
            home = Path(temporary)
            services = FakeServices(home)
            with self.assertRaisesRegex(ValueError, 'local session'):
                installer.activate_release(services, home, home, home, {},
                    'Conflicts=getty@tty1.service', idle_check=lambda: None)
            self.assertEqual(services.calls, [])
            self.assertFalse((home / 'backups').exists())

    def test_symlinked_settings_and_parent_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            (home / 'link').symlink_to('/tmp')
            with self.assertRaises(ValueError):
                installer.safe_child(home / 'link/file', home)

    def test_no_services_touched_when_printer_busy(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            services = FakeServices(home)
            def busy():
                raise ValueError('printing')
            with self.assertRaisesRegex(ValueError, 'printing'):
                installer.activate_release(services, home, home, home, {}, '', idle_check=busy)
            self.assertEqual(services.calls, [])


if __name__ == '__main__':
    unittest.main()
