#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "build and validation workflows isolate concurrency by workflow and branch" {
    run python3 - <<'PY'
import pathlib, yaml
names = ('quality.yml', 'release-validation.yml', 'arm-package.yml')
groups = []
for name in names:
    doc = yaml.safe_load((pathlib.Path('.github/workflows') / name).read_text())
    concurrency = doc['concurrency']
    group = concurrency['group']
    assert 'github.ref' in group, (name, group)
    assert 'cancel-in-progress' in concurrency, name
    groups.append(group)
assert len(set(groups)) == len(names)
PY
    [ "$status" -eq 0 ]
}

@test "package and final validation runs are not cancelled by a later push" {
    run python3 - <<'PY'
import pathlib, yaml
for name in ('release-validation.yml', 'arm-package.yml'):
    doc = yaml.safe_load((pathlib.Path('.github/workflows') / name).read_text())
    assert doc['concurrency']['cancel-in-progress'] is False, name
doc = yaml.safe_load(pathlib.Path('.github/workflows/quality.yml').read_text())
assert "github.ref != 'refs/heads/main'" in doc['concurrency']['cancel-in-progress']
PY
    [ "$status" -eq 0 ]
}

@test "release validation runs on candidate pushes and cannot publish packages" {
    run python3 - <<'PY'
import pathlib, yaml
doc = yaml.safe_load(pathlib.Path('.github/workflows/release-validation.yml').read_text())
triggers = doc.get('on', doc.get(True))
assert 'release-*' in triggers['push']['branches']
assert 'workflow_dispatch' in triggers
assert doc['permissions'] == {'contents': 'read'}
assert all('permissions' not in job for job in doc['jobs'].values())
PY
    [ "$status" -eq 0 ]
}
