BASE=`pwd`/..
A=arm-linux-gnueabihf
TOOLS=$BASE/tools/arm-bcm2708/gcc-arm-8.2-2019.01-x86_64-arm-linux-gnueabihf
SYSROOT2=$BASE/tools/arm-bcm2708/sysroot-glibc-8.2-2019.01-x86_64-arm-linux-gnueabihf
#TOOLS=$BASE/tools/arm-bcm2708/gcc-linaro-6.3.1-2017.05-x86_64_arm-linux-gnueabihf
#SYSROOT2=$BASE/tools/arm-bcm2708/sysroot-glibc-linaro-2.23-2017.05-arm-linux-gnueabihf
SYSROOT=`pwd`/sysroot/raspian_stretch_pi1-sysroot
HLIB3=/lib/$A
HLIB1=/usr/lib/$A
HLIB2=/usr/lib
HLIB4=/opt/vc/lib
HLIB5=/usr/local/lib/$A

LIB2=$SYSROOT$HLIB2
LIB1=$SYSROOT$HLIB1
LIB3=$SYSROOT$HLIB3
LIB4=$SYSROOT$HLIB4
LIB5=$SYSROOT$HLIB5

#INCLUDES="-I$SYSROOT/usr/include -I$SYSROOT/usr/include/$A -I$SYSROOT/opt/vc/include -I$SYSROOT/usr/lib/arm-linux-gnueabihf/dbus-1.0/include -I$SYSROOT/usr/include/libdrm"
INCLUDES="-I$SYSROOT/usr/include -I$SYSROOT/usr/include/$A -I$SYSROOT/usr/local/include -I$SYSROOT/opt/vc/include -I$SYSROOT/usr/lib/arm-linux-gnueabihf/dbus-1.0/include"
#DEFINES="-ggdb -D__VCCOREVER__=0x04000000 -DQT_WARNING_DISABLE_DEPRECATED=\"\""
DEFINES="-ggdb -D__VCCOREVER__=0x04000000"
ARCH="-march=armv7-a -mfpu=neon-vfpv4"
PREFIX=$TOOLS/bin/$A-


PATH="$TOOLS/bin:$PATH" \
  PKG_CONFIG="pkg-config --define-prefix" \
  PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/local/lib/$A/pkgconfig:$LIB3/pkgconfig" \
  CC=${PREFIX}gcc \
  CFLAGS="$ARCH $INCLUDES" \
  CPP=${PREFIX}cpp \
  CPPFLAGS="$ARCH $INCLUDES $DEFINES" \
  CXX=${PREFIX}g++ \
  CXXFLAGS="$ARCH $INCLUDES" \
  LDFLAGS="-ggdb -L$TOOLS/lib -L$LIB1 -L$LIB2 -L$LIB3 -L$LIB4 -L$LIB5 -Wl,-rpath=$HLIB1,-rpath-link=$LIB1,-rpath=$HLIB2,-rpath-link=$LIB2,-rpath=$HLIB3,-rpath-link=$LIB3,-rpath=$HLIB4,-rpath-link=$LIB4,-rpath=$HLIB5,-rpath-link=$LIB5,-rpath-link=`pwd`/src/.libs" \
  MOC="`which moc` -qt=5" \
  UIC="`which uic` -qt=5" \
  RCC="`which rcc` -qt=5" \
  ./configure --host=$A --enable-mmal-avcodec --disable-vdpau --disable-libva --enable-debug --disable-lua --disable-chromecast --disable-wayland --enable-gles2 --disable-opencv --enable-dav1d --disable-aom

#  ./configure --host=$A --enable-debug --disable-lua --disable-qt --disable-vdpau --disable-chromecast --disable-wayland --disable-bluray --disable-opencv
#  ./configure --host=$A --enable-debug --disable-wayland




