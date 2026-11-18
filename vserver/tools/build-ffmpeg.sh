#!/usr/bin/env bash
# Compila FFmpeg con Visual Studio (MSVC), solo con i componenti usati dal video server.
# Va eseguito in una shell MSYS2 con l'ambiente di Visual Studio caricato (vcvars64).
# Uso: build-ffmpeg.sh <cartella sorgenti FFmpeg> <cartella di installazione>
#
# Perche' non una build pronta: le build MinGW usano winpthreads, che importa
# SetThreadDescription da KERNEL32. Su Windows 10 1607 / Server 2016 quella funzione
# esiste solo in KernelBase, e le DLL non si caricano (errore 0xC0000139).
# I thread nativi di FFmpeg (w32threads) la cercano invece a runtime in KernelBase.
set -euo pipefail
src=$(cygpath -u "$1")
prefix=$(cygpath -u "$2")

# Il link.exe di MSYS2 nasconderebbe quello di Visual Studio.
[ -f /usr/bin/link.exe ] && mv /usr/bin/link.exe /usr/bin/link-msys.exe

cd "$src"
./configure \
  --toolchain=msvc --arch=x86_64 --target-os=win64 --prefix="$prefix" \
  --enable-shared --disable-static --disable-programs --disable-doc \
  --disable-everything --disable-network --disable-autodetect --enable-w32threads \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-encoder=mpeg2video,pcm_s24le \
  --enable-decoder=mpeg2video,pcm_s24le,pcm_s16le \
  --enable-muxer=mxf --enable-demuxer=mxf \
  --enable-parser=mpegvideo --enable-protocol=file \
  --enable-x86asm
make -j"$(nproc)"
make install
# Con MSVC le librerie di importazione possono finire in bin: il progetto le cerca in lib.
mkdir -p "$prefix/lib"
for f in "$prefix"/bin/*.lib; do [ -e "$f" ] && mv "$f" "$prefix/lib/"; done
ls "$prefix/bin" "$prefix/lib"
