set -e

NOTAG=
if [ "$1" == "--notag" ]; then
  shift
  NOTAG=1
fi

if [ "$1" == "" ] || [ "$2" != "" ]; then
  echo Usage: $0 [--notag] \<patch_tag\>
  echo e.g.: $0 mmal_4
  exit 1
fi
REF=$1

CONFIG_VERSION=`awk '/AC_INIT/{match($0,/[0-9]+(\.[0-9]+)+/);print substr($0,RSTART,RLENGTH)}' configure.ac`
if [ "$CONFIG_VERSION" == "" ]; then
  echo Config version not found
  exit 1
fi

# Config substitution here really should have escaped '.'s but it isn't
# really worth it. There is little chance they will cause false +ves
BRANCH=$(git branch --show-current)
BRANCH_VERSION=$(echo $BRANCH | awk "{match(\$0, /test\\/(${CONFIG_VERSION}.+)\\/.+/, a); print a[1];}")

if [ "$BRANCH_VERSION" == "" ]; then
  echo Branch $BRANCH not expected format \(test/${CONFIG_VERSION}*/*\)
  exit 1
fi

VERSION=$BRANCH_VERSION
echo VERSION=$VERSION

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
