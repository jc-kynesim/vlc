set -e

NOTAG=
if [ "$1" == "--notag" ]; then
  shift
  NOTAG=1
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

PATCHFILE=../vlc-$VERSION-$1-001.patch

if [ $NOTAG ]; then
  echo Not tagged
else
  # Only continue if we are all comitted
  git diff --name-status --exit-code

  PATCHTAG=pi/$VERSION/$1
  echo Tagging: $PATCHTAG

  git tag $PATCHTAG
fi

# We seem to sometimes gain add
echo Generating patch: $PATCHFILE
REFNAME=refs/heads/$VERSION
git diff $REFNAME -- \
 configure.ac \
 include \
 modules/Makefile.am \
 modules/codec \
 modules/hw/drm \
 modules/hw/mmal \
 modules/gui/qt/qt.cpp \
 modules/video_output/Makefile.am \
 modules/video_output/drmu \
 modules/video_output/opengl \
 src/input \
 src/misc \
 src/video_output \
 > $PATCHFILE
git diff $REFNAME -- modules/video_chroma/chain.c > ../vlc-$VERSION-$1-002-chain.patch
git diff $REFNAME -- bin/vlc.c > ../vlc-$VERSION-$1-003-vlc.patch
git diff $REFNAME -- modules/access/srt.c modules/access_output/srt.c > ../vlc-$VERSION-$1-004-srt.patch
git diff $REFNAME -- modules/video_output/caca.c > ../vlc-$VERSION-$1-005-caca.patch

#echo Copying patch to arm-build
#scp $PATCHFILE john@arm-build:patches/0002-vlc-3.0.6-mmal_test_4.patch
