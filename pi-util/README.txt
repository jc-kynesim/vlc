Release notes
=============

This version should run with gpu-mem=64 with the default switches. Having
said that this will only allow for 1 stream.  If you are playing >1 stream
(even transiently) then you will need more (say gpu_mem=128) and you will
need to set the --mmal-decoders option to the desired max number. The code
should give up cleanly if it cannot allocate a h/w video decoder and give
the stream to old-style ffmpeg decode, but as it stands in many cases it
thinks it has allocated a decoder cleanly only to find that it fails when
it tries to use it.

Needs firmware from "Sep 13 2016 17:01:56" or later to work properly
("vcgencmd version" will give the date).

There are a few command-line switches - in general you shouldn't use
them!


Decode and resizer options
--------------------------

--mmal-decode-opaque     Set the decoder to use opaque frames between
decoder and resizer.  This should be faster than i420 but doesn't work
with old firmware.  This is the default with newer firmware (>=
2016-11-01). (see --mmal-decode-i420)

--mmal-decode-i420       Set the decoder to use I420 frames between
decoder and resizer.  This generates an unnecessary conversion but works
with all firmware.  This is the default with older firmware (<
2016-11-01). (see --mmal-decode-opaque)

--mmal-low-delay         Force "low-delay" mode on the decoder pipe.  This
reduces the number of buffered ES frames before the decoder.  It isn't
exactly low-delay but is definitely lower than otherwise.  May have a
slight performance penalty and increase the risk of stuttering.  This mode
will be automatically set by Chrome for some streams.

--mmal-resize-isp        Use ISP resize rather than resizer.  Is noticably
faster but requires --mmal-frame-copy or --mmal-zero-copy and newer
firmware.  This is the default with newer firmware  (>= 2016-11-01) and
enough gpu memory to support --mmal-frame-copy.

--mmal-resize-resizer    Use resizer rather than ISP. Slower than ISP
resize but supports older firmware and --mmal-slice-copy which may be
needed if GPU memory is very limited (as will be the case on a Pi1 with a
default setup).

Copy-modes
----------

These are modes for getting frames out of mmal.  Current default is
--mmal-frame-copy if --mmal-resize-isp is the default resizer or it looks
like the firmware doesn't support --mmal-slice-copy otherwise
--mmal-slice-copy is the default. Explicit use of a copy mode option will
override the default regardless of whether or not we think the firmware
supports the selected option.  Only use one of of these flags.

--mmal-zero-copy         Pass gpu frames directly to chrome.  Chrome
buffers some frames and stalls if it doesn't get them. So this option
needs 6+ gpu frames allocated.  This is now a legacy and testing option as
--mmal-frame-copy is faster and you probably want to have gpu_mem=192 if
you are going to use it. Default frame-buffers = 6 (8M each)

--mmal-frame-copy        Copy frame at a time out of mmal to chrome.
Currently the fastest option.  Needs 2+ gpu frames for plausible
performance. Default frame-buffers = 2 (8M each).  You probably want
gpu-mem=80 for 1 decoder with this option.

--mmal-slice-copy        Copy frames out in 16-line slices.  Has the
lowest memory overhead, but the highest CPU load.  If this is selected
then --mmal-frame-buffers is the number of slice buffers. Default frame
buffers = 16 (~122k each).

Misc options
------------

--enable-logging=stderr This is a standard option for chrome but worth
noting as the mmal code will print out its interpretation of the command
line options passed to it along with how much GPU memory it has detected
and the firmware date.

--pi-patch-version       Print out the versions of Chromium and Pi
patches.  Chrome will then terminate

--mmal-decoders=<n>      Set the number of mmal decoders we wil try to
create simultainiously. Default=1. If this number is exceeded then decoder
init will fail and chrome will fallback to ffmpeg decode.  There is no
panalty for setting this to a large number if you wish to have "unlimited"
decoders.  However if it is set too big and there isn't the gpu mem to
satisfy the requirements of the decode it may fail cleanly and revert to
software (ffmpeg) decode or init may appear to succeed and decode then
fails in an undefined manner.

--mmal-frame-buffers=<n> Set the number of gpu "frame" buffers (see
--mmal-xxx-copy). Change with care.

--mmal-red-pixel         Puts a red square in the top left of a frame
decoded by mmal so you can tell that it is active.  Doesn't work if
zero-copy is set.
