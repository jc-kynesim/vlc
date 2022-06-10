set -e
BASE=`pwd`

MC=`uname -m`
if [ "$MC" == "armv7l" ]; then
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
$BASE/configure  --disable-vdpau --enable-gles2 --enable-mmal-avcodec
echo "Configured in $OUT"

