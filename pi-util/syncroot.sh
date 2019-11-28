set -e

if [ "$1" == "" ]; then
  echo Usage: $0 \<src_dir\> [\<rootname\>]
  echo src_dir is a source for rsync so may contain m/c name.
  echo rootname will be set to \"raspian_stretch_pi1\" if missing
  echo e.g.: pi-util/syncroot.sh my-pi: raspian_stretch_pi1
  exit 1
fi

SYSROOT_NAME=$2
if [ "$SYSROOT_NAME" == "" ]; then
  SYSROOT_NAME=raspian_stretch_pi1
fi

DST_ROOT=`pwd`
DST=$DST_ROOT/sysroot/$SYSROOT_NAME-sysroot
SRC=$1

RPI_BASE=$DST_ROOT/..
TOOL_BASE=$RPI_BASE/tools
#FIRMWARE_BASE=$RPI_BASE/firmware4

echo Sync src:  $SRC
echo Sync dest: $DST

mkdir -p $DST/lib
mkdir -p $DST/opt
mkdir -p $DST/usr/share
mkdir -p $DST/usr/local/include
mkdir -p $DST/usr/local/lib

#ln -sf $FIRMWARE_BASE/opt/vc $DST/opt
rsync -rl $SRC/opt/vc $DST/opt
rsync -rl $SRC/lib/arm-linux-gnueabihf $DST/lib
rsync -rl --exclude "*/cups/backend/*" $SRC/usr/lib $DST/usr
rsync -rl $SRC/usr/include $DST/usr
rsync -rl $SRC/usr/share/pkgconfig $DST/usr/share
rsync -rl $SRC/usr/local/include $DST/usr/local
rsync -rl $SRC/usr/local/lib $DST/usr/local

rm $DST/usr/lib/arm-linux-gnueabihf/libpthread*
rm $DST/usr/lib/arm-linux-gnueabihf/libc.*

PUSHDIR=`pwd`
cd $DST/usr/lib/pkgconfig
ln -sf ../arm-linux-gnueabihf/pkgconfig/* .
cd $PUSHDIR
pi-util/rebase_liblinks.py $DST


