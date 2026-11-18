#!/usr/bin/env bash
# Applies the transmit marker hooks in patches/ to the Zephyr and hal_nxp
# checkouts in the workspace. Safe to re-run: a patch already applied is skipped.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WS=$(dirname "$REPO")

apply() {
    local tree=$1 patch=$2
    if git -C "$tree" apply --reverse --check "$patch" 2>/dev/null; then
        echo "already applied: $(basename "$patch")"
    else
        git -C "$tree" apply "$patch"
        echo "applied: $(basename "$patch")"
    fi
}

apply "$WS/zephyr" "$REPO/patches/zephyr-enet-tx-done-hook.patch"
apply "$WS/modules/hal/nxp" "$REPO/patches/hal_nxp-enet-tx-activate-hook.patch"
