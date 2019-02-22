DSTBASE=/usr/lib/arm-linux-gnueabihf
DSTPLUGINS=$DSTBASE/vlc/plugins
DSTDIR=$DSTPLUGINS/mmal
sudo mkdir -p $DSTDIR
sudo cp modules/hw/mmal/.libs/*.so $DSTDIR/
#sudo cp modules/.libs/libxcb_x11_plugin.so $DSTPLUGINS/video_output/
#sudo cp src/.libs/libvlccore.so.9.0.0 $DSTBASE/
vlc --no-plugins-cache $*
