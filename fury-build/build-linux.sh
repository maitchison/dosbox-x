#!/bin/sh
# the official 2026.10.01 source with its own vs/sdl2 and vs/sdlnet, as build-sdl2 does;
# SDL2 configured as vs/sdl2/build-dosbox.sh does but X11 loaded at run time (no --disable-x11-shared)
set -e
apt-get -o APT::Sandbox::User=root remove -y -q libsdl2-dev libsdl2-net-dev >/dev/null
cd /src
./autogen.sh
chmod +x vs/sdl/build-scripts/strip_fPIC.sh || true
cd vs/sdl2 && rm -rf linux-host linux-build && mkdir linux-host linux-build && chmod +x configure && cd linux-build
ac_cv_header_iconv_h=no ac_cv_func_iconv=no ac_cv_lib_iconv_libiconv_open=no ../configure --srcdir=/src/vs/sdl2 --prefix=/src/vs/sdl2/linux-host --enable-static --disable-shared --disable-video-x11-xrandr --disable-video-x11-vm --disable-video-x11-xv
make -j8 && make install
cd /src/vs/sdlnet && ./build-dosbox.sh
cd /src && ./configure --enable-sdl2 --disable-debug && make -j8
