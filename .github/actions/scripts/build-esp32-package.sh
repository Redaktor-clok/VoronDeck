#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
firmware=firmware/helixscreen-esp32
test ! -e "$firmware/sdkconfig.local"
python3 scripts/check_esp32_app_srcs.py
make apply-patches SKIP_OPTIONAL_DEPS=1

# Configure fetches the managed filesystem component. The placeholder is never flashed;
# staging and packing must replace it before the build or packaging can succeed.
mkdir -p "$firmware/build"
touch "$firmware/build/storage_frogfs.bin"
idf.py -C "$firmware" reconfigure
python3 -m venv /tmp/vorondeck-esp-assets
/tmp/vorondeck-esp-assets/bin/python -m pip install Pillow PyYAML
/tmp/vorondeck-esp-assets/bin/python scripts/esp32_stage_assets.py
/tmp/vorondeck-esp-assets/bin/python scripts/esp32_pack_assets.py
test -s "$firmware/build/storage_frogfs.bin"
# Report all compilation failures in one run; a failed target still fails the job.
cmake --build "$firmware/build" -- -k 0
python3 scripts/check_esp32_size.py "$firmware/build/helixscreen_esp32.bin" \
    "$firmware/size_budget.json" "$firmware/build/helixscreen_esp32.map"
bash scripts/esp32_package_release.sh "v$(tr -d '\r\n' < VERSION.txt)" \
    "$firmware/build" build/esp32-package
(cd build/esp32-package && sha256sum ./*.zip > SHA256SUMS)
