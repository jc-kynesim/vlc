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
REF=$1

VERSION=`awk '/AC_INIT/{match($0,/[0-9]+(\.[0-9]+)+/);print substr($0,RSTART,RLENGTH)}' configure.ac`
if [ "$VERSION" == "" ]; then
  echo Can\'t find version in configure.ac
  exit 1
fi

if [ $NOTAG ]; then
  echo Not tagged
else
  # Only continue if we are all comitted
  git diff --name-status --exit-code

  PATCHTAG=pi/$VERSION/$REF
  echo Tagging: $PATCHTAG

  git tag $PATCHTAG
fi

DSTDIR=..
PATCHNAME=vlc-$VERSION-$REF
DIFFBASE=$DSTDIR/$PATCHNAME
ZIPNAME=$PATCHNAME-patch.zip

# We seem to sometimes gain add
echo Generating patches in: $DSTDIR/$ZIPNAME
REFNAME=refs/tags/$VERSION
git diff $REFNAME -- \
 configure.ac \
 include \
 modules/Makefile.am \
 modules/audio_filter \
 modules/audio_output \
 modules/codec \
 modules/gui/qt/qt.cpp \
 modules/hw/drm \
 modules/hw/mmal \
 modules/video_output/Makefile.am \
 modules/video_output/drmu \
 modules/video_output/opengl \
 modules/video_output/wayland \
 src/audio_output \
 src/input \
 src/misc \
 > $DIFFBASE-001-rpi.patch
git diff $REFNAME -- modules/video_chroma/chain.c > $DIFFBASE-002-chain.patch
git diff $REFNAME -- bin/vlc.c > $DIFFBASE-003-vlc.patch
git diff $REFNAME -- modules/video_output/caca.c > $DIFFBASE-004-caca.patch
git diff $REFNAME -- modules/gui/qt/components/interface_widgets.* > $DIFFBASE-005-qt-wayland.patch
cd $DSTDIR
zip -m $ZIPNAME $PATCHNAME-*.patch

#echo Copying patch to arm-build
#scp $PATCHFILE john@arm-build:patches/0002-vlc-3.0.6-mmal_test_4.patch
