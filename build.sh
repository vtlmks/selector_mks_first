#!/bin/bash

# Define output file base name
OUT_FILE="mks_first"
LINUX_OUT="selector_${OUT_FILE}.so"
WINDOWS_OUT="selector_${OUT_FILE}.dll"

# Define common compiler flags
COMMON_CFLAGS="-O2 -I../../include"
DEBUG_FLAGS="-g"
FPIC_FLAGS="-fPIC"
SHARED_FLAGS="-shared"

pushd data > /dev/null

bmp2h -i ../graphics/ddr_tiny_small8x8.bmp -o ddr_tiny_small8x8
bin2h -i ../music/zeus.mod -o zeus

popd > /dev/null

# Linux compilation
gcc $DEBUG_FLAGS $COMMON_CFLAGS $SHARED_FLAGS $FPIC_FLAGS -o "$LINUX_OUT" selector.c

# Windows compilation
x86_64-w64-mingw32-gcc $COMMON_CFLAGS $SHARED_FLAGS -o "$WINDOWS_OUT" selector.c

# Move the binaries if they exist
[ -e "$LINUX_OUT" ] && mv "$LINUX_OUT" ../../bin/remakes
[ -e "$WINDOWS_OUT" ] && mv "$WINDOWS_OUT" ../../bin/remakes
