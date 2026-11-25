# SPDX-License-Identifier: GPL-3.0-or-later
"""[vorondeck_update] вЂ” authenticated binary packages in Moonraker update_manager.

Link this file as moonraker/components/vorondeck_update.py. It registers a
BaseDeploy instance without modifying Moonraker's source or executing remote code.
"""
from __future__ import annotations
import asyncio
import importlib.util
import subprocess
from pathlib import Path

from .update_manager.base_deploy import BaseDeploy


def _load_runtime():
    path = Path(__file__).resolve().with_name('moonraker-vorondeck-runtime.py')
    spec = importlib.util.spec_from_file_location('_vorondeck_moonraker_runtime', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class VoronDeckDeploy(BaseDeploy):
    def __init__(self, config):
        server = config.get_server()
        manager = server.load_component(config, 'update_manager')
        if not callable(getattr(manager, 'get_updaters', None)):
            raise config.error('This Moonraker does not support the VoronDeck updater')
        if 'vorondeck' in manager.get_updaters():
            raise config.error('Remove the conflicting [update_manager vorondeck] section')
        super().__init__(config, name='vorondeck', prefix='VoronDeck')
        self.channel = config.get('channel', 'beta')
        if self.channel not in ('beta', 'stable'):
            raise config.error('VoronDeck channel must be beta or stable')
        self.runtime = _load_runtime()
        base = Path(config.get('install_path', str(Path.home() / '.local/share/vorondeck'))).expanduser()
        if not base.is_absolute() or base.is_symlink():
            raise config.error('VoronDeck install_path must be an absolute managed directory')
        self.base = self.runtime.installer.safe_child(base, Path.home().resolve())
        self.remote = None
        self.current = None
        self.last_error = ''
        self.selected_update_pending = False
        self.loop = server.get_event_loop()
        manager.get_updaters()['vorondeck'] = self
        from ..common import RequestType
        server.register_endpoint('/machine/update/vorondeck_test', RequestType.POST,
                                 self._handle_test_update)
        server.register_endpoint('/machine/update/vorondeck_release', RequestType.POST,
                                 self._handle_test_update)

    async def _handle_test_update(self, request):
        selection = {key: request.get_str(key) for key in
                     ('kind', 'version', 'tag', 'sha256', 'source_commit', 'installed_source')}
        manager = self.server.lookup_component('update_manager')
        if (not hasattr(manager, 'cmd_request_lock') or not hasattr(manager, 'cmd_helper')
                or not callable(getattr(manager.cmd_helper, 'is_update_busy', None))):
            raise self.server.error('This Moonraker cannot serialize selected updates')
        if (getattr(self, 'selected_update_pending', False)
                or manager.cmd_helper.is_update_busy()):
            raise self.server.error('Another update is running', 503)
        self.selected_update_pending = True
        acquired = False
        try:
            try:
                # Refresh shares this lock. Queue behind it, never bypass it.
                await asyncio.wait_for(manager.cmd_request_lock.acquire(), timeout=90)
                acquired = True
            except asyncio.TimeoutError:
                raise self.server.error('Update check did not finish within 90 seconds. Try again later.', 503)
            manager.cmd_helper.set_update_info('vorondeck', id(request))
            try:
                await self.update(selection)
            except Exception as error:
                manager.cmd_helper.notify_update_response('VoronDeck update failed', is_complete=True)
                if isinstance(error, BlockingIOError):
                    reason = 'Another update is running'
                elif isinstance(error, subprocess.TimeoutExpired):
                    reason = 'Package download timed out. Check the network and beta access.'
                else:
                    reason = str(error)[:300] or type(error).__name__
                raise self.server.error(reason, getattr(error, 'status_code', 500)) from error
            finally:
                manager.cmd_helper.clear_update_info()
        finally:
            if acquired:
                manager.cmd_request_lock.release()
            self.selected_update_pending = False
        return 'ok'

    async def initialize(self):
        storage = await super().initialize()
        self.remote = storage.get('vorondeck_release') if storage.get('vorondeck_channel') == self.channel else None
        try:
            self.current = await self.loop.run_in_thread(self.runtime.release_state, self.base)
        except Exception:
            self.last_error = 'Cannot read the installed VoronDeck package'
        return storage

    def get_persistent_data(self):
        data = super().get_persistent_data()
        data['vorondeck_release'] = self.remote
        data['vorondeck_channel'] = self.channel
        return data

    async def refresh(self):
        try:
            self.current = await self.loop.run_in_thread(self.runtime.release_state, self.base)
            self.remote = await self.loop.run_in_thread(self.runtime.remote_release, self.channel)
            self.last_error = ''
            self._save_state()
        except Exception:
            # A failed check is not a current-version verdict, and must not offer
            # installation from stale cached metadata.
            self.last_error = 'VoronDeck check failed: verify the network and channel access'
            self.remote = None

    def get_update_status(self):
        current, remote = self.current or {}, self.remote or {}
        current_hash = current.get('package_hash', '')
        remote_hash = remote.get('sha256', '')
        version = current.get('version', '?')
        available = remote.get('version', '?')
        # Build metadata distinguishes same-version beta repackages in clients.
        if current_hash:
            version += '+' + current_hash
        if remote_hash:
            available += '+' + remote_hash[:12]
        valid = bool(self.current and self.remote and not self.last_error)
        non_older = valid and self.runtime.checker.public.version_key(remote['version']) >= self.runtime.checker.public.version_key(current['version'])
        return {'name': self.name, 'configured_type': 'executable',
                'detected_type': 'executable', 'channel': self.channel,
                'channel_invalid': False, 'is_valid': valid, 'is_dirty': False,
                'owner': 'Redaktor-clok', 'repo_name': 'VoronDeck-beta' if self.channel == 'beta' else 'VoronDeck',
                'version': version, 'remote_version': available, 'rollback_version': '?',
                'vorondeck_package': True, 'vorondeck_test_updates': True, 'package_sha256': current_hash,
                'vorondeck_release_selection': True,
                'vorondeck_update_progress': 1,
                'remote_package_sha256': remote_hash,
                'update_available': bool(non_older and version != available),
                'last_error': self.last_error, 'warnings': [self.last_error] if self.last_error else [],
                'anomalies': [], 'info_tags': ['Verified binary package', 'SSH beta' if self.channel == 'beta' else 'Stable']}

    async def _assert_idle(self):
        api = self.server.lookup_component('klippy_apis')
        status = await api.query_objects({'print_stats': None, 'idle_timeout': None, 'heaters': None})
        heaters = status.get('heaters', {}).get('available_heaters')
        if not isinstance(heaters, list):
            raise self.server.error('Cannot verify heater state before VoronDeck update')
        if heaters:
            status.update(await api.query_objects({name: ['target'] for name in heaters}))
        self.runtime.installer.assert_idle(status)

    async def update(self, selection=None):
        machine = self.server.lookup_component('machine')
        if not machine.is_service_allowed('vorondeck'):
            raise self.server.error('Add vorondeck to moonraker.asvc before updating')
        await self._assert_idle()
        # Share the CLI installer's lock through staging, activation and rollback.
        with self.runtime.install_lock(self.base):
            previous = Path((await self.loop.run_in_thread(self.runtime.release_state, self.base))['path'])
            self.notify_status('Downloading and verifying the VoronDeck package')
            preparation = asyncio.ensure_future(self.loop.run_in_thread(
                self.runtime.prepare_release if selection is None else self.runtime.prepare_selected_release,
                self.base, self.channel if selection is None else selection))
            try:
                destination = await asyncio.shield(preparation)
            except asyncio.CancelledError:
                # Keep the shared installer lock until the staging worker exits.
                await asyncio.shield(preparation)
                raise
            await self._assert_idle()
            if destination == previous:
                await self.refresh()
                self.notify_status('VoronDeck is already up to date', is_complete=True)
                return False
            if not await self.loop.run_in_thread(self.runtime.service_active):
                raise self.server.error('VoronDeck must be running before an update')
            self.notify_status('Activating VoronDeck; screen settings are preserved')
            switched = False
            try:
                # This is only a small metadata write + atomic rename. No await
                # may split the switch from arming its rollback handler.
                self.runtime.switch_release(self.base, previous, destination)
                switched = True
                await machine.do_service_action('restart', 'vorondeck')
                for _ in range(5):
                    await asyncio.sleep(1)
                    if not await self.loop.run_in_thread(self.runtime.service_active):
                        raise self.server.error('Updated VoronDeck failed its startup check')
            except BaseException:
                if switched:
                    self.runtime.restore_release(self.base, previous)
                    await machine.do_service_action('restart', 'vorondeck')
                raise
            self.current = await self.loop.run_in_thread(self.runtime.release_state, self.base)
            await self.refresh()
            self.notify_status('VoronDeck updated successfully', is_complete=True)
            return True


def load_component(config):
    return VoronDeckDeploy(config)
