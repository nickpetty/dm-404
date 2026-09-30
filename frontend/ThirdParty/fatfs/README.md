FatFs R0.15a by ChaN (http://elm-chan.org/fsw/ff/), used for the SP-404's
drive images. The licence is in the header of each file (BSD-style: keep the
copyright notice).

Unchanged except `ffconf.h`: FF_USE_MKFS 1, FF_USE_CHMOD 1, FF_USE_LABEL 1,
FF_CODE_PAGE 437, FF_USE_LFN 3, FF_LFN_UNICODE 2 (UTF-8), FF_FS_EXFAT 1.
The disk I/O layer (an image file) is in frontend/Source/FatImage.cpp.
