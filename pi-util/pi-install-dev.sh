# Install set to build appropriate root on a clean pi

sudo apt-get install \
  libprotobuf-dev \
  libepoxy-dev \
  libavutil-dev \
  libavcodec-dev \
  libavformat-dev \
  libswscale-dev \
  libva-dev \
  libpostproc-dev \
  libtwolame-dev \
  liba52-dev \
  libflac-dev \
  libmpeg2-4-dev \
  libass-dev \
  libaribb24-dev \
  libzvbi-dev \
  libkate-dev \
  libogg-dev \
  libdca-dev \
  libxcb-keysyms1-dev \
  libsdl2-dev \
  librsvg2-dev \
  libsystemd-dev \
  libarchive-dev \
  libnfs-dev \
  libssh2-1-dev \
  libopencv-dev \
  libsmbclient-dev \
  libmodplug-dev \
  libshine-dev \
  libvorbis-dev \
  libxml2-dev \
comerr-dev \
libasound2-dev \
libatk1.0-dev \
libcap-dev \
libcups2-dev \
libexif-dev \
libffi-dev \
libgconf2-dev \
libgl1-mesa-dev \
libgnome-keyring-dev \
libgnutls28-dev \
libidn11-dev \
libjpeg-dev \
libkrb5-dev \
libnspr4-dev \
libnss3-dev \
libpam0g-dev \
libpango1.0-dev \
libpci-dev \
libpcre3-dev \
libssl-dev \
libudev-dev \
libx11-dev \
libx11-xcb-dev \
libxcb1-dev \
libxcb-shm0-dev \
libxcb-composite0-dev \
libxcb-xv0-dev \
libxss-dev \
libxt-dev \
libxtst-dev \
mesa-common-dev

# Pulse (hopefully) disabled
# libpulse-dev \

# Obviously replace paths appropriately below
# Now run pi-util/syncroot.sh on the compile m/c to grab the appropriate
# bits of the root and fix up the paths.
# e.g. ON COMPILE M/C in src dir:
# pi-util/syncroot.sh my-pi: raspian_jessie_pi1
