#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v python3 >/dev/null 2>&1; then
    echo 'VoronDeck: install Python 3.9+ using your Linux package manager, then retry.' >&2
    exit 1
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' || {
    echo 'VoronDeck requires Python 3.9 or newer.' >&2
    exit 1
}
exec python3 "$ROOT/scripts/vorondeck-bootstrap.py" "$@"
