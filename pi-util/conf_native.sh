set -e
BASE=`pwd`
OUT_BASE=$BASE/out

DO_BOOTSTRAP=
DO_MAKE=
DO_INSTALL=
SUDO_INSTALL=
DO_CONFIGURE=1
USR_PREFIX=

while [ "$1" != "" ] ; do
    case $1 in
        --make)
            DO_MAKE=1
            DO_CONFIGURE=
            ;;
        --install)
            DO_INSTALL=1
            DO_MAKE=1
            DO_CONFIGURE=
            ;;
        --bootstrap)
            DO_BOOTSTRAP=1
            ;;
	--usr)
	    USR_PREFIX=/usr
	    SUDO_INSTALL=sudo
	    ;;
        *)
            echo "Usage $0: [--bootstrap] [--make|--install] [--usr]"
            echo "  bootstrap Clean <build dir> then bootstrap before configure"
            echo "            Will happen automatically if already clean"
            echo "  make      Do make after configure"
            echo "  install   Do make and install after configure"
	    echo "  usr       Set install dir to /usr"
	    echo "            Default is <build dir>/install for testing"
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
elif [ "$MC" == "x86_64" ]; then
    A=x86_64-linux-gnu
    ARM=x86_64
else
  echo "Unknown machine name: $MC"
  exit 1
fi
OUT=$OUT_BASE/$ARM-`lsb_release -sc`-rel

if [ $DO_BOOTSTRAP ]; then
    echo "==== Bootstrapping & cleaning $OUT"
    rm -rf $OUT
    ./bootstrap
fi

if [ ! -f $OUT/Makefile ]; then
    DO_CONFIGURE=1
fi

if [ "$USR_PREFIX" == "" ]; then
    USR_PREFIX=$OUT/install
fi
LIB_PREFIX=$USR_PREFIX/lib/$A
INC_PREFIX=$USR_PREFIX/include/$A

echo "==== Configuring in $OUT"
mkdir -p $OUT
# Nothing under here need worry git - including this .gitignore!
echo "**" > $OUT_BASE/.gitignore

cd $OUT
if [ $DO_CONFIGURE ]; then
    $BASE/configure \
     --build=$A \
     --prefix=$USR_PREFIX\
     --libdir=$LIB_PREFIX\
     --includedir=$INC_PREFIX\
     --disable-vdpau\
     --enable-wayland\
     --enable-gles2\
     $CONF_MMAL
    echo "==== Configured in $OUT"
fi

if [ $DO_MAKE ]; then
    echo "==== Making $OUT"
    make -j8
    echo "==== Made $OUT"
fi

if [ $DO_INSTALL ]; then
    echo "==== Installing to $USR_PREFIX"
    $SUDO_INSTALL make -j8 install
    echo "==== Installed in $USR_PREFIX"
fi

