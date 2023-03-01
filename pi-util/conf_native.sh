set -e
BASE=`pwd`

CONF_MMAL=--disable-mmal

MC=`uname -m`
if [ "$MC" == "armv7l" ]; then
#  CONF_MMAL=--enable-mmal-avcodec
  CONF_MMAL=
  A=arm-linux-gnueabihf
  ARM=armv7
elif [ "$MC" == "aarch64" ]; then
  A=aarch64-linux-gnu
  ARM=arm64
else
  echo "Unknown machine name: $MC"
  exit 1
fi
OUT=$BASE/out/$ARM-`lsb_release -sc`-rel

USR_PREFIX=$OUT/install
LIB_PREFIX=$USR_PREFIX/lib/$A
INC_PREFIX=$USR_PREFIX/include/$A

echo "Configuring in $OUT"
mkdir -p $OUT
# Nothing under here need worry git - including this .gitignore!
echo "**" > $BASE/out/.gitignore

cd $OUT
$BASE/configure \
 --prefix=$USR_PREFIX\
 --libdir=$LIB_PREFIX\
 --includedir=$INC_PREFIX\
 --disable-vdpau\
 --enable-wayland\
 --enable-gles2\
 $CONF_MMAL
echo "Configured in $OUT"

