# SPDX-License-Identifier: GPL-3.0-or-later
"""Release checks must distinguish rebuilt packages with the same version."""
import copy
import importlib.util
from pathlib import Path
import shlex
import tempfile
import unittest

path = Path(__file__).resolve().parents[1] / 'assets/updates/check-vorondeck-update.py'
spec = importlib.util.spec_from_file_location('vorondeck_update_check', path)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class ReleaseCheck(unittest.TestCase):
    def setUp(self):
        self.installed = {'schema': 1, 'project': 'VoronDeck', 'version': '1.0.0-beta.1',
                          'architecture': 'aarch64', 'source_commit': 'a' * 40}
        self.index = {'schema': 1, 'project': 'VoronDeck', 'channel': 'beta', 'version': '1.0.0-beta.1',
                      'packages': [{'architecture': 'aarch64', 'libc': 'glibc',
                                    'sha256': 'b' * 64, 'source_commit': 'a' * 40}]}
        self.name = '1.0.0-beta.1-' + 'b' * 12

    def compare(self):
        return checker.compare_release(self.installed, self.name, self.index, 'glibc')

    def test_exact_installed_package_is_current(self):
        self.assertEqual(self.compare()['status'], 'current')

    def test_rebuilt_same_version_is_available(self):
        self.index['packages'][0]['sha256'] = 'c' * 64
        self.assertEqual(self.compare()['status'], 'available')

    def test_source_change_is_not_current(self):
        self.index['packages'][0]['source_commit'] = 'c' * 40
        self.assertEqual(self.compare()['status'], 'available')

    def test_versions_and_builds_are_reported(self):
        self.index['version'] = '1.0.0-beta.2'
        result = self.compare()
        self.assertEqual(result['status'], 'available')
        self.assertEqual(result['available_version'], '1.0.0-beta.2')
        self.assertEqual(result['installed_build'], 'a' * 8)

    def test_missing_duplicate_or_incompatible_package_is_error(self):
        package = copy.deepcopy(self.index['packages'][0])
        for packages in ([], [package, package], [dict(package, architecture='armv7l')],
                         [dict(package, libc='musl')]):
            with self.subTest(packages=packages), self.assertRaises(ValueError):
                self.index['packages'] = packages
                self.compare()

    def test_wrong_project_channel_and_malformed_metadata_are_errors(self):
        original = copy.deepcopy(self.index)
        for key, value in (('project', 'HelixScreen'), ('schema', 2), ('channel', 'stable'),
                           ('version', '../bad'), ('packages', None)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.index = dict(original, **{key: value})
                self.compare()
        for key in ('sha256', 'source_commit'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.index = copy.deepcopy(original)
                self.index['packages'][0][key] = 'invalid'
                self.compare()


class SshCommand(unittest.TestCase):
    def test_default_keeps_noninteractive_strict_host_check(self):
        with tempfile.TemporaryDirectory() as temp:
            args = shlex.split(checker.ssh_command(Path(temp)))
            self.assertIn('BatchMode=yes', args)
            self.assertIn('StrictHostKeyChecking=yes', args)
            self.assertNotIn('-i', args)

    def test_dedicated_identity_and_pins_are_quoted_and_require_both_files(self):
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp) / 'user space $(not-a-command)'
            ssh = home / '.ssh'
            ssh.mkdir(parents=True)
            key = ssh / 'vorondeck_beta_readonly'
            hosts = ssh / 'known_hosts_vorondeck'
            key.touch()
            self.assertNotIn('-i', shlex.split(checker.ssh_command(home)))
            hosts.touch()
            args = shlex.split(checker.ssh_command(home))
            self.assertEqual(args[args.index('-i') + 1], str(key))
            self.assertIn('IdentitiesOnly=yes', args)
            self.assertIn('UserKnownHostsFile=' + str(hosts), args)
            self.assertIn('StrictHostKeyChecking=yes', args)


class Channels(unittest.TestCase):
    setUp = ReleaseCheck.setUp
    def test_beta_install_keeps_active_channel_when_stable_exists(self):
        stable = copy.deepcopy(self.index)
        stable.update(channel='stable', version='1.0.0')
        result = checker.check_channels(self.installed, self.name, 'glibc', True,
                                       lambda channel: self.index if channel == 'beta' else stable)
        self.assertEqual(result['active_channel'], 'beta')
        self.assertEqual(result['status'], 'current')
        self.assertEqual(result['channels']['stable']['version'], '1.0.0')
        self.assertEqual(result['channels']['beta']['version'], '1.0.0-beta.1')

    def test_unpublished_stable_is_unavailable_not_a_fake_version(self):
        def fetch(channel):
            if channel == 'stable':
                raise ValueError('No published stable index')
            return self.index
        result = checker.check_channels(self.installed, self.name, 'glibc', True, fetch)
        self.assertEqual(result['status'], 'current')
        self.assertEqual(result['channels']['stable'], {'status': 'unavailable', 'version': '', 'build': ''})

    def test_missing_beta_key_does_not_probe_private_channel(self):
        calls = []
        def fetch(channel):
            calls.append(channel)
            raise ValueError('Not published')
        result = checker.check_channels(self.installed, self.name, 'glibc', False, fetch)
        self.assertEqual(calls, ['stable'])
        self.assertEqual(result['status'], 'access_required')
        self.assertEqual(result['channels']['beta']['status'], 'access_required')


if __name__ == '__main__':
    unittest.main()
