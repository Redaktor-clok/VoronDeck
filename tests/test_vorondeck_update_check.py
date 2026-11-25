# SPDX-License-Identifier: GPL-3.0-or-later
"""Release checks must distinguish rebuilt packages with the same version."""
import copy
import importlib.util
from pathlib import Path
import shlex
import tempfile
import unittest
from unittest.mock import patch

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

    def test_both_newer_channels_offer_install_independent_of_active_channel(self):
        self.index['version'] = '1.0.2-beta.1'
        stable = dict(self.index, channel='stable', version='1.0.1')
        for active in ('stable', 'beta'):
            result = checker.check_channels(self.installed, self.name, 'glibc', True,
                                           lambda c: stable if c == 'stable' else self.index,
                                           selected_channel=active)
            self.assertEqual([o['kind'] for o in result['offers']], ['stable', 'beta'])

    def test_older_beta_is_not_offered_to_stable_install(self):
        self.installed['version'] = '1.0.1'
        stable = dict(self.index, channel='stable', version='1.0.2')
        result = checker.check_channels(self.installed, self.name, 'glibc', True,
                                       lambda c: stable if c == 'stable' else self.index)
        self.assertEqual([o['kind'] for o in result['offers']], ['stable'])

    def test_revoked_beta_access_still_allows_newer_stable(self):
        stable = dict(self.index, channel='stable', version='1.0.1')
        def fetch(channel):
            if channel == 'beta':
                raise OSError('Access revoked')
            return stable
        result = checker.check_channels(self.installed, self.name, 'glibc', True, fetch)
        self.assertEqual([o['kind'] for o in result['offers']], ['stable'])
        self.assertEqual(result['channels']['beta']['status'], 'error')
    def test_selected_stable_overrides_installed_beta(self):
        stable = copy.deepcopy(self.index)
        stable.update(channel='stable', version='1.0.0')
        result = checker.check_channels(self.installed, self.name, 'glibc', True,
                                       lambda channel: stable if channel == 'stable' else self.index,
                                       selected_channel='stable')
        self.assertEqual(result['active_channel'], 'stable')
        self.assertEqual(result['notification_id'], 'stable:1.0.0:aaaaaaaa')

    def test_selected_authorized_beta_overrides_installed_stable(self):
        self.installed['version'] = '1.0.0'
        self.index['version'] = '1.0.1-beta.1'
        result = checker.check_channels(self.installed, self.name, 'glibc', True,
                                       lambda channel: self.index, selected_channel='beta')
        self.assertEqual(result['active_channel'], 'beta')
        self.assertEqual(result['notification_id'], 'beta:1.0.1-beta.1:aaaaaaaa')

    def test_selected_beta_without_key_does_not_fetch_or_notify(self):
        self.installed['version'] = '1.0.0'
        calls = []
        def fetch(channel):
            calls.append(channel)
            raise ValueError('No stable index')
        result = checker.check_channels(self.installed, self.name, 'glibc', False,
                                       fetch, selected_channel='beta')
        self.assertEqual(calls, ['stable'])
        self.assertEqual(result['active_channel'], 'beta')
        self.assertEqual(result['status'], 'access_required')
        self.assertEqual(result['notification_id'], '')

    def test_invalid_selected_channel_is_rejected_before_fetch(self):
        with self.assertRaises(ValueError):
            checker.check_channels(self.installed, self.name, 'glibc', True,
                                   lambda channel: self.fail('must not fetch'), selected_channel='dev')

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


class TestChannel(unittest.TestCase):
    def test_independent_tracking_and_install_eligibility(self):
        package = dict(kind='test', version='1.0.2-T.1', display_version='1.0.2 T1',
                       tag='v1.0.2-T.1', sha256='b' * 64, source_commit='a' * 40)
        for installed_version, stable, expected in (
                ('1.0.1', '1.0.1', 'available'),
                ('1.0.2-beta.2', '1.0.1', 'older'),
                ('1.0.2-T.1', '1.0.1', 'current'),
                ('1.0.1', '1.0.2', 'older')):
            with self.subTest(installed=installed_version, stable=stable):
                installed = dict(version=installed_version, source_commit='a' * 40,
                                 architecture='aarch64')
                result = dict(channels={'stable': {'version': stable}}, offers=[])
                with patch.object(checker.public, 'latest', return_value=package) as latest:
                    checker.check_test_channel(result, installed,
                                               installed_version + '-' + 'b' * 12, 'glibc')
                    latest.assert_called_once_with('aarch64', 'glibc')
                self.assertEqual(result['channels']['test'],
                                 {'status': expected, 'version': '1.0.2 T1'})
                self.assertEqual(bool(result['offers']), expected == 'available')

    def test_missing_and_failed_test_channel_are_distinct(self):
        installed = dict(version='1.0.1', source_commit='a' * 40, architecture='aarch64')
        for failure, expected in ((None, 'none'), (OSError('offline'), 'unavailable')):
            result = dict(channels={}, offers=[])
            with patch.object(checker.public, 'latest', return_value=None, side_effect=failure):
                checker.check_test_channel(result, installed, '1.0.1', 'glibc')
            self.assertEqual(result['test_status'], expected)
            self.assertEqual(result['offers'], [])


class Notifications(unittest.TestCase):
    def result(self):
        return dict(status='available', active_channel='stable', installed_version='1.0.0',
                    available_version='1.0.1', available_build='a' * 8)

    def test_new_stable_has_persistent_identity(self):
        result = self.result()
        self.assertEqual(checker.notification_id(result), 'stable:1.0.1:aaaaaaaa')
        self.assertEqual(checker.notification_id(dict(result)), checker.notification_id(result))

    def test_errors_current_and_unauthorized_beta_do_not_notify(self):
        for status in ('current', 'error', 'access_required', 'not_installed'):
            result = self.result()
            result['status'] = status
            self.assertEqual(checker.notification_id(result), '')

    def test_beta_cannot_be_announced_as_stable(self):
        result = self.result()
        result['available_version'] = '1.0.1-beta.1'
        self.assertEqual(checker.notification_id(result), '')
        result['active_channel'] = 'beta'
        self.assertEqual(checker.notification_id(result), 'beta:1.0.1-beta.1:aaaaaaaa')

    def test_downgrade_is_not_a_new_update(self):
        result = self.result()
        result.update(installed_version='1.0.10', available_version='1.0.9')
        self.assertEqual(checker.notification_id(result), '')
        result.update(active_channel='beta', installed_version='1.0.0-beta.10',
                      available_version='1.0.0-beta.9')
        self.assertEqual(checker.notification_id(result), '')

    def test_rebuilt_package_is_a_distinct_notice(self):
        result = self.result()
        result['available_version'] = result['installed_version']
        old = checker.notification_id(result)
        result['available_build'] = 'b' * 8
        self.assertNotEqual(checker.notification_id(result), old)


if __name__ == '__main__':
    unittest.main()
