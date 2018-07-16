# Install set to build appropriate root on a clean pi

sudo apt-get install \
  libprotobuf-dev\
  libxcb-1


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
libgnutls28-dev.h \
gtk+-3.0 \
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
libxcb1-dev \
libxcb-shm0-dev \
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
