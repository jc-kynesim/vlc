sudo mkdir -p /usr/lib/arm-linux-gnueabihf/vlc/plugins/hw/mmal
sudo cp --preserve=links src/.libs/libvlccor*.so /usr/lib/arm-linux-gnueabihf/
sudo cp modules/hw/mmal/.libs/*.so /usr/lib/arm-linux-gnueabihf/vlc/plugins/hw/mmal/
vlc --no-plugins-cache $*
