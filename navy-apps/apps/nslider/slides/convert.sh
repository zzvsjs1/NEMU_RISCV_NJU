#!/bin/bash

# The runtime viewer expects numbered 800x600 BMP pages in the Navy ramdisk.
# This script is an offline asset step: ImageMagick handles PDF rendering on
# the host, then the generated bitmaps become plain files under /share/slides.
# Keeping conversion here avoids teaching the Navy app about PDF tooling or
# host paths; it only depends on the same logical /share filesystem as PAL/ONS.
convert slides.pdf \
  -sharpen "0x1.0" \
  -type truecolor -resize 800x600\! slides.bmp

mkdir -p $NAVY_HOME/fsimg/share/slides/
# The destination directory is regenerated as a complete slide set so stale
# pages from an older PDF do not remain visible to nslider's fixed path pattern.
rm $NAVY_HOME/fsimg/share/slides/*
mv *.bmp $NAVY_HOME/fsimg/share/slides/
