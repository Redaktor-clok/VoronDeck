#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
export HELIX_PATCHES_FROM_CLEAN=1
mkdir -p build/ci-reports build/ci-package build/.ccache-arm
docker run --rm --user "$(id -u):$(id -g)" \
  -e HOME=/tmp -e CCACHE_DIR=/src/build/.ccache-arm -e CCACHE_MAXSIZE=2G \
  -e HELIX_PATCHES_FROM_CLEAN -e HELIX_GIT_HASH="$(git rev-parse HEAD)" \
  -v "$PWD:/src" -w /src vorondeck/toolchain-arm64 \
  make PLATFORM_TARGET=pi SKIP_OPTIONAL_DEPS=1 HELIX_PACKAGING=1 \
    ENABLE_MOCKS=no ENABLE_REMOTE_CONTROL=no ENABLE_DIAGNOSTIC_UPLOADS=no \
  2>&1 | tee build/ci-reports/arm64-build.log
test -s build/pi/bin/helix-screen.sym
if grep -qE 'Mock[A-Z_:]|[a-z]Mock\b|mock_internal' build/pi/bin/helix-screen.sym; then
  echo 'Unexpected mock symbols in production build' >&2
  exit 1
fi
test "$(cat build/pi/bin/.build-features)" = $'remote_control=no\ndiag_uploads=no'
version=$(tr -d '\r\n' < VERSION.txt)
python3 scripts/vorondeck-package.py --binary build/pi/bin/helix-screen \
  --version "$version" --output "build/ci-package/VoronDeck-$version-aarch64-glibc.tar.gz" \
  > build/ci-reports/package.json
readelf -h -d -V build/pi/bin/helix-screen > build/ci-package/elf.txt
python3 scripts/ci/verify-package.py
