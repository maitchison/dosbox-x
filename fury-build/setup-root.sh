#!/bin/sh
# setup-root.sh DIR: the Ubuntu 22.04 build userland both builds run in (bwrap, no sudo).
# ubuntu-base-22.04.5-base-amd64.tar.gz sha256 242cd8898b33ea806ef5f13b1076ed7c76f9f989d18384452f7166692438ff1a
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$1; mkdir -p "$root"; cd "$root"
[ -f ../ubuntu-base-22.04.5-base-amd64.tar.gz ] || \
  curl -sSL -o ../ubuntu-base-22.04.5-base-amd64.tar.gz https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/ubuntu-base-22.04.5-base-amd64.tar.gz
echo "242cd8898b33ea806ef5f13b1076ed7c76f9f989d18384452f7166692438ff1a  ../ubuntu-base-22.04.5-base-amd64.tar.gz" | sha256sum -c
tar xzf ../ubuntu-base-22.04.5-base-amd64.tar.gz
A="apt-get -o APT::Sandbox::User=root"
"$here/enter.sh" "$root" /tmp sh -c "$A update -q && $A install -y -q --no-install-recommends \
  build-essential autoconf automake libtool pkg-config file git ca-certificates nasm \
  libsdl2-dev libsdl2-net-dev libasound2-dev libgl-dev libfreetype-dev zlib1g-dev libpng-dev \
  libpcap-dev libslirp-dev libx11-dev libxrandr-dev libxkbfile-dev libxext-dev libpulse-dev \
  mingw-w64 g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix python3 patchelf binutils"
