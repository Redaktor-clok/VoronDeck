#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report manual LVGL cleanup calls lacking an explicit lifecycle justification."""
import argparse
from pathlib import Path
import re

from check_gcode_error_ownership import strip_comments_and_strings

CALL = re.compile(r'\b(lv_(?:display|group|indev)_delete)\s*\(')
JUSTIFICATION = re.compile(r'//\s*NOLINT\(helix-shutdown\)\s*:?[ \t]+\S.+')


def findings(source):
    original = source.splitlines()
    for match in CALL.finditer(strip_comments_and_strings(source)):
        line = source.count('\n', 0, match.start())
        annotation = original[line]
        if line and original[line - 1].lstrip().startswith('//'):
            annotation += '\n' + original[line - 1]
        if not JUSTIFICATION.search(annotation):
            yield line + 1, match.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='*', type=Path)
    args = parser.parse_args()
    paths = args.files or sorted(Path('src').rglob('*.cpp'))
    if not paths:
        parser.error('No C++ files found')
    violations = []
    for path in paths:
        for line, name in findings(path.read_text(encoding='utf-8')):
            violations.append(f'{path}:{line}: {name}: review ownership before manual cleanup')
    if violations:
        print('\n'.join(violations))
        return 1
    print(f'LVGL cleanup: {len(paths)} files checked; no unjustified calls')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
