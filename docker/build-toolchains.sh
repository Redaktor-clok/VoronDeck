#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
case "${1:-all}" in
    pi|pi32|x86) targets="$1" ;;
    all) targets="pi pi32 x86" ;;
    -h|--help|help) printf 'Usage: %s [pi|pi32|x86|all]\n' "$0"; exit 0 ;;
    *) printf 'Unsupported Linux toolchain: %s\n' "$1" >&2; exit 1 ;;
esac
for target in $targets; do
    docker build -t "helixscreen/toolchain-$target" -f "Dockerfile.$target" .
done
