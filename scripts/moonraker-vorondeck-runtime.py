#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Non-root package operations used by the VoronDeck Moonraker updater."""
from contextlib import contextmanager
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


load('vorondeck_distribution', ROOT / 'scripts/vorondeck_distribution.py')
installer = load('_vorondeck_updater_installer', ROOT / 'scripts/install-vorondeck.py')
bootstrap = load('_vorondeck_updater_bootstrap', ROOT / 'scripts/vorondeck-bootstrap.py')
checker = load('_vorondeck_updater_checker', ROOT / 'assets/updates/check-vorondeck-update.py')


def release_state(base):
    current = base / 'current'
    if not current.is_symlink():
        raise ValueError('VoronDeck current release is not a managed link')
    release = current.resolve(strict=True)
    if release.parent != (base / 'releases').resolve():
        raise ValueError('VoronDeck release is outside the managed directory')
    path = release / 'vorondeck-release.json'
    if path.stat().st_size > 2 * 1024**2:
        raise ValueError('Release manifest is too large')
    manifest = json.loads(path.read_text())
    version = manifest.get('version', '')
    if (manifest.get('project') != 'VoronDeck' or manifest.get('schema') != 1
            or not re.fullmatch(r'\d+\.\d+\.\d+(?:-beta\.\d+)?', version)
            or not re.fullmatch(r'[a-f0-9]{40}', manifest.get('source_commit', ''))
            or manifest.get('architecture') != installer.host_arch()):
        raise ValueError('Invalid installed VoronDeck metadata')
    match = re.fullmatch(re.escape(version) + r'-([a-f0-9]{12})', release.name)
    if not match or not (release / 'bin/helix-screen').is_file():
        raise ValueError('Incomplete installed VoronDeck release')
    return {'path': str(release), 'version': version, 'source_commit': manifest['source_commit'],
            'package_hash': match.group(1), 'architecture': manifest['architecture']}


@contextmanager
def install_lock(base):
    import fcntl
    base.mkdir(parents=True, exist_ok=True)
    path = base / 'installer.lock'
    if path.is_symlink():
        raise ValueError('Linked installer lock refused')
    with path.open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


@contextmanager
def release_index(channel):
    with checker.release_index(channel) as (git, env, commit, index):
        if index.get('channel') != channel:
            raise ValueError('Release index channel mismatch')
        version = index.get('version', '')
        pattern = r'\d+\.\d+\.\d+-beta\.\d+' if channel == 'beta' else r'\d+\.\d+\.\d+'
        if not re.fullmatch(pattern, version):
            raise ValueError('Release version does not belong to the selected channel')
        package = bootstrap.select_package(index, installer.host_arch(), bootstrap.libc_name())
        if not re.fullmatch(r'[a-f0-9]{40}', package.get('source_commit', '')):
            raise ValueError('Invalid package source revision')
        yield git, env, commit, dict(package, version=version)


def remote_release(channel):
    with release_index(channel) as (_, _, _, package):
        return package


def prepare_release(base, channel):
    """Download, authenticate and stage only; the old screen stays running."""
    with release_index(channel) as (git, env, commit, package):
        destination = base / 'releases' / (package['version'] + '-' + package['sha256'][:12])
        current = release_state(base)
        if Path(current['path']) == destination and current['source_commit'] == package['source_commit']:
            return destination
        if shutil.disk_usage(base).free < package['size'] * 2 + 64 * 1024**2:
            raise ValueError('Not enough free disk space for the package download')
        blob = commit + ':' + package['path']
        # Reading a promised binary blob may fetch it: allow slow printer Wi-Fi.
        size = int(checker.run(git + ['cat-file', '-s', blob], env, 1800))
        if size != package['size']:
            raise ValueError('Package size mismatch')
        with tempfile.TemporaryDirectory(prefix='vorondeck-update-') as temp:
            archive = Path(temp) / 'package.tar.gz'
            archive.write_bytes(checker.run(git + ['show', blob], env, 180))
            manifest, payload = installer.verify_package(archive, package['sha256'], installer.host_arch())
            if manifest['version'] != package['version'] or manifest['source_commit'] != package['source_commit']:
                raise ValueError('Package differs from the authenticated release index')
            installer.stage_release(destination, manifest, payload, allow_dependencies=False)
            if json.loads((destination / 'vorondeck-release.json').read_text()) != manifest:
                raise ValueError('Existing staged release metadata differs from the package')
            return destination


def switch_release(base, previous, destination):
    if Path(release_state(base)['path']) != previous:
        raise ValueError('Installed release changed while preparing the update')
    if destination.resolve().parent != (base / 'releases').resolve() or destination.is_symlink():
        raise ValueError('Invalid staged release target')
    backup = base / 'backups' / ('update-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
    backup.mkdir(parents=True, mode=0o700)
    installer.atomic_write(backup / 'update.json', json.dumps({
        'previous_target': str(previous), 'new_target': str(destination),
        'settings_unchanged': True}).encode())
    installer.replace_link(base / 'current', destination)
    return backup


def restore_release(base, previous):
    if previous.resolve().parent != (base / 'releases').resolve():
        raise ValueError('Invalid rollback target')
    installer.replace_link(base / 'current', previous)


def service_active():
    return installer.Services.detect().active('vorondeck')
