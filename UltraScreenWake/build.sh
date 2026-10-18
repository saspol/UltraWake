#!/bin/sh
# Build UltraScreenWake.kext with the Command Line Tools SDK (no Xcode needed).
set -eu
cd "$(dirname "$0")"

NAME=UltraScreenWake
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk}
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
OUT=build
KEXT="$OUT/$NAME.kext"

rm -rf "$OUT"
mkdir -p "$KEXT/Contents/MacOS"

CFLAGS="-arch x86_64 -isysroot $SDK -I$KH -mkernel -nostdinc -fno-builtin -fno-common
        -mmacosx-version-min=15.5 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
        -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-inconsistent-missing-override -Werror"

clang $CFLAGS -c kmod_info.c -o "$OUT/kmod_info.o"
clang++ $CFLAGS -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti \
    -c "$NAME.cpp" -o "$OUT/$NAME.o"

clang++ -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=15.5 -nostdlib \
    -Xlinker -kext -Xlinker -export_dynamic \
    "$OUT/kmod_info.o" "$OUT/$NAME.o" \
    -L"$SDK/usr/lib" -lkmodc++ -lkmod -lcc_kext \
    -o "$KEXT/Contents/MacOS/$NAME"

cp Info.plist "$KEXT/Contents/Info.plist"
plutil -lint "$KEXT/Contents/Info.plist"
echo "built $KEXT"
