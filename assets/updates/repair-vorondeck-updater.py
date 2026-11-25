#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Activate the packaged updater only after an explicit on-screen confirmation."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
from urllib.request import Request, urlopen


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def verify_bundle(root):
    manifest = json.loads((root / 'vorondeck-release.json').read_text())
    if manifest.get('schema') != 1 or manifest.get('project') != 'VoronDeck':
        raise ValueError('Not a managed release')
    manager = root / 'assets/updates/manager'
    expected = {name: digest for name, digest in manifest['files'].items()
                if name.startswith('assets/updates/manager/')}
    if not expected:
        raise ValueError('Updater bundle is absent')
    for name, digest in expected.items():
        path = root / name
        if (not path.resolve().is_relative_to(manager.resolve()) or path.is_symlink()
                or hashlib.sha256(path.read_bytes()).hexdigest() != digest):
            raise ValueError('Updater bundle differs from installed package')
    return manager


def repair(root, home, port, *, request=None, sleep=time.sleep, attempts=20):
    if not 1 <= port <= 65535:
        raise ValueError('Invalid Moonraker port')
    root, home = root.resolve(), home.resolve()
    base = root.parent.parent
    if (root.parent.name != 'releases' or not base.is_relative_to(home)
            or not (base / 'current').is_symlink() or (base / 'current').resolve() != root):
        raise ValueError('Not the active managed release')
    manager = verify_bundle(root)
    runtime = load('_vorondeck_repair_runtime', manager / 'scripts/moonraker-vorondeck-runtime.py')
    setup = load('_vorondeck_repair_setup', manager / 'scripts/setup-vorondeck-updates.py')
    module = setup._safe(home / 'moonraker/moonraker/components/vorondeck_update.py', home, link=True)
    source = manager / 'scripts/moonraker-vorondeck.py'
    previous = setup._state(module)
    if previous[0] != 'link':
        raise ValueError('Unmanaged Moonraker module')
    old = module.resolve(strict=True)
    known = [home / name / 'scripts/moonraker-vorondeck.py' for name in ('VoronDeck', 'VoronDeck-beta')]
    packaged = (old.is_relative_to(base / 'releases')
                and old.parts[-5:] == ('assets', 'updates', 'manager', 'scripts', 'moonraker-vorondeck.py'))
    if old not in known and not packaged:
        raise ValueError('Unowned Moonraker module')

    def http(path, data=None):
        url = f'http://127.0.0.1:{port}/' + path
        req = Request(url, data=json.dumps(data).encode() if data is not None else None,
                      headers={'Content-Type': 'application/json'})
        with urlopen(req, timeout=5) as response:
            if response.geturl() != url:
                raise ValueError('Local Moonraker redirected the request')
            raw = response.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise ValueError('Oversized Moonraker response')
        return json.loads(raw)['result']

    request = request or http

    def idle():
        status = request('machine/update/status')
        if status.get('busy', True):
            raise ValueError('Another update is running')
        from urllib.parse import quote
        state = request('printer/objects/query?print_stats&idle_timeout&heaters')['status']
        heaters = state.get('heaters', {}).get('available_heaters')
        if not isinstance(heaters, list):
            raise ValueError('Cannot verify heaters')
        if heaters:
            state.update(request('printer/objects/query?' + '&'.join(
                quote(name, safe='') for name in heaters))['status'])
        runtime.installer.assert_idle(state)

    def loaded():
        try:
            info = request('server/info')
            updater = request('machine/update/status').get('version_info', {}).get('vorondeck', {})
            return ('vorondeck_update' not in info.get('failed_components', [])
                    and updater.get('vorondeck_release_selection') is True
                    and updater.get('vorondeck_update_progress', 0) >= 1)
        except (OSError, ValueError, KeyError, TypeError):
            return False

    def restart():
        try:
            request('machine/services/restart', {'service': 'moonraker'})
        except OSError:
            # The service may close its HTTP connection as it restarts; probe it.
            pass

    with runtime.install_lock(base):
        idle()
        if old == source and loaded():
            return
        backups = setup._safe(base / 'backups', home)
        backups.mkdir(exist_ok=True, mode=0o700)
        backup = Path(tempfile.mkdtemp(prefix='updater-', dir=backups))
        setup._write(backup / 'module.json', ('file', json.dumps({
            'path': str(module), 'target': previous[1]}).encode(), 0o600))
        wanted = ('link', str(base / 'current/assets/updates/manager/scripts/moonraker-vorondeck.py'), None)
        if setup._state(module) != previous or (base / 'current').resolve() != root:
            raise ValueError('Installation changed during repair')
        setup._write(module, wanted)
        restarted = False
        try:
            idle()
            restarted = True
            restart()
            for _ in range(attempts):
                if loaded():
                    return
                sleep(1)
            raise RuntimeError('Updated manager did not load')
        except BaseException:
            if setup._state(module) == wanted:
                setup._write(module, previous)
                if restarted:
                    restart()
            raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    args = parser.parse_args()
    try:
        repair(Path(__file__).resolve().parents[2], Path.home(), args.port)
    except Exception:
        print('Updater repair failed; verify the local Moonraker configuration.', file=sys.stderr)
        raise SystemExit(1)
