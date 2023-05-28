set -e
BASE=`pwd`

DO_BOOTSTRAP=
DO_MAKE=

while [ "$1" != "" ] ; do
    case $1 in
	--make)
	    DO_MAKE=1
	    ;;
	--bootstrap)
	    DO_BOOTSTRAP=1
	    ;;
	*)
	    echo "Usage $0: [--bootstrap] [--make]"
	    echo "  bootstrap Do bootstrap before configure"
	    echo "            (will always bootstrap if clean)"
	    echo "  make      Do make after configure"
	    exit 1
	    ;;
    esac
    shift
done

if [ ! -f $BASE/configure ]; then
  echo "configure not found - will bootstrap"
  DO_BOOTSTRAP=1
fi

CONF_MMAL=--disable-mmal

# uname -m gives kernel type which may not have the same
# 32/64bitness as userspace :-( getconf shoudl provide the answer
# but use uname to check we are on the right processor
MC=`uname -m`
LB=`getconf LONG_BIT`
if [ "$MC" == "armv7l" ] || [ "$MC" == "aarch64" ]; then
  if [ "$LB" == "32" ]; then
    #  CONF_MMAL=--enable-mmal-avcodec
    CONF_MMAL=
    A=arm-linux-gnueabihf
    ARM=armv7
  elif [ "$LB" == "64" ]; then
    A=aarch64-linux-gnu
    ARM=arm64
  else
    echo "Unknown LONG_BIT name: $LB"
    exit 1
  fi
else
  echo "Unknown machine name: $MC"
  exit 1
fi
OUT=$BASE/out/$ARM-`lsb_release -sc`-rel

USR_PREFIX=$OUT/install
LIB_PREFIX=$USR_PREFIX/lib/$A
INC_PREFIX=$USR_PREFIX/include/$A

if [ $DO_BOOTSTRAP ]; then
  echo "Bootstrapping"
  ./bootstrap
fi

echo "Configuring in $OUT"
mkdir -p $OUT
# Nothing under here need worry git - including this .gitignore!
echo "**" > $BASE/out/.gitignore

cd $OUT
$BASE/configure \
 --build=$A \
 --prefix=$USR_PREFIX\
 --libdir=$LIB_PREFIX\
 --includedir=$INC_PREFIX\
 --disable-vdpau\
 --enable-wayland\
 --enable-gles2\
 $CONF_MMAL
echo "Configured in $OUT"

if [ $DO_MAKE ]; then
  make -j8
fi
