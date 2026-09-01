#!/bin/sh
# Cross-compile with MinGW-w64 (works on Linux, macOS or Windows).
set -e
x86_64-w64-mingw32-gcc -O2 -municode -mwindows \
    -o claude-traffic-light.exe src/traffic-light.c \
    -lws2_32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lm -s
echo "built: claude-traffic-light.exe"
