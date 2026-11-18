#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify, stage and activate a VoronDeck package on Linux (systemd or OpenRC)."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
import sys
from urllib.request import urlopen
from vorondeck_distribution import elf_arch

SERVICE = 'vorondeck.service'
UNIT_MARKER = '# Managed by VoronDeck beta installer'


def host_arch():
    # Kernel architecture may be 64-bit while the installed userland is 32-bit.
    with open('/proc/self/exe', 'rb') as executable:
        data = executable.read(64)
    return elf_arch(data)


def verify_package(package, expected_sha, machine):
    if not re.fullmatch(r'[a-f0-9]{64}', expected_sha):
        raise ValueError('Expected a SHA256 from the authenticated release source')
    digest = hashlib.sha256()
    with package.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    if digest.hexdigest() != expected_sha:
        raise ValueError('Package SHA256 mismatch')
    payload, total = {}, 0
    with tarfile.open(package, 'r:gz') as archive:
        for entry in archive:
            name = PurePosixPath(entry.name)
            if (not name.parts or not entry.isfile() or name.is_absolute() or '..' in name.parts or
                    '\\' in entry.name or str(name) != entry.name or entry.name in payload):
                raise ValueError('Unsafe or duplicate archive entry')
            allowed = (name.parts[0] in ('assets', 'ui_xml') or entry.name in (
                'bin/helix-screen', 'bin/libhelix-bluetooth.so', 'config/settings.json.template', 'LICENSE',
                'COPYRIGHT', 'docs/VORONDECK-CREDITS.md', 'vorondeck-release.json'))
            if not allowed or entry.size > 100 * 1024**2:
                raise ValueError('Unexpected package entry: ' + entry.name)
            total += entry.size
            if total > 2 * 1024**3:
                raise ValueError('Package exceeds the unpacked size limit')
            payload[entry.name] = archive.extractfile(entry).read()
    manifest = json.loads(payload.pop('vorondeck-release.json'))
    if (not isinstance(manifest, dict) or manifest.get('schema') != 1 or manifest.get('project') != 'VoronDeck' or
            manifest.get('architecture') != machine or not re.fullmatch(
                r'[0-9]+\.[0-9]+\.[0-9]+(?:-beta\.[0-9]+)?', manifest.get('version', ''))):
        raise ValueError('Incompatible release manifest')
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in payload.items()}
    if hashes != manifest.get('files'):
        raise ValueError('Package contents do not match their manifest')
    executable = payload['bin/helix-screen']
    if elf_arch(executable) != machine:
        raise ValueError('Executable architecture mismatch')
    if 'bin/libhelix-bluetooth.so' in payload and elf_arch(payload['bin/libhelix-bluetooth.so']) != machine:
        raise ValueError('Bluetooth plugin architecture mismatch')
    template = json.loads(payload['config/settings.json.template'])
    if not isinstance(template, dict) or type(template.get('config_version')) is not int or template['config_version'] < 1:
        raise ValueError('Versioned settings template required for an isolated first run')
    return manifest, payload


def assert_idle(status):
    if status.get('print_stats', {}).get('state') not in ('standby', 'complete', 'cancelled'):
        raise ValueError('Printer must be idle; printing, paused and unknown states are refused')
    if status.get('idle_timeout', {}).get('state') != 'Idle':
        raise ValueError('Wait until the printer is idle before installing')
    heaters = status.get('heaters', {}).get('available_heaters')
    if not isinstance(heaters, list):
        raise ValueError('Cannot verify heater state')
    for heater in heaters:
        target = status.get(heater, {}).get('target')
        if not isinstance(target, (int, float)) or isinstance(target, bool) or target != 0:
            raise ValueError('All heater targets must be zero before installing')


