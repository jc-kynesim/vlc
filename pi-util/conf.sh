BASE=`pwd`/..
A=arm-linux-gnueabihf
TOOLS=$BASE/tools/arm-bcm2708/gcc-arm-8.3-2019.03-x86_64-$A
SYSROOT=`pwd`/sysroot/raspian_stretch_pi1-sysroot

HLIB0=/usr/lib/gcc/$A/8
HLIB1=/usr/lib/$A
HLIB2=/usr/lib
HLIB3=/lib/$A
HLIB4=/opt/vc/lib
HLIB5=/usr/local/lib/$A

LIB0=$SYSROOT$HLIB0
LIB1=$SYSROOT$HLIB1
LIB2=$SYSROOT$HLIB2
LIB3=$SYSROOT$HLIB3
LIB4=$SYSROOT$HLIB4
LIB5=$SYSROOT$HLIB5

INCLUDES="-I$SYSROOT/usr/include -I$SYSROOT/usr/include/$A -I$SYSROOT/usr/local/include -I$SYSROOT/opt/vc/include -I$LIB1/dbus-1.0/include"
DEFINES="-ggdb -D__VCCOREVER__=0x04000000"
ARCH="-march=armv7-a -mfpu=neon-vfpv4"
PREFIX=$TOOLS/bin/$A-

mkdir -p build
cd build

PATH="$TOOLS/bin:$PATH" \
  PKG_CONFIG="pkg-config --define-prefix" \
  PKG_CONFIG_PATH="$LIB2/pkgconfig:$LIB3/pkgconfig:$LIB5/pkgconfig" \
  CC=${PREFIX}gcc \
  CFLAGS="$ARCH $INCLUDES $DEFINES" \
  CPP=${PREFIX}cpp \
  CPPFLAGS="$ARCH $INCLUDES $DEFINES" \
  CXX=${PREFIX}g++ \
  CXXFLAGS="$ARCH $INCLUDES" \
  LDFLAGS="-ggdb -L$LIB0 -L$LIB1 -L$LIB2 -L$LIB3 -L$LIB4 -L$LIB5 -Wl,-rpath=$HLIB0,-rpath-link=$LIB0,-rpath=$HLIB1,-rpath-link=$LIB1,-rpath=$HLIB2,-rpath-link=$LIB2,-rpath=$HLIB3,-rpath-link=$LIB3,-rpath=$HLIB4,-rpath-link=$LIB4,-rpath=$HLIB5,-rpath-link=$LIB5,-rpath-link=`pwd`/src/.libs" \
  MOC="`which moc` -qt=5" \
  UIC="`which uic` -qt=5" \
  RCC="`which rcc` -qt=5" \
  ../configure --host=$A --enable-mmal-avcodec --disable-vdpau --enable-vaapi --enable-debug --disable-lua --disable-chromecast --disable-wayland --enable-gles2 --disable-opencv --enable-dav1d --disable-aom --disable-qt

#  ./configure --host=$A --enable-debug --disable-lua --disable-qt --disable-vdpau --disable-chromecast --disable-wayland --disable-bluray --disable-opencv
#  ./configure --host=$A --enable-debug --disable-wayland




