#!/bin/bash
# Build RV530Agent on a macOS host (Xcode or the Command Line Tools), for a
# guest without Xcode 3.2: install.sh uses ./RV530Agent when the guest has
# no gcc.  The SDK still links for 10.6 (crt1.10.6.o), but today's ld only
# writes LC_MAIN, which 10.6's dyld predates; unixthread.py swaps it for
# the LC_UNIXTHREAD an old ld would have written.
#     snowleopard/agent/build-host.sh      -> snowleopard/agent/RV530Agent
set -euo pipefail
cd "$(dirname "$0")"
[ "$(uname -s)" = Darwin ] || { echo "build-host.sh: needs a macOS host" >&2; exit 1; }
clang -target x86_64-apple-macos10.6 -Wall -O2 -Wl,-headerpad,0x400 \
    -framework Cocoa -framework ApplicationServices -framework IOKit \
    -o RV530Agent RV530Agent.m
python3 unixthread.py RV530Agent
echo "built $(pwd)/RV530Agent"
