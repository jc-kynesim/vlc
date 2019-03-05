set -e

if [ "$1" == "" ]; then
  echo Usage: $0 \<patch_tag\>
  echo e.g.: $0 mmal_4
  exit 1
fi

VERSION=`awk '/AC_INIT/{match($0,/[0-9]\.[0-9]\.[0-9]/);print substr($0,RSTART,RLENGTH)}' configure.ac`
if [ "$VERSION" == "" ]; then
  echo Can\'t find version in configure.ac
  exit 1
fi

git diff --name-status --exit-code

PATCHTAG=${VERSION}-$1
PATCHFILE=../vlc-$PATCHTAG.patch

git tag pi/$PATCHTAG
git diff $VERSION -- modules/hw/mmal src/misc include configure.ac > $PATCHFILE

scp $PATCHFILE john@arm-build:patches/0002-vlc-3.0.6-mmal_test_4.patch
