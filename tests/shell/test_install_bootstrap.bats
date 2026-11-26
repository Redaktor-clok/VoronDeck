#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

@test "legacy entry point forwards all arguments and the exit status locally" {
    local root="$BATS_TEST_TMPDIR/install"
    mkdir -p "$root/scripts"
    cp "$BATS_TEST_DIRNAME/../../scripts/install.sh" "$root/scripts/install.sh"
    cat > "$root/install.sh" <<'SH'
#!/bin/sh
printf '<%s>\n' "$@"
exit 7
SH
    run sh "$root/scripts/install.sh" --channel stable 'value with spaces'
    [ "$status" -eq 7 ]
    [ "$output" = $'<--channel>\n<stable>\n<value with spaces>' ]
}
