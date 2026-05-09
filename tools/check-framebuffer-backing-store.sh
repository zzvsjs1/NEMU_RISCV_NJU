#!/usr/bin/env bash
set -euo pipefail

rg -q "NEMU_VGACTL_CAPTURE_DST" abstract-machine/am/src/platform/nemu/include/nemu.h
rg -q "vga_capture_to_guest" nemu/src/device/vga.c
rg -q "fb_capture_slot_if_stale" nanos-lite/src/device.c
rg -q "fb_backing_stale" nanos-lite/src/device.c

if rg -q "update_current_fb_shadow\\(buf, offset, len\\)" nanos-lite/src/device.c; then
  echo "foreground hot path still calls update_current_fb_shadow" >&2
  exit 1
fi

if ! rg -q "device_capture_foreground_before_switch\\(\\);" nanos-lite/src/proc.c; then
  echo "foreground switching is not capturing the old foreground before switch" >&2
  exit 1
fi
