#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Interactive installer entry point; private beta packages use the clone's SSH key."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('vorondeck_installer', ROOT / 'scripts/install-vorondeck.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)
REMOTE = 'git@github.com:Redaktor-clok/VoronDeck-beta.git'
PACKAGE_REF = 'refs/heads/beta-packages'
CHANNELS = {
    'beta': (REMOTE, PACKAGE_REF),
    'stable': ('https://github.com/Redaktor-clok/VoronDeck.git', 'refs/heads/stable-packages'),
}


def libc_name():
    try:
        if os.confstr('CS_GNU_LIBC_VERSION').startswith('glibc '):
            return 'glibc'
    except (ValueError, OSError, AttributeError):
        pass
    if list(Path('/lib').glob('ld-musl-*.so.1')):
        return 'musl'
    raise ValueError('Cannot identify the installed C library')


def select_package(index, arch, libc):
    if not isinstance(index, dict) or index.get('schema') != 1 or index.get('project') != 'VoronDeck':
        raise ValueError('Invalid release index')
    packages = index.get('packages')
    if not isinstance(packages, list) or len(packages) > 12:
        raise ValueError('Invalid package list')
    matches = [p for p in packages if isinstance(p, dict) and p.get('architecture') == arch
               and p.get('libc') == libc]
    if len(matches) != 1:
        raise ValueError('No unique published package for ' + arch + '/' + libc + '; nothing installed')
    package = matches[0]
    path = PurePosixPath(package.get('path', ''))
    if (len(path.parts) != 2 or path.parts[0] != 'packages' or
            not re.fullmatch(r'[A-Za-z0-9_.-]+\.tar\.gz', path.name)
            or str(path) != package['path'] or
            not re.fullmatch(r'[a-f0-9]{64}', package.get('sha256', '')) or
            type(package.get('size')) is not int or not 0 < package['size'] < 95 * 1024**2):
        raise ValueError('Invalid package metadata')
    return package


def git_output(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args], timeout=180)


def fetch_package(folder, arch, libc, channel='beta'):
    remote, package_ref = CHANNELS[channel]
    # Explicit project remote: never inherit upstream HelixScreen origin accidentally.
    subprocess.run(['git', '-C', str(ROOT), 'fetch', '--no-tags', remote, package_ref],
                   check=True, timeout=1800)  # Printer Wi-Fi can take minutes for a binary package.
    commit = git_output('rev-parse', 'FETCH_HEAD').decode().strip()
    if not re.fullmatch(r'[a-f0-9]{40,64}', commit):
        raise ValueError('Invalid fetched commit')
    if int(git_output('cat-file', '-s', commit + ':release.json')) > 65536:
        raise ValueError('Release index too large')
    index = json.loads(git_output('show', commit + ':release.json'))
    entry = select_package(index, arch, libc)
    blob = commit + ':' + entry['path']
    if int(git_output('cat-file', '-s', blob)) != entry['size']:
        raise ValueError('Package size mismatch')
    package = folder / 'vorondeck.tar.gz'
    with package.open('wb') as output:
        subprocess.run(['git', '-C', str(ROOT), 'show', blob], stdout=output,
                       check=True, timeout=180)
    if hashlib.sha256(package.read_bytes()).hexdigest() != entry['sha256']:
        raise ValueError('Package SHA256 mismatch')
    return package, entry['sha256']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, help='Offline package; requires --sha256')
    parser.add_argument('--sha256')
    parser.add_argument('--check', action='store_true', help='Verify only; no installation')
    parser.add_argument('--config-dir', type=Path)
    parser.add_argument('--moonraker-port', type=int, default=7125)
    parser.add_argument('--channel', choices=('stable', 'beta'),
                        help='Defaults to the channel of this source checkout')
    args = parser.parse_args()
    if sys.platform != 'linux' or os.geteuid() == 0:
        parser.error('Run on Linux as the printer user, not root')
    if bool(args.package) != bool(args.sha256):
        parser.error('Offline installation requires both --package and --sha256')
    if not 1 <= args.moonraker_port <= 65535:
        parser.error('Invalid Moonraker port')
    channel = args.channel or ('beta' if '-beta.' in (ROOT / 'VERSION.txt').read_text() else 'stable')
    installer.Services.detect()
    arch, libc = installer.host_arch(), libc_name()
    print('VoronDeck — установка:', arch, '/', libc, flush=True)
    installer.check_printer(args.moonraker_port)
    with tempfile.TemporaryDirectory(prefix='vorondeck-package-') as temporary:
        package, digest = ((args.package, args.sha256) if args.package else
                           fetch_package(Path(temporary), arch, libc, channel))
        manifest, _ = installer.verify_package(package, digest, arch)
        package_channel = 'beta' if '-beta.' in manifest['version'] else 'stable'
        if (args.channel or not args.package) and package_channel != channel:
            raise ValueError('Published package does not match the requested channel')
        command = [sys.executable, str(ROOT / 'scripts/install-vorondeck.py'),
                   '--package', str(package), '--sha256', digest,
                   '--moonraker-port', str(args.moonraker_port), '--channel', package_channel]
        if args.config_dir:
            command += ['--config-dir', str(args.config_dir)]
        if not args.check:
            print('1. Установить и включить автозапуск VoronDeck (прежний экран отключится).')
            print('2. Запустить для проверки до перезагрузки.')
            print('3. Подготовить файлы, оставить прежний экран работающим.')
            print('0. Отмена. Файлы KlipperScreen сохраняются для отката.')
            choice = input('Выберите [1/2/3/0]: ').strip()
            if choice not in ('1', '2', '3'):
                return
            command += ['--install', '--install-deps']
            if choice in ('1', '2'):
                command += ['--activate']
            if choice == '1':
                command += ['--autostart']
        subprocess.run(command, check=True)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        print('Установка остановлена: ' + str(error), file=sys.stderr)
        sys.exit(1)
