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
PATCHTMP=/tmp/vlc-patches
rm -rf $PATCHTMP
mkdir -p $PATCHTMP
git format-patch --output-directory $PATCHTMP $REFNAME
zip -j $DSTDIR/$ZIPNAME $PATCHTMP/*
