set -e

FFNAME=libffmpeg_chrome.so.66
LIBROOT=/usr/lib/arm-linux-gnueabihf

if [ ! -d $LIBROOT ]; then
  echo Can\'t find $LIBROOT
  echo Are you sure you are running this on a Pi?
  exit 1
fi

echo Copying $FFNAME from armv6/7 to $LIBROOT/...

cp out/armv7/$FFNAME /tmp
sudo cp /tmp/$FFNAME $LIBROOT/neon/vfp
cp out/armv6/$FFNAME /tmp
sudo cp /tmp/$FFNAME $LIBROOT


