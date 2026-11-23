#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify a stable candidate without executing its binaries or scripts."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys


def verify(candidate, source):
    index_path = candidate / 'release.json'
    if index_path.is_symlink() or index_path.stat().st_size > 65536:
        raise ValueError('Invalid release index')
    index = json.loads(index_path.read_text())
    if (index.get('schema') != 1 or index.get('project') != 'VoronDeck'
            or index.get('channel') != 'stable'
            or not re.fullmatch(r'\d+\.\d+\.\d+', index.get('version', ''))):
        raise ValueError('Expected a stable VoronDeck release')
    packages = index.get('packages')
    if not isinstance(packages, list) or not 1 <= len(packages) <= 12:
        raise ValueError('Invalid package list')
    sys.path.insert(0, str(source / 'scripts'))
    spec = importlib.util.spec_from_file_location('stable_installer', source / 'scripts/install-vorondeck.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    seen = set()
    expected_sums = []
    for package in packages:
        path = package.get('path', '')
        sha, commit = package.get('sha256', ''), package.get('source_commit', '')
        if (not re.fullmatch(r'packages/[A-Za-z0-9_.-]+\.tar\.gz', path)
                or not re.fullmatch(r'[a-f0-9]{64}', sha)
                or not re.fullmatch(r'[a-f0-9]{40}', commit)
                or not re.fullmatch(r'[a-f0-9]{64}', package.get('binary_sha256', ''))
                or type(package.get('size')) is not int or not 0 < package['size'] < 95 * 1024**2):
            raise ValueError('Invalid package metadata')
        key = (package.get('architecture'), package.get('libc'))
        if key in seen or key[1] != 'glibc':
            raise ValueError('Duplicate or unsupported ABI')
        seen.add(key)
        # The source checkout is protected main, not candidate-controlled code.
        subprocess.run(['git', '-C', str(source), 'merge-base', '--is-ancestor', commit, 'HEAD'], check=True)
        archive = candidate / path
        if archive.is_symlink() or not archive.resolve().is_relative_to(candidate.resolve()) or archive.stat().st_size != package['size']:
            raise ValueError('Invalid package path or size')
        manifest, payload = installer.verify_package(archive, sha, key[0])
        if (manifest['version'] != index['version'] or manifest['source_commit'] != commit
                or hashlib.sha256(payload['bin/helix-screen']).hexdigest() != package['binary_sha256']):
            raise ValueError('Package provenance mismatch')
        expected_sums.append(f'{sha}  {path}')
    if sorted((candidate / 'SHA256SUMS').read_text().splitlines()) != sorted(expected_sums):
        raise ValueError('SHA256SUMS differs from release index')
    return index['version']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    print('Verified stable package:', verify(args.candidate, args.source))
