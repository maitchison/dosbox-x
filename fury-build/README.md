# Fury builds of DOSBox-X 2026.10.01

This branch is the upstream tag `dosbox-x-v2026.10.01` plus four fixes to the
`dynamic_x86` core and DOS, one commit each. It is the source of the "fury1"
DOSBox-X that Airtime's Windows and Linux download packages may carry.

The scripts here are how those binaries are built: an Ubuntu 22.04 userland
under bubblewrap (`setup-root.sh`, `enter.sh`), then `build-linux.sh` and
`build-win64.sh` (mingw-w64, static). `make-linux-base.sh` and
`make-win64-base.py` pack the result; the Win64 one replaces `dosbox-x.exe` in
upstream's own portable zip. They call helpers from the Fury harness, which is
not public; nothing they do changes the source.

DOSBox-X is GPL v2 (see `COPYING`).
