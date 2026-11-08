#!/bin/sh
# enter the Ubuntu 22.04 build userland as a namespaced root: enter.sh ROOT SRC [cmd...]
root=$1; src=$2; shift 2
exec bwrap --unshare-user --uid 0 --gid 0 --bind "$root" / --proc /proc --dev /dev \
  --ro-bind /etc/resolv.conf /etc/resolv.conf --tmpfs /tmp --bind "$src" /src \
  --setenv HOME /root --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  --setenv DEBIAN_FRONTEND noninteractive --chdir /src -- "$@"
