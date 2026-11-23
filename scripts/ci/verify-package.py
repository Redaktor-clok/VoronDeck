#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the actual archive with the installer before retaining a CI package."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path('scripts').resolve()))
spec = importlib.util.spec_from_file_location('installer', 'scripts/install-vorondeck.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)
record = json.loads(Path('build/ci-reports/package.json').read_text())
package = Path(record['package'])
manifest, payload = installer.verify_package(package, record['sha256'], 'aarch64')
assert manifest['version'] == Path('VERSION.txt').read_text().strip()
assert manifest['source_commit'] == subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
assert package.stat().st_size < 95 * 1024**2, 'Package exceeded the 95 MiB budget'
settings = json.loads(payload['config/settings.json.template'])
assert settings['wizard_completed'] is False
assert settings['network']['wifi_ssid'] == settings['network']['eth_ip'] == ''
assert settings['print_completion_sound'] is False and settings['ui_sounds_enabled'] is False
printer = settings['printers']['default']
assert printer['moonraker_host'] == '127.0.0.1'
assert printer['type'] == 'Unknown' and printer['hardware']['last_snapshot'] == {}
assert 'panel_widgets' not in printer
assert 'config/settings.json' not in payload
assert not any(name.startswith(('assets/presentation/', '.ssh/', '.dev-vm/')) for name in payload)
assert not any(name.endswith(('.pem', '.key', '.ps1', '.pdf')) for name in payload)
elf = Path('build/ci-package/elf.txt').read_text()
assert 'AArch64' in elf
for prefix, maximum in [('GLIBC', (2, 31)), ('GLIBCXX', (3, 4, 28))]:
    versions = [tuple(map(int, value.split('.'))) for value in re.findall(prefix + r'_([0-9.]+)', elf)]
    assert versions and max(versions) <= maximum, f'{prefix} ABI requirement increased'
record.update(source_commit=manifest['source_commit'], version=manifest['version'],
              files=len(payload), size=package.stat().st_size, clean_first_run=True,
              binary_sha256=hashlib.sha256(payload['bin/helix-screen']).hexdigest())
Path('build/ci-package/verified.json').write_text(json.dumps(record, indent=2) + '\n')
Path('build/ci-package/SHA256SUMS').write_text(f"{record['sha256']}  {package.name}\n")
print(json.dumps(record, indent=2))