def check_printer(port=7125):
    # Local Moonraker only. No credentials, commands, heating or motion.
    def request(path):
        with urlopen('http://127.0.0.1:' + str(port) + path, timeout=5) as response:
            data = response.read(1024 * 1024 + 1)
        if len(data) > 1024 * 1024:
            raise ValueError('Oversized Moonraker response')
        return json.loads(data)['result']
    if request('/server/info').get('klippy_state') != 'ready':
        raise ValueError('Klipper is not ready')
    from urllib.parse import quote
    base = request('/printer/objects/query?heaters')['status']['heaters']['available_heaters']
    query = '&'.join(quote(name, safe='') for name in ['print_stats', 'idle_timeout', 'heaters', *base])
    assert_idle(request('/printer/objects/query?' + query)['status'])


def local_vt_available():
    return all(Path(path).is_char_device() for path in ('/dev/tty0', '/dev/tty1'))


def check_console_session():
    # Only the idle login prompt or a screen we already manage may own tty1.
    allowed_pids = set()
    for name in ('vorondeck', 'KlipperScreen', 'helixscreen'):
        result = subprocess.run(['systemctl', 'show', name + '.service', '--property=MainPID',
                                 '--value'], capture_output=True, text=True, timeout=10)
        if result.returncode == 0 and result.stdout.strip().isdigit():
            allowed_pids.add(int(result.stdout.strip()))
    result = subprocess.run(['ps', '-t', 'tty1', '-o', 'pid=,uid=,comm='],
                            capture_output=True, text=True, timeout=10)
    # procps returns 1 with no output when no process has this terminal.
    if result.returncode == 1 and not result.stdout.strip() and not result.stderr.strip():
        return
    if result.returncode != 0:
        raise ValueError('Cannot verify tty1 login state; no console takeover allowed')
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 3 or not fields[0].isdigit() or not fields[1].isdigit():
            raise ValueError('Cannot verify tty1 process identity')
        pid, uid, command = int(fields[0]), int(fields[1]), fields[2]
        if pid not in allowed_pids and not (uid == 0 and command in ('agetty', 'getty')):
            raise ValueError('tty1 has an active local session; log out before switching screens')


def unit_text(user, current, config, groups, port=7125, *, console=False):
    for value in (user, str(current), str(config), *groups):
        if not re.fullmatch(r'[A-Za-z0-9_./-]+', value):
            raise ValueError('Unsupported service path or user name')
    console_order = '\nConflicts=getty@tty1.service\nAfter=getty@tty1.service' if console else ''
    console_io = ('StandardInput=tty\nTTYPath=/dev/tty1\nTTYReset=yes\nTTYVTDisallocate=yes\n'
                  if console else '')
    return f'''{UNIT_MARKER}
[Unit]
Description=VoronDeck touchscreen
After=network.target plymouth-quit-wait.service{console_order}
[Service]
Type=simple
User={user}
WorkingDirectory={current}
Environment=HELIX_CONFIG_DIR={config}
Environment=HELIX_HOT_RELOAD=0
Environment=HELIX_DISABLE_AUTO_UPDATES=1
Environment=HELIX_DIAGNOSTIC_UPLOADS=0
ExecStart={current}/bin/helix-screen --moonraker ws://127.0.0.1:{port} -v
Restart=always
RestartSec=5
Nice=10
NoNewPrivileges=true
SupplementaryGroups={' '.join(groups)}
AmbientCapabilities=CAP_SYS_TTY_CONFIG
{console_io}StandardOutput=journal
StandardError=journal
[Install]
WantedBy=multi-user.target
'''


def run(*args):
    subprocess.run(args, check=True, timeout=90)


