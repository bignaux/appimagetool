#!/bin/bash

#
# This script installs the required build-time dependencies
# and builds AppImage
#

HERE="$(dirname "$(readlink -f "${0}")")"

# Figure out whether we should use sudo
SUDO=''
if (( $EUID != 0 )); then
    SUDO='sudo'
fi

if [ -e /usr/bin/apt-get ] ; then
  $SUDO apt-get update
  $SUDO bash -ex ./install-build-deps.sh
fi

if [ -e /usr/bin/yum ] ; then
  $SUDO yum -y install epel-release
  $SUDO yum -y install git make binutils fuse glibc-devel glib2-devel fuse-devel gcc zlib-devel autoconf automake libtool xz-devel lz4-devel patch openssl-devel vim-common inotify-tools-devel

  wget http://ftp.gnu.org/gnu/autoconf/autoconf-2.69.tar.gz
  tar xvfvz autoconf-2.69.tar.gz
  cd autoconf-2.69
  ./configure
  make
  $SUDO make install 
fi

# Install dependencies for Arch Linux
if [ -e /usr/bin/pacman ] ; then
  echo "Please submit a pull request if you would like to see Arch Linux support."
  exit 1
fi

# Fetch git submodules
git submodule init
git submodule update

# Clean up from previous run
rm -rf build/ || true

# Patch squashfuse_ll to be a library rather than an executable

cd squashfuse
if [ ! -e ./ll.c.orig ]; then patch -p1 --backup < ../squashfuse.patch ; fi

# Build libsquashfuse_ll library

if [ ! -e ./Makefile ] ; then
  ./autogen.sh
  ./configure --disable-demo --disable-high-level \
    --with-xz=/usr/lib/ --without-lzo
fi

bash --version

make

cd ..

mkdir build

# Compile runtime
make -f Makefile.runtime install
make -f Makefile.runtime clean

cd build

# Compile and link digest tool

cc -o digest ../getsection.c ../digest.c -lssl -lcrypto
# cc -o digest -Wl,-Bdynamic ../digest.c -Wl,-Bstatic -static  -lcrypto -Wl,-Bdynamic -ldl # 1.4 MB
strip digest

# Compile and link validate tool

cc -o validate ../getsection.c ../validate.c -lssl -lcrypto -lglib-2.0 $(pkg-config --cflags glib-2.0)
strip validate

# Test if we can read it back
readelf -x .upd_info runtime # hexdump
readelf -p .upd_info runtime || true # string

# The raw updateinformation data can be read out manually like this:
HEXOFFSET=$(objdump -h runtime | grep .upd_info | awk '{print $6}')
HEXLENGTH=$(objdump -h runtime | grep .upd_info | awk '{print $3}')
dd bs=1 if=runtime skip=$(($(echo 0x$HEXOFFSET)+0)) count=$(($(echo 0x$HEXLENGTH)+0)) | xxd

# Convert runtime into a data object that can be embedded into appimagetool

ld -r -b binary -o data.o runtime

# Compile appimagetool but do not link - glib version

cc -DVERSION_NUMBER=\"$(git describe --tags --always --abbrev=7)\" -D_FILE_OFFSET_BITS=64 -I../squashfuse/ $(pkg-config --cflags glib-2.0) -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -g -Os ../getsection.c  -c ../appimagetool.c

# Now statically link against libsquashfuse and liblzma - glib version

cc data.o appimagetool.o ../elf.c ../getsection.c -DENABLE_BINRELOC ../binreloc.c ../squashfuse/.libs/libsquashfuse.a ../squashfuse/.libs/libfuseprivate.a -Wl,-Bdynamic -lfuse -lpthread  $(pkg-config --libs glib-2.0 liblzma liblz4) -lz -Wl,-Bdynamic -o appimagetool

# Version without glib
# cc -D_FILE_OFFSET_BITS=64 -I ../squashfuse -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -g -Os -c ../appimagetoolnoglib.c
# cc data.o appimagetoolnoglib.o -DENABLE_BINRELOC ../binreloc.c ../squashfuse/.libs/libsquashfuse.a ../squashfuse/.libs/libfuseprivate.a -Wl,-Bdynamic -lfuse -lpthread -lz -Wl,-Bstatic -llzma -Wl,-Bdynamic -o appimagetoolnoglib

# appimaged, an optional component
cc -std=gnu99 ../getsection.c -Wl,-Bdynamic -DVERSION_NUMBER=\"$(git describe --tags --always --abbrev=7)\" ../elf.c ../appimaged.c ../squashfuse/.libs/libsquashfuse.a ../squashfuse/.libs/libfuseprivate.a -I../squashfuse/ -Wl,-Bstatic -linotifytools -Wl,-Bdynamic $(pkg-config --cflags --libs glib-2.0) $(pkg-config --cflags gio-2.0) $(pkg-config --libs gio-2.0) -ldl -lpthread -lz $(pkg-config --libs liblzma liblz4)  -o appimaged

cd ..

# Strip and check size and dependencies

rm build/*.o
strip build/* 2>/dev/null
chmod a+x build/*
ldd build/appimagetool
ls -lh build/*
