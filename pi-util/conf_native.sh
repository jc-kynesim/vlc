set -e
BASE=`pwd`

CONF_MMAL=--disable-mmal

MC=`uname -m`
if [ "$MC" == "armv7l" ]; then
#  CONF_MMAL=--enable-mmal-avcodec
  CONF_MMAL=
  ARM=armv7
elif [ "$MC" == "aarch64" ]; then
  ARM=arm64
else
  echo "Unkown machine name: $MC"
  exit 1
fi
OUT=$BASE/out/$ARM-`lsb_release -sc`-rel

echo "Configuring in $OUT"
mkdir -p $OUT
# Nothing under here need worry git - including this .gitignore!
echo "**" > $BASE/out/.gitignore

cd $OUT
$BASE/configure  --disable-vdpau --enable-gles2 $CONF_MMAL
echo "Configured in $OUT"

