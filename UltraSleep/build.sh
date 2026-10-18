#!/bin/sh
# Compile the UltraSleep tables with iasl (ACPICA). IASL may point at any
# recent iasl; the tables use only ACPI 2.0 operators.
set -eu
cd "$(dirname "$0")"
IASL=${IASL:-iasl}
for t in SSDT-UltraSleep SSDT-UltraSleep-Debug; do
    "$IASL" -vw 3133 "$t.dsl"
done
