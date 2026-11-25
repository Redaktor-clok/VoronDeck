#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the release archive with both the current and legacy installers."""
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path.cwd()
OUT = ROOT / 'build/package'
LEGACY = 'e38801b898fa007bd2016867e00549e7dd9d6051'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    sys.path.insert(0, str(ROOT / 'scripts'))
    installer = load('beta_installer', ROOT / 'scripts/install-vorondeck.py')
    record = json.loads((OUT / 'package.json').read_text())
    archive = Path(record['package']).resolve()
    assert archive.is_relative_to(OUT / 'packages')
    manifest, payload = installer.verify_package(archive, record['sha256'], 'aarch64')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    version = (ROOT / 'VERSION.txt').read_text().strip()
    assert re.fullmatch(r'\d+\.\d+\.\d+(?:-beta\.\d+)?', version)
    channel = 'beta' if '-beta.' in version else 'stable'
    assert manifest['version'] == version and manifest['source_commit'] == commit
    assert commit.encode() in payload['bin/helix-screen'] and version.encode() in payload['bin/helix-screen']
    assert archive.stat().st_size < 95 * 1024**2
    assert 'assets/updates/repair-vorondeck-updater.py' in payload
    russian = {entry.get('tag'): entry.get('ru') for entry in
               ET.fromstring(payload['ui_xml/translations/ru.xml']).iter('translation')}
    for key in ('This release is installed', 'The installed version is newer',
                'No newer test release', 'Update manager', 'Stage {} of 3',
                'Waiting for the background check to finish...',
                'Another component is being updated. Wait for it to finish, then try again.'):
        assert russian.get(key) and russian[key] != key, 'Missing packaged Russian status: ' + key
    assert 'ui_xml/vorondeck_update_progress_modal.xml' in payload
    assert b'vorondeck_update_progress' in payload['assets/updates/manager/scripts/moonraker-vorondeck.py']
    for name in ('scripts/moonraker-vorondeck.py', 'scripts/moonraker-vorondeck-runtime.py',
                 'scripts/install-vorondeck.py', 'scripts/setup-vorondeck-updates.py',
                 'assets/updates/check-vorondeck-update.py',
                 'assets/updates/vorondeck_public_releases.py'):
        assert payload['assets/updates/manager/' + name] == (ROOT / name).read_bytes()
    settings = json.loads(payload['config/settings.json.template'])
    assert settings['wizard_completed'] is False
    assert settings['network']['wifi_ssid'] == settings['network']['eth_ip'] == ''
    assert 'panel_widgets' not in settings['printers']['default']
    assert 'config/settings.json' not in payload
    assert not any(name.startswith(('assets/presentation/', '.ssh/', '.dev-vm/')) for name in payload)
    package = dict(architecture='aarch64', libc='glibc', path='packages/' + archive.name,
                   sha256=record['sha256'], size=archive.stat().st_size, source_commit=commit,
                   binary_sha256=hashlib.sha256(payload['bin/helix-screen']).hexdigest())
    index = dict(schema=1, project='VoronDeck', channel=channel, version=version, packages=[package])
    with tempfile.TemporaryDirectory() as directory:
        legacy = Path(directory)
        fixture = ROOT / '.github/actions/fixtures/legacy-updater.tar.gz'
        assert hashlib.sha256(fixture.read_bytes()).hexdigest() == 'c36abcc219020961f95c882a68ab985742707261a4da41231dad3fe9cbdcfdf9'
        expected = {'scripts/install-vorondeck.py', 'scripts/moonraker-vorondeck-runtime.py', 'assets/updates/check-vorondeck-update.py', 'scripts/vorondeck-bootstrap.py', 'scripts/vorondeck_distribution.py'}
        with tarfile.open(fixture) as snapshot:
            assert {entry.name for entry in snapshot} == expected
            for entry in snapshot:
                assert entry.isfile() and entry.name in expected
                target = legacy / entry.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(snapshot.extractfile(entry).read())
        runtime = load('legacy_runtime', legacy / 'scripts/moonraker-vorondeck-runtime.py')
        runtime.installer.host_arch = lambda: 'aarch64'
        runtime.bootstrap.libc_name = lambda: 'glibc'
        @contextmanager
        def release_index(requested_channel):
            assert requested_channel == channel
            yield [], {}, commit, index
        runtime.checker.release_index = release_index
        assert runtime.remote_release(channel)['version'] == version
        old_manifest, _ = runtime.installer.verify_package(archive, package['sha256'], 'aarch64')
        assert old_manifest == manifest
        installed = dict(schema=1, project='VoronDeck', architecture='aarch64',
                         source_commit=LEGACY, version='1.0.0-beta.1')
        offer = runtime.checker.compare_release(installed, '1.0.0-beta.1-6c6d6cf8388f', index, 'glibc', channel)
        assert offer['status'] == 'available' and offer['available_version'] == version
    (OUT / 'release.json').write_text(json.dumps(index, indent=2) + '\n')
    (OUT / 'SHA256SUMS').write_text(package['sha256'] + '  ' + package['path'] + '\n')
    (OUT / 'verified.json').write_text(json.dumps(dict(package, version=version, legacy_compatible=True), indent=2))
    print('Verified release package and legacy update offer:', version, package['sha256'])


if __name__ == '__main__':
    main()
