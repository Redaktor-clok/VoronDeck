#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the authenticated beta package index. Never install or execute fetched files."""
from contextlib import contextmanager
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess

CHANNELS = {
    'beta': ('git@github.com:Redaktor-clok/VoronDeck-beta.git', 'beta-packages'),
    'stable': ('https://github.com/Redaktor-clok/VoronDeck.git', 'stable-packages'),
}
VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+(?:-(?:beta|T)\.[0-9]+)?')
SHA = re.compile(r'[a-f0-9]{64}')
COMMIT = re.compile(r'[a-f0-9]{40}')

_spec = importlib.util.spec_from_file_location('vorondeck_public_releases', Path(__file__).with_name('vorondeck_public_releases.py'))
public = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(public)


def ssh_command(home):
    args = ['ssh', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
            '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=15',
            '-o', 'ServerAliveCountMax=2']
    key = home / '.ssh/vorondeck_beta_readonly'
    hosts = home / '.ssh/known_hosts_vorondeck'
    # Reuse the installer's dedicated identity and pinned hosts, without reading
    # private key contents or executing an arbitrary repository core.sshCommand.
    if key.is_file() and hosts.is_file():
        args += ['-i', str(key), '-o', 'IdentitiesOnly=yes',
                 '-o', 'UserKnownHostsFile=' + str(hosts)]
    return shlex.join(args)


def run(args, env=None, timeout=30):
    # A timeout must also stop ssh (git's child), not leave an orphan transfer.
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          env=env, start_new_session=True) as process:
        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            raise
        if process.returncode:
            raise ValueError('Authenticated update check failed')
        return output


def compare_release(installed, release_name, index, libc, channel='beta'):
    if (not isinstance(installed, dict) or installed.get('project') != 'VoronDeck'
            or installed.get('schema') != 1 or not VERSION.fullmatch(installed.get('version', ''))
            or not COMMIT.fullmatch(installed.get('source_commit', ''))):
        raise ValueError('Invalid installed release metadata')
    if (not isinstance(index, dict) or index.get('schema') != 1 or index.get('project') != 'VoronDeck'
            or channel not in CHANNELS or index.get('channel') != channel):
        raise ValueError('Invalid release index')
    packages = index.get('packages')
    if not isinstance(packages, list) or len(packages) > 12:
        raise ValueError('Invalid package list')
    matches = [p for p in packages if isinstance(p, dict)
               and p.get('architecture') == installed.get('architecture') and p.get('libc') == libc]
    if len(matches) != 1:
        raise ValueError('No unique compatible package')
    package = matches[0]
    version = index.get('version', '')
    pattern = r'\d+\.\d+\.\d+-beta\.\d+' if channel == 'beta' else r'\d+\.\d+\.\d+'
    if (not re.fullmatch(pattern, version) or not SHA.fullmatch(package.get('sha256', ''))
            or not COMMIT.fullmatch(package.get('source_commit', ''))):
        raise ValueError('Invalid release metadata')
    current = (release_name == installed['version'] + '-' + package['sha256'][:12]
               and installed['source_commit'] == package['source_commit']
               and installed['version'] == version)
    return {'status': 'current' if current else 'available',
            'installed_version': installed['version'], 'available_version': version,
            'installed_build': installed['source_commit'][:8],
            'available_build': package['source_commit'][:8],
            'sha256': package['sha256'], 'source_commit': package['source_commit']}


@contextmanager
def release_index(channel):
    import fcntl
    if channel not in CHANNELS:
        raise ValueError('Unknown release channel')
    remote, branch = CHANNELS[channel]
    cache = Path.home() / ('.cache/vorondeck/update-' + channel + '.git')
    cache.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    env = dict(os.environ, GIT_TERMINAL_PROMPT='0', GIT_SSH_COMMAND=ssh_command(Path.home()))
    lock_name = 'update-check.lock' if channel == 'beta' else 'update-stable.lock'
    with (cache.parent / lock_name).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if cache.is_symlink():
            raise ValueError('Linked update cache refused')
        if not cache.exists():
            run(['git', 'init', '--bare', str(cache)], env)
        git = ['git', '--git-dir', str(cache),
               '-c', 'remote.origin.url=' + remote,
               '-c', 'remote.origin.promisor=true',
               '-c', 'remote.origin.partialclonefilter=blob:none']
        # Fetch trees only; reading release.json lazily fetches its small blob.
        run(git + ['fetch', '--depth=1', '--filter=blob:none', '--no-tags',
                   '--no-write-fetch-head', 'origin',
                   '+refs/heads/' + branch + ':refs/vorondeck/check'], env, 90)
        commit = run(git + ['rev-parse', 'refs/vorondeck/check'], env).decode().strip()
        if not COMMIT.fullmatch(commit):
            raise ValueError('Invalid release index commit')
        blob = commit + ':release.json'
        size = int(run(git + ['cat-file', '-s', blob], env))
        if not 0 < size <= 65536:
            raise ValueError('Release index size invalid')
        index = json.loads(run(git + ['show', blob], env))
        yield git, env, commit, index


