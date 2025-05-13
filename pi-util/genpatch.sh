set -e

NOPATCH=
if [ "$1" == "--notag" ]; then
  shift
  NOPATCH=1
fi

if [ "$1" == "" ]; then
  echo Usage: $0 [--notag] \<patch_tag\>
  echo e.g.: $0 mmal_4
  exit 1
fi

VERSION=`awk '/AC_INIT/{match($0,/[0-9]+(\.[0-9]+)+/);print substr($0,RSTART,RLENGTH)}' configure.ac`
if [ "$VERSION" == "" ]; then
  echo Can\'t find version in configure.ac
  exit 1
fi

PATCHFILE=../vlc-$VERSION-$1.patch

if [ $NOPATCH ]; then
  echo Not tagged
else
  # Only continue if we are all comitted
  git diff --name-status --exit-code

  PATCHTAG=pi/$VERSION/$1
  echo Tagging: $PATCHTAG

  git tag $PATCHTAG
fi
echo Generating patch: $PATCHFILE
git diff $VERSION -- modules/hw/mmal modules/video_output/opengl src/misc include src/video_output src/input configure.ac > $PATCHFILE
git diff $VERSION -- modules/video_chroma/chain.c > ../vlc-$VERSION-$1-chain.patch
git diff $VERSION -- bin/vlc.c > ../vlc-$VERSION-$1-vlc.patch

#echo Copying patch to arm-build
#scp $PATCHFILE john@arm-build:patches/0002-vlc-3.0.6-mmal_test_4.patch
