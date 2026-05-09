# Per-Process Framebuffer Backing Store Design

## Context

Bad Apple in FCEUX reaches about 59-60 FPS when run as a bare-metal AM program on `riscv32-nemu`, but the same ROM reaches about 40 FPS when FCEUX runs as a Navy app under Nanos-lite.

The measured difference is not caused by ROM selection. Under Nanos-lite, `proc.c` launches:

```c
static char *const argv_fceux[] = { "/bin/fceux", "/share/games/nes/c.nes", NULL };
```

The relevant measurements were:

| Run mode | Game FPS | NEMU simulation frequency | Audio underruns |
| --- | ---: | ---: | --- |
| Bare-metal `fceux-am`, normal audio | 59-60 | about 1.44B instr/s | none |
| Nanos-lite, current framebuffer shadow | about 40 | about 0.98B instr/s | continuous |
| Nanos-lite, temporary shadow-capture disabled | 59-60 | about 1.04B instr/s | none after startup |

The temporary experiment only skipped `update_current_fb_shadow()` in `nanos-lite/src/device.c`. That shows the per-frame shadow copy is a primary bottleneck for full-screen video workloads.

## Current Design

Today the Nanos-lite video path is:

```text
FCEUX
  -> Navy libam AM_GPU_FBDRAW
  -> NDL_DrawRect()
  -> lseek(/dev/fb)
  -> write(/dev/fb)
  -> Nanos fs_write()
  -> Nanos fb_write()
  -> AM_GPU_FBDRAW
  -> NEMU VGA blit
  -> update_current_fb_shadow()
```

`fb_shadow` was added for foreground app switching, not for native SDL window resizing. The purpose is to preserve each foreground app's last visible screen when switching with F1/F2/F3. This avoids stale pixels when apps have different canvas sizes or when an event-driven app does not repaint immediately after becoming foreground.

The cost is high for Bad Apple. Each frame writes an 800 x 480 x 32-bit band:

```text
800 * 480 * 4 = 1,536,000 bytes
```

At 40 FPS, the kernel copies about 61,440,000 bytes per second just to maintain the shadow. This copy is executed by the emulated RISC-V CPU, so it directly competes with the game and audio producer.

## Goal

Keep the public app-facing behaviour compatible while removing the duplicate per-frame foreground copy.

Requirements:

- Do not require changes to FCEUX source code.
- Preserve `/dev/fb` write and `lseek` semantics for existing Navy apps.
- Preserve foreground switching correctness for PAL, ONScripter, FCEUX, and other foreground apps.
- Avoid app-specific logic such as "if process name is fceux".
- Keep the implementation understandable and documented enough to avoid future ABI confusion.
- Leave room for a future mmap/shared-framebuffer design.

## Chosen Approach

Use a per-process framebuffer backing store as the source of truth.

Each switchable foreground process owns one full-screen framebuffer buffer in Nanos-lite. A write to `/dev/fb` updates the current process's backing store. If the current process is also the selected foreground process, Nanos flushes the written rectangle to the physical AM/NEMU display. If the process is not foreground, Nanos updates only its backing store.

On F1/F2/F3 foreground switch, Nanos flushes the selected process's backing store to the physical display once.

This is closer to how real OS/window systems work: each app/window has owned pixel storage, and the display is the currently composed output. The kernel does not maintain a second always-updated mirror in addition to the displayed pixels.

## Public ABI

The current public ABI remains unchanged:

- Apps still open `/dev/fb`.
- Apps still use `lseek(fd, offset, SEEK_SET)` to select a framebuffer byte offset.
- Apps still use `write(fd, pixels, len)` to draw.
- `/proc/dispinfo` continues to report the physical display size.
- Navy `NDL_DrawRect()` and Navy `libam` can remain source-compatible with existing apps.

The backing-store behaviour is hidden inside Nanos-lite and platform libraries. Existing app source code should not need changes.

## Internal Semantics

For each foreground PCB slot, Nanos keeps:

- `fb_backing[slot]`: full physical framebuffer-sized pixel storage.
- Optional dirty tracking for future optimisation.

