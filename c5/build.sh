#!/usr/bin/env bash
# Build C5VRX for the OSPTEK ESP32-P4C5 module. Needs ESP-IDF 6.0.2 exported
# (C5VRX pins that release's libphy; other versions build with its filter and
# DC correction disabled).
set -euo pipefail
cd "$(dirname "$0")/C5VRX"
idf.py -B build-p4c5 -DSDKCONFIG=sdkconfig.p4c5 \
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.p4c5.defaults" build "$@"
