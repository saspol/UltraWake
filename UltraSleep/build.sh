#!/bin/sh
# Compile the UltraSleep tables with iasl (ACPICA). IASL may point at another
# iasl; tested with ACPICA 20260930.
set -eu
cd "$(dirname "$0")"
IASL=${IASL:-iasl}
for t in SSDT-UltraSleep SSDT-UltraSleep-Debug; do
    "$IASL" -vw 3133 "$t.dsl"
done