class Services:
    """The two supported init systems; no distro-name assumptions."""
    def __init__(self, kind):
        if kind not in ('systemd', 'openrc'):
            raise ValueError('Unsupported init system: ' + kind)
        self.kind = kind
        self.unit = Path('/etc/systemd/system/vorondeck.service' if kind == 'systemd'
                         else '/etc/init.d/vorondeck')

    @classmethod
    def detect(cls):
        if Path('/run/systemd/system').is_dir() and shutil.which('systemctl'):
            return cls('systemd')
        if (Path('/run/openrc').is_dir() and Path('/sbin/openrc-run').is_file()
                and shutil.which('rc-service') and shutil.which('rc-update')):
            return cls('openrc')
        raise ValueError('Supported init systems: systemd and OpenRC; no services changed')

    def active(self, name):
        command = (['systemctl', 'is-active', '--quiet', name + '.service'] if self.kind == 'systemd'
                   else ['rc-service', name, 'status'])
        return subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=15).returncode == 0

    def boot_state(self, name):
        if self.kind == 'systemd':
            result = subprocess.run(['systemctl', 'is-enabled', name + '.service'],
                                    capture_output=True, text=True, timeout=15)
            state = result.stdout.strip()
            if state not in ('enabled', 'disabled', 'not-found', 'static', 'masked'):
                raise ValueError('Unsupported service enable state: ' + name + ': ' + state)
            return state
        return sorted(p.parent.name for p in Path('/etc/runlevels').glob('*/' + name))

    def action(self, action, name):
        command = (['systemctl', action, name + '.service'] if self.kind == 'systemd'
                   else ['rc-service', name, action])
        run('sudo', *command)

    def reload(self):
        if self.kind == 'systemd':
            run('sudo', 'systemctl', 'daemon-reload')

    def set_boot(self, name, state):
        if self.kind == 'systemd':
            current = self.boot_state(name)
            if current == state or state == 'disabled' and current in ('not-found', 'static', 'masked'):
                return
            if current != state:
                run('sudo', 'systemctl', 'enable' if state == 'enabled' else 'disable', name + '.service')
        else:
            current = self.boot_state(name)
            for level in set(current) - set(state):
                run('sudo', 'rc-update', 'del', name, level)
            for level in set(state) - set(current):
                run('sudo', 'rc-update', 'add', name, level)

    def install(self, source):
        run('sudo', 'install', '-m', '644' if self.kind == 'systemd' else '755',
            str(source), str(self.unit))
        self.reload()

    def remove(self):
        if self.unit.exists():
            if self.unit.is_symlink() or UNIT_MARKER not in self.unit.read_text():
                raise ValueError('Refusing to remove a service not managed by VoronDeck')
            run('sudo', 'rm', '--', str(self.unit))
            self.reload()

    def text(self, user, current, config, groups, port):
        # Apply the same validation before interpolating OpenRC shell literals.
        systemd = unit_text(user, current, config, groups, port,
                            console=self.kind == 'systemd' and local_vt_available())
        if self.kind == 'systemd':
            return systemd
        return f'''#!/sbin/openrc-run
{UNIT_MARKER}
description="VoronDeck touchscreen"
command="{current}/bin/helix-screen"
command_args="--moonraker ws://127.0.0.1:{port} -v"
command_user="{user}"
directory="{current}"
command_background=true
pidfile="/run/vorondeck.pid"
export HELIX_CONFIG_DIR="{config}"
export HELIX_HOT_RELOAD=0
export HELIX_DISABLE_AUTO_UPDATES=1
export HELIX_DIAGNOSTIC_UPLOADS=0
depend() {{
    after net
}}
'''


