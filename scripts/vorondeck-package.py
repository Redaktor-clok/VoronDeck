#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a prebuilt VoronDeck binary and tracked runtime data for beta testing."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
from vorondeck_distribution import elf_arch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-beta\.[0-9]+)?', args.version):
        parser.error('Use a version such as 1.0.0 or 1.0.0-beta.1')
    root = Path(__file__).resolve().parents[1]
    if args.output.exists():
        parser.error('Refusing to overwrite an existing package')
    binary = args.binary.read_bytes()
    arch = elf_arch(binary)
    features = args.binary.parent / '.build-features'
    if not features.is_file():
        parser.error('Missing .build-features: build provenance is required')
    flags = features.read_text()
    for flag in ('remote_control=no', 'diag_uploads=no'):
        if flag not in flags.splitlines():
            parser.error('Production build must record ' + flag)
    names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z']).decode().split('\0')
    payload = {'bin/helix-screen': (binary, 0o755)}
    bluetooth = args.binary.parent / 'libhelix-bluetooth.so'
    if bluetooth.is_file():
        data = bluetooth.read_bytes()
        if bluetooth.is_symlink() or elf_arch(data) != arch:
            raise ValueError('Bluetooth plugin architecture mismatch or linked build artifact')
        payload['bin/libhelix-bluetooth.so'] = (data, 0o644)
    for name in names:
        if not name:
            continue
        parts = Path(name).parts
        runtime = (parts[0] == 'ui_xml' or
                   parts[0] == 'assets' and len(parts) > 1 and
                   parts[1] not in ('test_gcodes', 'test_timelapse', 'presentation'))
        legal = name in ('LICENSE', 'COPYRIGHT', 'docs/VORONDECK-CREDITS.md')
        template = name == 'config/settings.json.template'
        if runtime or legal or template:
            path = root / name
            if path.is_symlink() or not path.is_file():
                raise ValueError('Runtime source missing or linked: ' + name)
            payload[name] = (path.read_bytes(), 0o644)
    manifest = {'schema': 1, 'project': 'VoronDeck', 'version': args.version,
                'architecture': arch, 'source_commit': subprocess.check_output(
                    ['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
                'files': {name: hashlib.sha256(data).hexdigest()
                          for name, (data, _) in payload.items()}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, 'w:gz') as archive:
        for name, (data, mode) in sorted(payload.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), mode
            archive.addfile(info, io.BytesIO(data))
        data = json.dumps(manifest, indent=2).encode()
        info = tarfile.TarInfo('vorondeck-release.json')
        info.size, info.mode = len(data), 0o644
        archive.addfile(info, io.BytesIO(data))
    print(json.dumps({'package': str(args.output), 'architecture': arch,
                      'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