def fetch_index(channel):
    with release_index(channel) as (_, _, _, index):
        return index


def check_channels(installed, release_name, libc, has_beta_key, fetch=None, selected_channel=None):
    fetch = fetch or fetch_index
    if selected_channel is not None and selected_channel not in CHANNELS:
        raise ValueError('Unknown release channel')
    active = selected_channel or ('beta' if '-beta.' in installed.get('version', '') else 'stable')
    channels, results = {}, {}
    for channel in ('stable', 'beta'):
        if channel == 'beta' and not has_beta_key:
            result = {'status': 'access_required'}
        else:
            try:
                result = compare_release(installed, release_name, fetch(channel), libc, channel)
            except (OSError, ValueError, TypeError, subprocess.SubprocessError):
                result = {'status': 'unavailable' if channel == 'stable' else 'error'}
        results[channel] = result
        channels[channel] = {'status': result['status'],
                             'version': result.get('available_version', ''),
                             'build': result.get('available_build', '')}
    result = dict(results[active], active_channel=active, channels=channels)
    result['installed_version'] = installed['version']
    result['notification_id'] = notification_id(result)
    result['offers'] = []
    for channel, release in results.items():
        if notification_id(dict(release, active_channel=channel)):
            result['offers'].append({'kind': channel, 'version': release['available_version'],
                                    'display_version': release['available_version'], 'tag': '',
                                    'sha256': release['sha256'], 'source_commit': release['source_commit'],
                                    'notification_id': channel + ':' + release['available_version'] + ':' + release['sha256']})
    return result


def notification_id(result):
    """Only the authorized active channel may announce a non-older package."""
    channel = result.get('active_channel')
    installed = result.get('installed_version', '')
    available = result.get('available_version', '')
    build = result.get('available_build', '')
    if (result.get('status') != 'available' or channel not in CHANNELS
            or not VERSION.fullmatch(installed) or not VERSION.fullmatch(available)
            or not re.fullmatch(r'[a-f0-9]{8}', build)
            or ('-beta.' in available) != (channel == 'beta')):
        return ''

    if public.version_key(available) < public.version_key(installed):
        return ''
    return ':'.join((channel, available, build))


def check(selected_channel=None):
    root = Path(__file__).resolve().parents[2]
    manifest = root / 'vorondeck-release.json'
    if not manifest.is_file():
        return {'status': 'not_installed'}
    if manifest.stat().st_size > 2 * 1024**2:
        raise ValueError('Installed release metadata too large')
    installed = json.loads(manifest.read_text())
    if (not isinstance(installed, dict) or installed.get('schema') != 1
            or installed.get('project') != 'VoronDeck'
            or not VERSION.fullmatch(installed.get('version', ''))
            or not COMMIT.fullmatch(installed.get('source_commit', ''))):
        raise ValueError('Invalid installed release metadata')
    try:
        libc = 'glibc' if os.confstr('CS_GNU_LIBC_VERSION') else 'musl'
    except (OSError, ValueError):
        libc = 'musl'
    home = Path.home()
    has_beta_key = ((home / '.ssh/vorondeck_beta_readonly').is_file()
                    and (home / '.ssh/known_hosts_vorondeck').is_file())
    result = check_channels(installed, root.name, libc, has_beta_key,
                            selected_channel=selected_channel)
    result['installed_source'] = installed.get('source_commit', '')
    try:
        floor = installed['version']
        stable_version = result.get('channels', {}).get('stable', {}).get('version', '')
        if VERSION.fullmatch(stable_version) and public.version_key(stable_version) > public.version_key(floor):
            floor = stable_version
        offer = public.latest(installed['architecture'], libc, floor)
        result['test_status'] = 'available' if offer else 'none'
        if offer:
            result['offers'].append({key: offer[key] for key in
                                    ('kind', 'version', 'display_version', 'tag', 'sha256', 'source_commit')})
            result['offers'][-1]['notification_id'] = 'test:' + offer['version'] + ':' + offer['sha256']
    except (OSError, ValueError, TypeError, KeyError):
        result['test_status'] = 'unavailable'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', choices=tuple(CHANNELS))
    args = parser.parse_args()
    try:
        print(json.dumps(check(args.channel)))
    except (OSError, ValueError, TypeError, subprocess.SubprocessError):
        # No Git stderr/configuration/SSH credentials in the UI or logs.
        print(json.dumps({'status': 'error'}))
