DSTDIR=/usr/lib/arm-linux-gnueabihf/vlc/plugins/mmal
sudo mkdir -p $DSTDIR
sudo cp modules/hw/mmal/.libs/*.so $DSTDIR/
vlc --no-plugins-cache $*
