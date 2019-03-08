set -e

BASETAG=3.0.6

PATCHFILE=../vlc-${BASETAG}-$1.patch

if [ "$1" == "" ]; then
  echo Usage: $0 \<patch_tag\>
  echo e.g.: $0 mmal_4
  exit 1
fi

git diff $BASETAG -- modules/hw/mmal src/misc include configure.ac > $PATCHFILE

scp $PATCHFILE john@arm-build:patches/0002-vlc-3.0.6-mmal_test_4.patch
