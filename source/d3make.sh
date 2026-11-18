#!/bin/bash
# Build the Diablo III recomp (runtime + game library). Usage: bash d3make.sh [make args]
cd <project folder>/src/d3recomp
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
export TMP=<project folder>/tools/tmp TEMP=<project folder>/tools/tmp TMPDIR=<project folder>/tools/tmp
make -j30 "$@" 2>&1 | grep -E "error|undefined|lasgame.dll$|mv -f" | grep -v "^make.*Error 1$" | head -40
rc=${PIPESTATUS[0]}
# (the tester's logo put inside the program, if its tool is there - outside this folder, never shared; D3_NO_LOGO=1: not)
if [ "$rc" = 0 ] && [ -f <project folder>/tools/d3_pack_logo.py ]; then python -I <project folder>/tools/d3_pack_logo.py </dev/null | grep -E '^logo'; fi
exit $rc