def atomic_write(path, data):
    """Replace only our file, preserving the previous file until fsync completes."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.vorondeck-', delete=False) as stream:
        temp = Path(stream.name)
        try:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            temp.unlink(missing_ok=True)
            raise
    try:
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def replace_link(current, target):
    temporary = current.with_name('.current-' + str(os.getpid()))
    if temporary.exists() or temporary.is_symlink():
        raise ValueError('Unfinished link operation: ' + str(temporary))
    try:
        temporary.symlink_to(target, target_is_directory=True)
        os.replace(temporary, current)
    finally:
        temporary.unlink(missing_ok=True)


def safe_child(path, home):
    path = Path(os.path.abspath(path))
    if not path.is_relative_to(home) or path == home:
        raise ValueError('Install/config path must remain inside the printer user home')
    for component in (path, *path.parents):
        if component == home:
            break
        if component.is_symlink():
            raise ValueError('Linked install/config paths are not supported: ' + str(component))
    return path


def dependency_packages(sonames, manager):
    # Package names vary; unknown libraries must not be guessed or downloaded individually.
    packages = {
        'apt-get': {'libstdc++.so.6': 'libstdc++6', 'libgcc_s.so.1': 'libgcc-s1',
                    'libatomic.so.1': 'libatomic1', 'libdrm.so.2': 'libdrm2',
                    'libinput.so.10': 'libinput10', 'libgbm.so.1': 'libgbm1',
                    'libEGL.so.1': 'libegl1', 'libGLESv2.so.2': 'libgles2',
                    'libevdev.so.2': 'libevdev2'},
        'dnf': {'libstdc++.so.6': 'libstdc++', 'libgcc_s.so.1': 'libgcc',
                'libatomic.so.1': 'libatomic', 'libdrm.so.2': 'libdrm',
                'libinput.so.10': 'libinput', 'libgbm.so.1': 'mesa-libgbm',
                'libEGL.so.1': 'libglvnd-egl', 'libGLESv2.so.2': 'libglvnd-gles',
                'libevdev.so.2': 'libevdev'},
        'pacman': {'libstdc++.so.6': 'gcc-libs', 'libgcc_s.so.1': 'gcc-libs',
                   'libatomic.so.1': 'gcc-libs', 'libdrm.so.2': 'libdrm',
                   'libinput.so.10': 'libinput', 'libgbm.so.1': 'mesa',
                   'libEGL.so.1': 'libglvnd', 'libGLESv2.so.2': 'libglvnd',
                   'libevdev.so.2': 'libevdev'},
        'apk': {'libstdc++.so.6': 'libstdc++', 'libgcc_s.so.1': 'libgcc',
                'libatomic.so.1': 'libatomic', 'libdrm.so.2': 'libdrm',
                'libinput.so.10': 'libinput', 'libgbm.so.1': 'mesa-gbm',
                'libEGL.so.1': 'mesa-egl', 'libGLESv2.so.2': 'mesa-gles',
                'libevdev.so.2': 'libevdev'},
        'zypper': {'libstdc++.so.6': 'libstdc++6', 'libgcc_s.so.1': 'libgcc_s1',
                   'libatomic.so.1': 'libatomic1', 'libdrm.so.2': 'libdrm2',
                   'libinput.so.10': 'libinput10', 'libgbm.so.1': 'libgbm1',
                   'libEGL.so.1': 'libEGL1', 'libGLESv2.so.2': 'libGLESv2-2',
                   'libevdev.so.2': 'libevdev2'},
    }
    mapping = packages.get(manager, {})
    if not sonames or any(name not in mapping for name in sonames):
        raise ValueError('Install packages providing these libraries through your OS: ' + ', '.join(sonames))
    return sorted({mapping[name] for name in sonames})


def check_dependencies(binary, allow_install=False):
    if not shutil.which('ldd'):
        raise ValueError('Install the system ldd utility before continuing')
    def inspect():
        return subprocess.run(['ldd', str(binary)], capture_output=True, text=True,
                              timeout=30, env={**os.environ, 'LC_ALL': 'C'})
    result = inspect()
    output = result.stdout + result.stderr
    missing = re.findall(r'^\s*(\S+)\s+=>\s+not found\s*$', output, re.MULTILINE)
    if missing and allow_install:
        manager = next((m for m in ('apt-get', 'dnf', 'pacman', 'apk', 'zypper') if shutil.which(m)), None)
        packages = dependency_packages(missing, manager)
        options = {'apt-get': ['install', '-y', '--no-install-recommends'], 'dnf': ['install', '-y'],
                   'pacman': ['-S', '--needed', '--noconfirm'], 'apk': ['add'],
                   'zypper': ['--non-interactive', 'install']}
        print('Required system packages:', manager, ', '.join(packages))
        if input('Install these packages from the configured OS repositories? [y/N] ').lower() != 'y':
            raise ValueError('Required runtime libraries are missing')
        if manager == 'apt-get':
            subprocess.run(['sudo', 'apt-get', 'update'], check=True)
        subprocess.run(['sudo', manager, *options[manager], *packages], check=True)
        result = inspect()
        output = result.stdout + result.stderr
    if result.returncode or 'not found' in output:
        raise ValueError('Incompatible libc or missing runtime libraries:\n' + output)


def stage_release(destination, manifest, payload, allow_dependencies=False):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_dir():
            raise ValueError('Staged release must be a real directory')
        for name, data in payload.items():
            path = safe_child(destination / name, destination)
            if not path.is_file() or path.read_bytes() != data:
                raise ValueError('Existing staged release differs from the verified package')
        check_dependencies(destination / 'bin/helix-screen', allow_dependencies)
        return
    required = sum(map(len, payload.values())) + 64 * 1024**2
    if shutil.disk_usage(destination.parent).free < required:
        raise ValueError('Insufficient free space to stage the release')
    with tempfile.TemporaryDirectory(prefix='.stage-', dir=destination.parent) as temporary:
        stage = Path(temporary) / 'release'
        stage.mkdir()
        for name, data in payload.items():
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o755 if name == 'bin/helix-screen' else 0o644)
        (stage / 'vorondeck-release.json').write_text(json.dumps(manifest, indent=2))
        # Only a package authenticated by the caller's SHA256 reaches the loader.
        check_dependencies(stage / 'bin/helix-screen', allow_dependencies)
        # A single rename publishes a complete release, never a partial directory.
        os.rename(stage, destination)


def restore_switch(backup, services, base, config):
    state = json.loads((backup / 'switch.json').read_text())
    if state['init'] != services.kind or state['config'] != str(config):
        raise ValueError('Backup belongs to a different init system or config path')
    errors = []

    def attempt(label, operation):
        try:
            operation()
        except Exception as error:
            errors.append(label + ': ' + str(error))

    attempt('stop VoronDeck', lambda: services.action('stop', 'vorondeck')
            if services.active('vorondeck') else None)
    if services.active('vorondeck'):
        raise RuntimeError('VoronDeck could not be stopped; rollback must be retried. Backup: ' + str(backup))
    current = base / 'current'
    if state['previous_target'] is None:
        attempt('restore release link', lambda: current.unlink(missing_ok=True))
    else:
        attempt('restore release link', lambda: replace_link(current, state['previous_target']))
    settings = config / 'settings.json'
    if state['had_settings']:
        attempt('restore settings', lambda: atomic_write(settings, (backup / 'settings.json').read_bytes()))
    else:
        attempt('remove new settings', lambda: settings.unlink(missing_ok=True))
    # Restore boot state while the new service still exists, then restore its definition.
    attempt('restore VoronDeck autostart', lambda: services.set_boot('vorondeck', state['boot']['vorondeck']))
    attempt('restore service', lambda: services.install(backup / 'previous.service')
            if state['had_unit'] else services.remove())
    if state.get('getty_active') and not services.active('getty@tty1'):
        attempt('restore tty1 login', lambda: services.action('start', 'getty@tty1'))
    for name in ('KlipperScreen', 'helixscreen'):
        attempt('restore autostart ' + name, lambda n=name: services.set_boot(n, state['boot'][n]))
        if state['active'][name]:
            attempt('restart ' + name, lambda n=name: services.action('start', n))
    if state['active']['vorondeck']:
        attempt('restart previous VoronDeck', lambda: services.action('start', 'vorondeck'))
    if errors:
        raise RuntimeError('Rollback incomplete. Backup: ' + str(backup) + '\n' + '\n'.join(errors))


def activate_release(services, base, config, destination, payload, service, *, fresh=False,
                     autostart=False, idle_check=check_printer):
    idle_check()
    current, settings = base / 'current', config / 'settings.json'
    if current.exists() and not current.is_symlink():
        raise ValueError('Current release is not a managed symlink')
    if settings.is_symlink():
        raise ValueError('Settings must not be a symlink')
    if services.unit.is_symlink() or (services.unit.exists() and UNIT_MARKER not in services.unit.read_text()):
        raise ValueError('Existing service is not managed by this installer')
    console_claim = (services.kind == 'systemd' and
                     'Conflicts=getty@tty1.service' in service.splitlines())
    if console_claim:
        if not local_vt_available():
            raise ValueError('Local virtual terminal is unavailable; regenerate the service')
        check_console_session()
    state = {'init': services.kind, 'config': str(config),
             'previous_target': os.readlink(current) if current.is_symlink() else None,
             'had_settings': settings.exists(), 'had_unit': services.unit.exists(),
             'active': {n: services.active(n) for n in ('vorondeck', 'KlipperScreen', 'helixscreen')},
             'boot': {n: services.boot_state(n) for n in ('vorondeck', 'KlipperScreen', 'helixscreen')}}
    if console_claim:
        state['getty_active'] = services.active('getty@tty1')
    backup = base / 'backups' / datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    backup.mkdir(parents=True, mode=0o700)
    if state['had_settings']:
        shutil.copyfile(settings, backup / 'settings.json')
    if state['had_unit']:
        shutil.copyfile(services.unit, backup / 'previous.service')
    atomic_write(backup / 'switch.json', json.dumps(state, indent=2).encode())
    atomic_write(backup / 'new.service', service.encode())
    try:
        # Includes failures halfway through stopping competing screens.
        for name, active in state['active'].items():
            if active:
                services.action('stop', name)
                if services.active(name):
                    raise RuntimeError('Service did not stop: ' + name)
        if fresh or not state['had_settings']:
            defaults = json.loads(payload['config/settings.json.template'])
            defaults.update(wizard_completed=False, language='ru')
            atomic_write(settings, json.dumps(defaults, indent=2).encode())
        replace_link(current, destination)
        services.install(backup / 'new.service')
        if console_claim:
            check_console_session()
        services.action('start', 'vorondeck')
        for _ in range(5):
            time.sleep(1)
            if not services.active('vorondeck'):
                raise RuntimeError('VoronDeck exited during its startup check')
        if autostart:
            for name in ('KlipperScreen', 'helixscreen'):
                services.set_boot(name, 'disabled' if services.kind == 'systemd' else [])
            services.set_boot('vorondeck', 'enabled' if services.kind == 'systemd' else ['default'])
    except BaseException as original:
        try:
            restore_switch(backup, services, base, config)
        except Exception as rollback_error:
            raise RuntimeError(str(original) + '\n' + str(rollback_error)) from original
        raise
    return backup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path)
    parser.add_argument('--sha256')
    parser.add_argument('--install', action='store_true', help='Stage a verified release; default is validation only')
    parser.add_argument('--activate', action='store_true', help='Switch screens after confirmation')
    parser.add_argument('--autostart', action='store_true', help='Also switch boot services; requires --activate')
    parser.add_argument('--fresh', action='store_true', help='Back up VoronDeck settings and restart its wizard')
    parser.add_argument('--install-deps', action='store_true', help='Offer to install missing OS libraries')
    parser.add_argument('--config-dir', type=Path, help='VoronDeck config inside the current user home')
    parser.add_argument('--moonraker-port', type=int, default=7125)
    parser.add_argument('--channel', choices=('stable', 'beta'), help='Update channel; defaults to the verified package version')
    parser.add_argument('--rollback', type=Path, help='Restore a previous switch backup after confirmation')
    args = parser.parse_args()
    if sys.version_info < (3, 9):
        parser.error('Python 3.9 or newer is required')
    if platform.system() != 'Linux':
        parser.error('Run this installer on the Linux printer host')
    if os.geteuid() == 0:
        parser.error('Run as the printer user, not root; sudo is used only for service management')
    if not 1 <= args.moonraker_port <= 65535:
        parser.error('Invalid Moonraker port')
    if args.activate and not args.install or args.autostart and not args.activate or args.fresh and not args.activate:
        parser.error('--activate requires --install; --fresh and --autostart require --activate')
    if args.rollback and (args.package or args.install or args.activate or args.fresh or args.autostart):
        parser.error('--rollback cannot be combined with installation')
    import grp
    import pwd
    import fcntl
    home = Path.home().resolve()
    base = safe_child(home / '.local/share/vorondeck', home)
    config = safe_child(args.config_dir or home / 'printer_data/config/VoronDeck', home)
    services = Services.detect()
    machine = host_arch()
    print('Host:', machine, 'init:', services.kind, 'config:', config)
    idle_check = lambda: check_printer(args.moonraker_port)
    if not args.rollback:
        if not args.package or not args.sha256:
            parser.error('--package and --sha256 are required')
        manifest, payload = verify_package(args.package, args.sha256, machine)
        package_channel = 'beta' if '-beta.' in manifest['version'] else 'stable'
        if args.channel and args.channel != package_channel:
            parser.error('Requested update channel does not match the verified package')
        idle_check()
        print('Package verified; printer is idle')
        if not args.install:
            return
    # Hold a per-user lock through staging, switching and rollback.
    base.mkdir(parents=True, exist_ok=True)
    lock = safe_child(base / 'installer.lock', home)
    with lock.open('a') as guard:
        try:
            fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error('Another VoronDeck installer is running')
        if args.rollback:
            backup = safe_child(args.rollback.absolute(), home)
            if not backup.is_relative_to(base / 'backups') or not (backup / 'switch.json').is_file():
                parser.error('Choose a backup created by this installer')
            idle_check()
            if input('Restore screen services and settings from this backup? [y/N] ').lower() != 'y':
                return
            run('sudo', '-v')
            restore_switch(backup, services, base, config)
            print('Previous screen restored')
            return
        destination = safe_child(base / 'releases' / (manifest['version'] + '-' + args.sha256[:12]), home)
        stage_release(destination, manifest, payload, args.install_deps)
        print('Package staged:', destination)
        if not args.activate:
            print('Screen unchanged. Add --activate to switch after confirmation.')
            return
        user = pwd.getpwuid(os.getuid()).pw_name
        known = {group.gr_name: group.gr_gid for group in grp.getgrall()}
        groups = [name for name in ('video', 'input', 'render', 'tty', 'audio') if name in known]
        if services.kind == 'openrc':
            missing = [name for name in groups if known[name] not in os.getgrouplist(user, os.getgid())]
            if missing:
                parser.error('OpenRC requires device group membership first: ' + ', '.join(missing))
        service = services.text(user, base / 'current', config, groups, args.moonraker_port)
        active = [n for n in ('KlipperScreen', 'helixscreen', 'vorondeck') if services.active(n)]
        print('Active screens:', ', '.join(active) or 'none')
        print('Existing screen files are kept. Boot services change only with --autostart.')
        if input('Switch to VoronDeck now? [y/N] ').lower() != 'y':
            return
        run('sudo', '-v')
        backup = activate_release(services, base, config, destination, payload, service,
                                  fresh=args.fresh, autostart=args.autostart, idle_check=idle_check)
        print('VoronDeck service stayed active for 5 seconds. Check the physical display and touch.')
        print('Rollback backup:', backup)
        import importlib.util
        setup_spec = importlib.util.spec_from_file_location(
            'vorondeck_updates_setup', Path(__file__).with_name('setup-vorondeck-updates.py'))
        setup_module = importlib.util.module_from_spec(setup_spec)
        setup_spec.loader.exec_module(setup_module)
        try:
            updates_backup = setup_module.setup(Path(__file__).resolve().parents[1], base, config,
                services, idle_check, channel=package_channel, port=args.moonraker_port)
        except Exception as error:
            raise RuntimeError('VoronDeck is installed, but automatic updates setup failed. ' + str(error)) from error
        print('VoronDeck updates connected to Moonraker; channel:', package_channel)
        if updates_backup:
            print('Update configuration backup:', updates_backup)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        print('Installation stopped: ' + str(error), file=sys.stderr)
        sys.exit(1)
