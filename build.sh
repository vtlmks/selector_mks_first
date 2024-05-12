#!/bin/bash

# Define output file base name
OUT_FILE="mks_first"

# Define common compiler flags
COMMON_CFLAGS="-O2 -Wall -Wextra -pedantic -Wshadow -Wconversion -I../../include"
DEBUG_FLAGS="-g"
FPIC_FLAGS="-fPIC"
SHARED_FLAGS="-shared"

# Define the Linux output file and compile
LINUX_OUT="selector_${OUT_FILE}.so"
gcc $DEBUG_FLAGS $COMMON_CFLAGS $SHARED_FLAGS $FPIC_FLAGS -o "$LINUX_OUT" selector.c

# Define the Windows output file and compile
WINDOWS_OUT="selector_${OUT_FILE}.dll"
x86_64-w64-mingw32-gcc $COMMON_CFLAGS $SHARED_FLAGS -o "$WINDOWS_OUT" selector.c

# Move the Linux binary if it exists
[ -e "$LINUX_OUT" ] && mv "$LINUX_OUT" ../../bin/remakes

# Move the Windows binary if it exists
[ -e "$WINDOWS_OUT" ] && mv "$WINDOWS_OUT" ../../bin/remakes
