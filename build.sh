#!/bin/sh
# Build UltraScreenWake.kext, UltraProbe.kext and the UltraSleep tables.
set -eu
cd "$(dirname "$0")"
./UltraScreenWake/build.sh
./UltraProbe/build.sh
./UltraSleep/build.sh
