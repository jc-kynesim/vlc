set -e
BASE=`pwd`

MC=`dpkg  --print-architecture`
if [ "$MC" == "armhf" ]; then
  ARM=armv7
elif [ "$MC" == "arm64" ]; then
  ARM=arm64
else
  echo "Unknown machine name: $MC"
  exit 1
fi
OUT=$BASE/out/$ARM-`lsb_release -sc`-rel

echo "Configuring in $OUT"
mkdir -p $OUT
cd $OUT
LIBS=-latomic $BASE/configure  --disable-vdpau --enable-gles2 --disable-mmal
echo "Configured in $OUT"

