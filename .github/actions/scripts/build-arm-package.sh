#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
mkdir -p build/package
dpkg-query -W > build/package/toolchain.txt
export HELIX_PACKAGING=1
make PLATFORM_TARGET=pi SKIP_OPTIONAL_DEPS=1 ENABLE_MOCKS=no ENABLE_REMOTE_CONTROL=no ENABLE_DIAGNOSTIC_UPLOADS=no
test "$(cat build/pi/bin/.build-features)" = $'remote_control=no\ndiag_uploads=no'
if grep -qE 'Mock[A-Z_:]|[a-z]Mock\b|mock_internal' build/pi/bin/helix-screen.sym; then
    echo 'Mock symbols found in production package' >&2
    exit 1
fi
version="$(cat VERSION.txt)"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?$ ]]
git -c safe.directory=/src diff --exit-code -- src include ui_xml assets config VERSION.txt
mkdir -p build/package/packages
aarch64-linux-gnu-readelf -h -d -V build/pi/bin/helix-screen > build/package/elf.txt
python3 scripts/vorondeck-package.py --binary build/pi/bin/helix-screen --version "$version" \
  --output "build/package/packages/VoronDeck-$version-aarch64-glibc.tar.gz" > build/package/package.json
python3 .github/actions/scripts/check-beta-package.py
if [[ "$version" != *-beta.* ]]; then
  python3 .github/actions/scripts/check-stable-package.py build/package .
fi
