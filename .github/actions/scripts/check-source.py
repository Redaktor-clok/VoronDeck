#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Parse tracked XML and Python without importing printer-facing modules."""
import ast
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

files = subprocess.check_output(['git', 'ls-files', '-z'], text=True).split('\0')
counts = {'python': 0, 'xml': 0}
for name in files:
    path = Path(name)
    if name.endswith('.xml') and name.startswith(('ui_xml/', 'translations/')):
        ET.parse(path)
        counts['xml'] += 1
    elif name.endswith('.py') and name.startswith(('scripts/', 'moonraker-plugin/', '.github/actions/')):
        ast.parse(path.read_text(encoding='utf-8'), filename=name)
        counts['python'] += 1
assert all(counts.values()), 'No source files checked'
print(counts)
