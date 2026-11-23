#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot disposable mock profiles at four sizes and retain real UI screenshots."""
import itertools
import json
from pathlib import Path
import sys

from PIL import Image

root = Path.cwd()
sys.path.insert(0, str(root / 'tests/ui'))
from helix.app import HelixApp

binary = root / 'build/bin/helix-screen'
assert binary.is_file(), 'Build the application before UI checks'
report = root / 'build/ci-reports/ui'
report.mkdir(parents=True, exist_ok=True)
results = []
for size, ams, theme in itertools.product(
        ['800x480', '480x800', '1024x600', '600x1024'], ['afc', 'none'], ['dark', 'light']):
    case = report / f'{size}-{ams}-{theme}'
    case.mkdir(exist_ok=True)
    app = HelixApp(binary, case / 'control.sock', case / 'app.log',
                   extra_args=['-s', size, '--' + theme],
                   extra_env={'HELIX_MOCK_AMS': ams, 'HELIX_HOT_RELOAD': '0'})
    try:
        app.start()
        app.navigate('home')
        app.wait_idle()
        for panel in ['home', 'controls', 'settings', 'filament', 'print-select']:
            app.navigate(panel)
            app.wait_idle()
            assert app.current()['panel'] == panel
            shot = case / f'{panel}.png'
            app.screenshot(str(shot))
            with Image.open(shot) as image:
                assert image.size == tuple(map(int, size.split('x')))
                assert image.convert('L').getextrema()[0] != image.convert('L').getextrema()[1]
        results.append({'size': size, 'ams': ams, 'theme': theme, 'navigation': 'passed'})
        print(case.name, 'PASS', flush=True)
    finally:
        app.stop()
        (report / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
