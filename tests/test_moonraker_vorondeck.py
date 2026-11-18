# SPDX-License-Identifier: GPL-3.0-or-later
"""Hermetic transaction tests; no printer, service, network or package mutations."""
import contextlib
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch

root = Path(__file__).resolve().parents[1]
package = types.ModuleType('_vd_test_moonraker')
package.__path__ = []
updates = types.ModuleType('_vd_test_moonraker.update_manager')
updates.__path__ = []
base = types.ModuleType('_vd_test_moonraker.update_manager.base_deploy')

class BaseDeploy:
    def notify_status(self, text, is_complete=False):
        self.events.append(('notice', text, is_complete))

base.BaseDeploy = BaseDeploy
for module in (package, updates, base):
    sys.modules[module.__name__] = module
spec = importlib.util.spec_from_file_location('_vd_test_moonraker.vorondeck_update',
                                             root / 'scripts/moonraker-vorondeck.py')
component = importlib.util.module_from_spec(spec)
spec.loader.exec_module(component)


class DirectLoop:
    async def run_in_thread(self, callback, *args):
        return callback(*args)


class RuntimeNoop(unittest.TestCase):
    def test_current_package_does_not_download_or_stage(self):
        runtime = component._load_runtime()
        base = Path('/managed')
        release = {'version': '1.0.0-beta.1', 'sha256': 'b' * 64, 'source_commit': 'a' * 40}
        target = base / 'releases' / ('1.0.0-beta.1-' + 'b' * 12)
        @contextlib.contextmanager
        def index(channel):
            yield [], {}, 'c' * 40, release
        with patch.object(runtime, 'release_index', index), patch.object(runtime, 'release_state',
                return_value={'path': str(target), 'source_commit': 'a' * 40}), \
                patch.object(runtime.checker, 'run', side_effect=AssertionError('Unexpected package fetch')):
            self.assertEqual(runtime.prepare_release(base, 'beta'), target)


class Transaction(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.events = []
        self.current = Path('/managed/releases/old')
        self.new = Path('/managed/releases/new')
        self.busy = False
        self.fail_prepare = False
        self.fail_restart = False
        self.target = 0
        self.active = True
        self.node = component.VoronDeckDeploy.__new__(component.VoronDeckDeploy)
        self.node.base = Path('/managed')
        self.node.channel = 'beta'
        self.node.name = 'vorondeck'
        self.node.events = self.events
        self.node.loop = DirectLoop()
        self.node.refresh = AsyncMock()

        def validate(status):
            if status['print_stats']['state'] != 'standby' or status['extruder']['target']:
                raise ValueError('Printer not idle')
        def prepare(base, channel):
            self.events.append(('prepare', channel))
            if self.fail_prepare:
                raise ValueError('Package verification failed')
            return self.new
        def switch(base, previous, destination):
            self.events.append(('switch', previous, destination))
            self.current = destination
        def restore(base, previous):
            self.events.append(('restore', previous))
            self.current = previous
        async def restart(action, service):
            self.events.append(('service', action, service))
            if self.fail_restart:
                self.fail_restart = False
                raise ValueError('Restart failed')
        async def query(objects):
            if 'heaters' in objects:
                return {'heaters': {'available_heaters': ['extruder']},
                        'print_stats': {'state': 'printing' if self.busy else 'standby'},
                        'idle_timeout': {'state': 'Idle'}}
            return {'extruder': {'target': self.target}}
        machine = types.SimpleNamespace(is_service_allowed=lambda name: name == 'vorondeck',
                                        do_service_action=restart)
        api = types.SimpleNamespace(query_objects=query)
        self.node.server = types.SimpleNamespace(
            lookup_component=lambda name: machine if name == 'machine' else api,
            error=lambda text: ValueError(text))
        self.node.runtime = types.SimpleNamespace(
            installer=types.SimpleNamespace(assert_idle=validate),
            install_lock=lambda base: contextlib.nullcontext(),
            release_state=lambda base: {'path': str(self.current)},
            prepare_release=prepare, switch_release=switch, restore_release=restore,
            service_active=lambda: self.active)
        self.sleep = patch.object(component.asyncio, 'sleep', new=AsyncMock())
        self.sleep.start()
        self.addCleanup(self.sleep.stop)

    async def test_success_stages_before_switch_and_restarts_only_screen(self):
        self.assertTrue(await self.node.update())
        actions = [entry[0] for entry in self.events]
        self.assertLess(actions.index('prepare'), actions.index('switch'))
        self.assertEqual([e for e in self.events if e[0] == 'service'], [('service', 'restart', 'vorondeck')])
        self.assertEqual(self.current, self.new)

    async def test_busy_printer_refused_before_download(self):
        self.busy = True
        with self.assertRaises(ValueError):
            await self.node.update()
        self.assertFalse(any(e[0] in ('prepare', 'switch', 'service') for e in self.events))

    async def test_heating_refused_before_download(self):
        self.target = 200
        with self.assertRaises(ValueError):
            await self.node.update()
        self.assertFalse(any(e[0] == 'prepare' for e in self.events))

    async def test_failed_package_keeps_old_screen(self):
        self.fail_prepare = True
        with self.assertRaises(ValueError):
            await self.node.update()
        self.assertEqual(self.current, Path('/managed/releases/old'))
        self.assertFalse(any(e[0] in ('switch', 'service') for e in self.events))

    async def test_second_idle_check_blocks_switch(self):
        original = self.node.runtime.prepare_release
        def prepare(*args):
            result = original(*args)
            self.busy = True
            return result
        self.node.runtime.prepare_release = prepare
        with self.assertRaises(ValueError):
            await self.node.update()
        self.assertFalse(any(e[0] in ('switch', 'service') for e in self.events))

    async def test_restart_failure_restores_previous_release(self):
        self.fail_restart = True
        with self.assertRaises(ValueError):
            await self.node.update()
        self.assertEqual(self.current, Path('/managed/releases/old'))
        self.assertEqual(len([e for e in self.events if e[0] == 'service']), 2)
        self.assertTrue(any(e[0] == 'restore' for e in self.events))

    async def test_same_package_does_not_restart_screen(self):
        self.new = self.current
        self.assertFalse(await self.node.update())
        self.assertFalse(any(e[0] in ('switch', 'service') for e in self.events))

    async def test_stopped_screen_is_not_started_by_update(self):
        self.active = False
        with self.assertRaises(ValueError):
            await self.node.update()
        self.assertFalse(any(e[0] in ('switch', 'service') for e in self.events))

    def test_status_exposes_verified_package_discriminator_and_build_change(self):
        self.node.current = {'version': '1.0.0-beta.1', 'package_hash': 'a' * 12}
        self.node.remote = {'version': '1.0.0-beta.1', 'sha256': 'b' * 64}
        self.node.last_error = ''
        status = self.node.get_update_status()
        self.assertTrue(status['vorondeck_package'])
        self.assertEqual(status['configured_type'], 'executable')
        self.assertTrue(status['update_available'])
        self.node.last_error = 'Check failed'
        self.assertFalse(self.node.get_update_status()['update_available'])


if __name__ == '__main__':
    unittest.main()
