#!/bin/sh
# build-win64.sh, run as root in the 22.04 userland (enter.sh ROOT TREE sh /src/build-win64.sh) with
# the patched tree at /src: DOSBox-X for Win64 cross-compiled with mingw-w64 (gcc 10, posix
# threads), statically, with the libraries build-mingw-sdl2 uses built from the tree's own vs/
# copies. Configure as upstream's mingw64 CI (build-mingw-sdl2) but --disable-debug (no
# curses for mingw here; the Linux base is --disable-debug too) and no libslirp.
set -e
H=x86_64-w64-mingw32
export CC=$H-gcc-posix CXX=$H-g++-posix AR=$H-ar RANLIB=$H-ranlib WINDRES=$H-windres STRIP=$H-strip
P=/src/wprefix W=/src/wcase
rm -rf $P $W && mkdir -p $P/include $P/lib $P/bin $W
export PATH=$P/bin:$PATH PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_PATH=
# shims for building on a case-sensitive host with Ubuntu's mingw-w64; none changes the source
I=/usr/share/mingw-w64/include
grep -rhoE '#\s*include\s*[<"][A-Za-z0-9_/]*[A-Z][A-Za-z0-9_]*\.h[>"]' /src/src /src/include | \
  sed -E 's/.*[<"](.*)[>"]/\1/' | sort -u | while read h; do
    l=$(echo "$h" | tr A-Z a-z)
    if [ -f "$I/$l" ] && [ ! -f "$I/$h" ]; then echo "#include <$l>" > "$W/$h"; fi
  done
echo "#include <d3dx9shader.h>" > "$W/D3dx9shader.h"
printf '#include "/src/src/ints/int10.h"\n' > "$W/..\\ints\\int10.h"   # output_direct3d11.cpp
# dos_programs.cpp includes misc/winiconv.c (win-iconv) and then <iconv.h>, MSYS2's libiconv header
cat > $W/iconv.h <<'H'
#include <stddef.h>
typedef void* iconv_t;
iconv_t iconv_open(const char *tocode, const char *fromcode);
int iconv_close(iconv_t cd);
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft);
H
# Ubuntu's mingw-w64 libuuid lacks this key (mic_input_win32.cpp), MSYS2's has it
cat > $W/pkey.c <<'H'
#define INITGUID
#include <windows.h>
#include <propkeydef.h>
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);
H
$CC -c -o $W/pkey.o $W/pkey.c
cd /src && ./autogen.sh
chmod +x vs/sdl/build-scripts/strip_fPIC.sh || true
cd /src/vs/zlib && make -f win32/Makefile.gcc PREFIX=$H- CC=$CC libz.a >/dev/null && cp zlib.h zconf.h $P/include && cp libz.a $P/lib && make -f win32/Makefile.gcc clean >/dev/null
cd /src/vs/libpng && rm -rf wbuild && mkdir wbuild && cd wbuild && chmod +x ../configure && \
  CPPFLAGS=-I$P/include LDFLAGS=-L$P/lib ../configure --host=$H --prefix=$P --enable-static --disable-shared >/dev/null && \
  make pnglibconf.h >/dev/null && sed -e 's/\x0D//' pnglibconf.h > x && mv x pnglibconf.h && make -j8 >/dev/null && make install >/dev/null
cd /src/vs/freetype && rm -rf wbuild && mkdir wbuild && cd wbuild && chmod +x ../configure ../builds/unix/install-sh && \
  ../configure --host=$H --prefix=$P --enable-static --disable-shared --with-zlib=no --with-bzip2=no --with-harfbuzz=no --with-png=no --with-brotli=no >/dev/null && make -j8 >/dev/null && make install >/dev/null
cd /src/vs/sdl2 && rm -rf wbuild && mkdir wbuild && cd wbuild && chmod +x ../configure && \
  ac_cv_header_iconv_h=no ac_cv_func_iconv=no ac_cv_lib_iconv_libiconv_open=no ../configure --host=$H --prefix=$P --enable-static --disable-shared --disable-video-wayland --disable-libudev >/dev/null && make -j8 >/dev/null && make install >/dev/null
cd /src/vs/sdl2net && ./autogen.sh >/dev/null 2>&1 || true; autoreconf -fi >/dev/null 2>&1; rm -rf wbuild && mkdir wbuild && cd wbuild && chmod +x ../configure && \
  ../configure --host=$H --prefix=$P --enable-static --disable-shared --disable-examples --with-sdl-prefix=$P >/dev/null && make -j8 >/dev/null && make install >/dev/null
cd /src
export CPPFLAGS="-I$W -D_WIN32_WINNT=0x0A00 -I$P/include -I$P/include/freetype2 -I$P/include/SDL2" LDFLAGS="-L$P/lib -static -static-libgcc -static-libstdc++ $W/pkey.o"
export INTERNAL_FREETYPE=1
ac_cv_header_iconv_h=no ./configure --host=$H --enable-sdl2 --disable-debug --disable-avcodec --enable-d3d9 --enable-d3d-shaders --disable-libfluidsynth --disable-libslirp --with-sdl2-prefix=$P
make -j8
$STRIP -o src/dosbox-x-stripped.exe src/dosbox-x.exe
ls -la src/dosbox-x.exe src/dosbox-x-stripped.exe
