#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install the local Moonraker package updater, restoring its config on failure."""
import fnmatch
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import time
from urllib.request import urlopen

MARKER = '# Managed by VoronDeck updater setup'


def _safe(path, home, *, link=False):
    path = Path(os.path.abspath(path))
    if path == home or not path.is_relative_to(home):
        raise ValueError('Updater paths must stay inside the printer user home')
    for part in (path.parent if link else path, *(path.parent.parents if link else path.parents)):
        if part == home:
            break
        if part.is_symlink():
            raise ValueError('Linked updater paths are not supported: ' + str(part))
    return path


def _state(path):
    if path.is_symlink():
        return ('link', os.readlink(path), None)
    if path.exists():
        if not path.is_file():
            raise ValueError('Updater destination is not a regular file: ' + str(path))
        return ('file', path.read_bytes(), stat.S_IMODE(path.stat().st_mode))
    return ('missing', None, None)


def _write(path, state):
    kind, data, mode = state
    if kind == 'missing':
        path.unlink(missing_ok=True)
        return
    fd, name = tempfile.mkstemp(prefix='.vorondeck-', dir=path.parent)
    temp = Path(name)
    try:
        with os.fdopen(fd, 'wb') as stream:
            if kind == 'file':
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
        if kind == 'link':
            temp.unlink()
            temp.symlink_to(data)
        else:
            temp.chmod(mode)
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def probe_loaded(port):
    """Read only local Moonraker state; never request an update or refresh."""
    def get(endpoint):
        with urlopen(f'http://127.0.0.1:{port}/{endpoint}', timeout=3) as response:
            return json.load(response)['result']
    try:
        info = get('server/info')
        status = get('machine/update/status')
        return ('vorondeck_update' in info.get('components', [])
                and 'vorondeck_update' not in info.get('failed_components', [])
                and 'vorondeck' in status.get('version_info', {}))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def setup(root_repo, base, config_dir, services, idle_check, channel='beta', port=7125,
          *, home=None, probe=None, sleep=time.sleep, attempts=15):
    """Return backup directory, or None for an already healthy installation.

    `probe` accepts a port and returns whether both component and updater loaded.
    Caller supplies its existing printer-idle guard and service adapter.
    """
    if channel not in ('beta', 'stable') or not 1 <= int(port) <= 65535:
        raise ValueError('Invalid updater channel or Moonraker port')
    if attempts < 1:
        raise ValueError('Updater verification requires at least one attempt')
    home = Path(home or Path.home()).resolve()
    root_repo = _safe(root_repo, home)
    base = _safe(base, home)
    if any(char in str(base) for char in '\r\n'):
        raise ValueError('Invalid updater install path')
    config_dir = _safe(config_dir, home)
    source = _safe(root_repo / 'scripts/moonraker-vorondeck.py', home)
    runtime = _safe(source.with_name('moonraker-vorondeck-runtime.py'), home)
    if not source.is_file() or not runtime.is_file():
        raise ValueError('VoronDeck updater sources are missing from the cloned repository')
    components = _safe(home / 'moonraker/moonraker/components', home)
    if not components.is_dir():
        raise ValueError('Moonraker source not found at ~/moonraker/moonraker/components; updater was not installed')
    main = _safe(config_dir.parent / 'moonraker.conf', home)
    dedicated = _safe(main.with_name('VoronDeck-updates.conf'), home)
    allowlist = _safe(main.parent.parent / 'moonraker.asvc', home)
    module = _safe(components / 'vorondeck_update.py', home, link=True)
    previous = {path: _state(path) for path in (module, dedicated, main, allowlist)}
    if previous[main][0] != 'file':
        raise ValueError('Moonraker configuration not found: ' + str(main))
    if previous[module][0] != 'missing' and not (
            previous[module][0] == 'link' and module.resolve() == source):
        raise ValueError('Refusing to replace an unowned Moonraker updater module')
    if previous[dedicated][0] != 'missing' and not (
            previous[dedicated][0] == 'file'
            and previous[dedicated][1].startswith((MARKER + '\n').encode())):
        raise ValueError('Refusing to replace an unmanaged VoronDeck-updates.conf')
    text = previous[main][1].decode('utf-8')
    if re.search(r'^\s*\[(?:vorondeck_update|update_manager\s+vorondeck)\]\s*(?:[#;].*)?$',
                 text, re.MULTILINE | re.IGNORECASE):
        raise ValueError('Conflicting VoronDeck updater section in moonraker.conf; no files changed')
    includes = re.findall(r'^\s*\[include\s+([^\]]+)\]', text, re.MULTILINE | re.IGNORECASE)
    included = any(fnmatch.fnmatch(dedicated.name, item.strip()) for item in includes)
    main_data = previous[main][1]
    if not included:
        main_data += (b'' if main_data.endswith(b'\n') else b'\n')
        main_data += b'\n[include VoronDeck-updates.conf]\n'
    allowed = previous[allowlist][1] or b''
    if 'vorondeck' not in [line.strip() for line in allowed.decode('utf-8').splitlines()]:
        allowed += (b'' if not allowed or allowed.endswith(b'\n') else b'\n') + b'vorondeck\n'
    wanted = {
        module: ('link', str(source), None),
        dedicated: ('file', (f'{MARKER}\n[vorondeck_update]\nchannel: {channel}\n'
                             f'install_path: {base}\n').encode(), 0o600),
        main: ('file', main_data, previous[main][2]),
        allowlist: ('file', allowed, previous[allowlist][2] or 0o600),
    }
    changed = [path for path in wanted if previous[path] != wanted[path]]
    probe = probe or probe_loaded
    if not changed and probe(port):
        return None
    idle_check()
    backups = _safe(base / 'backups', home)
    backups.mkdir(parents=True, exist_ok=True, mode=0o700)
    backup = Path(tempfile.mkdtemp(prefix=time.strftime('updates-%Y%m%d-%H%M%S-'), dir=backups))
    backup.chmod(0o700)
    manifest = []
    for index, (path, state) in enumerate(previous.items()):
        kind, data, mode = state
        record = {'path': str(path), 'kind': kind, 'mode': mode}
        if kind == 'file':
            snapshot = backup / str(index)
            _write(snapshot, ('file', data, 0o600))
            record['snapshot'] = snapshot.name
        elif kind == 'link':
            record['target'] = data
        manifest.append(record)
    _write(backup / 'manifest.json', ('file', json.dumps(manifest, indent=2).encode(), 0o600))
    written = []
    restart_attempted = False
    try:
        for path in changed:
            # Detect a concurrent config edit before replacing its bytes.
            _safe(path, home, link=path == module)
            if _state(path) != previous[path]:
                raise ValueError('Updater destination changed during installation: ' + str(path))
            _write(path, wanted[path])
            written.append(path)
        idle_check()
        restart_attempted = True
        services.action('restart', 'moonraker')
        for attempt in range(attempts):
            if probe(port):
                return backup
            if attempt + 1 < attempts:
                sleep(1)
        raise RuntimeError('Moonraker did not load the VoronDeck updater')
    except BaseException as error:
        failures = []
        for path in reversed(written):
            try:
                _safe(path, home, link=path == module)
                _write(path, previous[path])
            except Exception:
                failures.append(str(path))
        if restart_attempted:
            try:
                services.action('restart', 'moonraker')
            except Exception:
                failures.append('Moonraker restart')
        details = ' Rollback incomplete: ' + ', '.join(failures) if failures else ' Previous configuration restored.'
        raise RuntimeError('VoronDeck updater setup failed.' + details + ' Backup: ' + str(backup)) from error
