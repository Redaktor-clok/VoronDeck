#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
load helpers

# ------------------------------------------------------------- verifier

VERSION=0.0.0-dryrun.42
BASE=https://cdn.example.invalid/dry-run/42/

# A consistent artifact + R2 prefix for pi and x86 on channels beta and dev.
make_fixture() {
    ART="$BATS_TEST_TMPDIR/art" R2="$BATS_TEST_TMPDIR/r2"
    REL="$R2/releases/v$VERSION"
    mkdir -p "$ART" "$REL" "$R2/beta" "$R2/dev"
    local p
    for p in pi x86; do
        echo "$p tarball" > "$ART/helixscreen-$p-v1.1.0-beta.3.tar.gz"
        echo "$p zip" > "$ART/helixscreen-$p.zip"
    done
    echo '#!/bin/sh' > "$ART/install.sh"
    cp "$ART"/*.tar.gz "$ART"/*.zip "$REL/"
    cp "$ART/install.sh" "$R2/install.sh"
    scripts/generate-manifest.sh --version "$VERSION" --tag "v$VERSION" --notes n \
        --dir "$REL" --base-url "${BASE}releases/v$VERSION" \
        --zip-exclude "" --output "$R2/beta/manifest.json" >/dev/null
    cp "$R2/beta/manifest.json" "$R2/dev/manifest.json"
}

verify() {
    run scripts/verify-dry-run-release.sh --artifact "$ART" --r2 "$R2" --base-url "$BASE" \
        --version "$VERSION" --platforms "pi x86" --channels "beta dev"
}

@test "verifier passes a consistent dry run" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    verify
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" == *"beta -> releases/v$VERSION/helixscreen-x86.zip"* ]] || fail "$output"
}

@test "verifier fails when install.sh differs" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    echo changed >> "$R2/install.sh"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"install.sh differs"* ]] || fail "$output"
}

@test "verifier fails when a manifest points outside the dry-run prefix" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    sed -i 's|/dry-run/42/releases|/releases|' "$R2/dev/manifest.json"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"dev manifest points outside the dry-run prefix"* ]] || fail "$output"
}

@test "verifier fails when an uploaded file does not match its manifest sha256" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    echo corrupt >> "$REL/helixscreen-pi.zip"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"sha256 for ${BASE}releases/v$VERSION/helixscreen-pi.zip"* ]] || fail "$output"
}

@test "verifier fails when a platform's zip never reached R2" {
    command -v jq >/dev/null || skip "jq not installed"
    make_fixture
    rm "$REL/helixscreen-x86.zip"
    verify
    [ "$status" -eq 1 ]
    [[ "$output" == *"x86 zip in $REL missing"* ]] || fail "$output"
}
