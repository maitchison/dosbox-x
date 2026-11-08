#!/bin/sh
# make-linux-base.sh ROOT TREE OUTDIR: fury web linux-base run inside the 22.04 userland, so lib/ holds
# 22.04's libraries, on TREE/src/dosbox-x (build-linux.sh), with patches/ beside it
set -e
here=$(cd "$(dirname "$0")" && pwd); fury=$(cd "$here/../../../.." && pwd)
root=$1; tree=$2; out=$3; mkdir -p "$out"
exec bwrap --unshare-user --uid 0 --gid 0 --bind "$root" / --proc /proc --dev /dev --tmpfs /tmp \
  --bind "$tree" /src --ro-bind "$fury" /fury --bind "$out" /out --ro-bind "$here/patches" /patches \
  --setenv HOME /root --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin --chdir /out -- \
  python3 /fury/hdrv/py/linuxbase.py /src/src/dosbox-x --source /src --patches /patches \
  --head "tag dosbox-x-v2026.10.01 (commit b6abbd5980a885f5f310a4088c59a8688d1b116c), official release source (archive sha256 df023a6c0e4a139dcbd60befff5947e0db2cc68e4e79db463670f581a4044ed4) with the four patches below. Built on Ubuntu 22.04 (ubuntu-base 22.04.5, gcc 11) as the stock base: hdrv/probes/dosbox-x86-fix/bundle-2026.10.01/build-linux.sh" \
  --out /out/base-2026.10.01-fury1.tar.gz
