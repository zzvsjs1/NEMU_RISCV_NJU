#!/bin/bash

convert slides.pdf \
  -sharpen "0x1.0" \
  -type truecolor -resize 800x600\! slides.bmp

mkdir -p $NAVY_HOME/fsimg/share/slides/
rm $NAVY_HOME/fsimg/share/slides/*
mv *.bmp $NAVY_HOME/fsimg/share/slides/