On `/dev/fb` write:

1. Convert `offset` and `len` into a framebuffer byte range and rectangle/spans.
2. Copy the userspace bytes into `fb_backing[current_slot]`.
3. If `current_slot == foreground_slot`, flush the same changed region from backing storage to `AM_GPU_FBDRAW`.
4. If the writer is not the foreground slot, do not flush to the physical display.

On foreground switch:

1. Change `foreground_slot`.
2. Restore the selected app's audio configuration as today.
3. Flush `fb_backing[foreground_slot]` to the display once.

The physical display becomes a cache of the selected process backing store, not the canonical owner of app pixels.

## Performance Notes

This design removes the current duplicate foreground work:

```text
old foreground write = NEMU blit from user buffer + copy user buffer into shadow
new foreground write = copy user buffer into backing + NEMU blit from backing
```

That still has one guest-side copy. It is not as fast as bare-metal AM, because the app is still going through syscalls, VME, and Nanos device code. However, it removes the extra copy that made Bad Apple fall from 60 FPS to 40 FPS.

If the first implementation copies into backing and then flushes from backing, it should be correct but may not be the absolute fastest possible path. A later optimisation can make full-width foreground writes flush directly from the user buffer and update backing with a faster or deferred policy, but only if it preserves switch restore correctness.

## Correctness Rules

- Background process writes must never appear on the display until that process becomes foreground.
- Foreground process writes must appear immediately, matching current `/dev/fb` behaviour.
- Switching to an app must display that app's latest backing store, even if the app does not repaint immediately.
- Backing store dimensions are physical display dimensions, not the app canvas dimensions, because `/dev/fb` offsets are physical framebuffer offsets.
- Smaller centred canvases remain correct because pixels outside the canvas are part of the process backing store.
- `NDL_OpenCanvas()` can still clear the framebuffer for a new process, but that clear should update the process backing store as well as the display if the process is foreground.

## Testing Plan

Performance tests:

- Run bare-metal Bad Apple:
  `make -C fceux-am ARCH=riscv32-nemu mainargs=c run -j12`
- Run Nanos-lite Bad Apple:
  `NAVY_HOME=$PWD/navy-apps make -C nanos-lite ARCH=riscv32-nemu run -j12`
- Compare game FPS printed by FCEUX, not only `NEMU_VGA_FPS`.
- Enable `NEMU_AUDIO_STATS=1` and verify underruns are gone or greatly reduced.

Correctness tests:

- Start Nanos-lite with FCEUX, PAL, and ONScripter loaded.
- Switch F1/F2/F3 repeatedly.
- Confirm each app's previous screen is restored before it repaints.
- Confirm smaller canvas apps do not show stale pixels from larger apps.
- Confirm audio format restoration still works across foreground switches.
- Run the existing static checks such as `scripts/codex-check-framebuffer-clear.sh` and `scripts/codex-check-timesharing.sh`.

Regression risks:

- If backing updates are skipped or deferred incorrectly, switching back to an app may show stale pixels.
- If foreground detection is wrong, a background process could draw over the selected app.
- If `/dev/fb` offset handling changes, MiniSDL dirty rectangles may draw in the wrong position.
- If the implementation assumes full-width writes only, smaller apps and partial updates may break.

## Future mmap Design

The better long-term ABI is a shared or mmap-style framebuffer:

- Nanos allocates per-process framebuffer backing storage.
- Userspace maps that storage directly.
- Userspace draws into it without `write()` copying.
- Userspace submits dirty rectangles through a flush command.
- Nanos flushes dirty regions for the foreground process and restores the selected process backing store on switch.

That model avoids most framebuffer copy overhead and is closer to modern OS/compositor designs. It should be implemented later as a new hidden Navy/Nanos path first, so app source can remain unchanged while `NDL_DrawRect()` and Navy `libam` use the improved ABI internally.

This design document intentionally keeps the first step compatible with existing `/dev/fb` users. The mmap design should be treated as a future extension, not a prerequisite for fixing the current 40 FPS bottleneck.
