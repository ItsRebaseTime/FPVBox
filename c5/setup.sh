#!/usr/bin/env bash
# Fetch C5VRX at the pinned commit and apply the FPVBox board patch.
# Result: c5/C5VRX, buildable for the OSPTEK ESP32-P4C5 module.
set -euo pipefail
cd "$(dirname "$0")"
C5VRX_URL=https://github.com/Twotoz/C5VRX.git
C5VRX_COMMIT=100bf7bfa34e0892a3ab1efd856564d0df37e92a

if [ ! -d C5VRX/.git ]; then
    git clone "$C5VRX_URL" C5VRX
fi
git -C C5VRX fetch -q origin "$C5VRX_COMMIT" || true
git -C C5VRX checkout -q "$C5VRX_COMMIT"
for p in patches/*.patch; do
    if git -C C5VRX apply --reverse --check "../$p" 2>/dev/null; then
        echo "already applied: $p"
    else
        git -C C5VRX apply "../$p"
        echo "applied: $p"
    fi
done
