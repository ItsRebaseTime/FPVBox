#!/usr/bin/env bash
# Fetch ExpressLRS at the pinned release into components/elrs_tx/ExpressLRS
# and apply the FPVBox patches. ELRS links only between the same major
# version: match ELRS_TAG to the firmware on your receivers (4.x by default).
set -euo pipefail
cd "$(dirname "$0")"
ELRS_URL=https://github.com/ExpressLRS/ExpressLRS.git
ELRS_TAG=${ELRS_TAG:-4.1.0}

if [ ! -d ExpressLRS/.git ]; then
    git clone --depth 1 --branch "$ELRS_TAG" "$ELRS_URL" ExpressLRS
else
    git -C ExpressLRS fetch -q --depth 1 origin "refs/tags/$ELRS_TAG:refs/tags/$ELRS_TAG"
    git -C ExpressLRS checkout -q "$ELRS_TAG"
fi
for p in patches/*.patch; do
    [ -e "$p" ] || continue
    if git -C ExpressLRS apply --reverse --check "../$p" 2>/dev/null; then
        echo "already applied: $p"
    else
        git -C ExpressLRS apply "../$p"
        echo "applied: $p"
    fi
done
echo "ExpressLRS $ELRS_TAG ready"
