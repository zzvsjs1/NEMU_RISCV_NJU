/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Xodnizel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <klib-macros.h>
#include "sdl.h"
#include "../common/vidblit.h"
#include "../../fceu.h"
#include "../../version.h"
#include "../../video.h"

#include "../../utils/memory.h"

#include "sdl-icon.h"
#include "dface.h"

#include "sdl-video.h"
#include "../../config.h"

static int s_srendline;
//static int s_erendline;
static const int s_tlines = NES_BASE_HEIGHT;
static int s_inited;

#define NWIDTH NES_BASE_WIDTH

static int s_paletterefresh;

int KillVideo()
{
    // return failure if the video system was not initialized
    if (s_inited == 0)
        return -1;

    // if the rest of the system has been initialized, shut it down
    // shut down the system that converts from 8 to 16/32 bpp
    KillBlitToHigh();

    s_inited = 0;
    return 0;
}

void FCEUD_VideoChanged()
{
    PAL = 0; // NTSC and Dendy
}

/**
 * Attempts to initialize the graphical video display.  Returns 0 on
 * success, -1 on failure.
 */
int InitVideo(FCEUGI *gi)
{
    FCEUI_printf("Initializing video...");

    // check the starting, ending, and total scan lines
    //FCEUI_GetCurrentVidSystem(&s_srendline, &s_erendline);
    //s_tlines = s_erendline - s_srendline + 1;

    s_inited = 1;

    FCEUI_SetShowFPS(true);

    s_paletterefresh = 1;

    // if using more than 8bpp, initialize the conversion routines
    InitBlitToHigh(4, 0x00ff0000, 0x0000ff00, 0x000000ff, 0, 0, 0);
    return 0;
}

static struct
{
    uint8 r, g, b, unused;
} s_psdl[256];

static uint32_t *screen_frame;
static int screen_frame_w;
static int screen_frame_h;

/**
 * Sets the color for a particular index in the palette.
 */
void FCEUD_SetPalette(uint8 index,
                      uint8 r,
                      uint8 g,
                      uint8 b)
{
    s_psdl[index].r = r;
    s_psdl[index].g = g;
    s_psdl[index].b = b;

    s_paletterefresh = 1;
}

/**
 * Pushes the given buffer of bits to the screen.
 */
void BlitScreen(uint8 *XBuf)
{
    // refresh the palette if required
    if (s_paletterefresh)
    {
        SetPaletteBlitToHigh((uint8 *)s_psdl);
        s_paletterefresh = 0;
    }

    // XXX soules - not entirely sure why this is being done yet
    XBuf += s_srendline * 256;

    // XXX soules - again, I'm surprised SDL can't handle this
    // perform the blit, converting bpp if necessary
    //Blit8ToHigh(XBuf, (uint8 *)canvas, NWIDTH, s_tlines, NWIDTH * 4, 1, 1);

    static uint32_t canvas_line[NWIDTH];
    int i;
#ifdef HAS_GUI
    AM_GPU_CONFIG_T gpu = io_read(AM_GPU_CONFIG);
    int scale = gpu.width / NES_BASE_WIDTH;
    int scale_y = gpu.height / NES_BASE_HEIGHT;
    if (scale > scale_y)
        scale = scale_y;
    if (scale > NES_MAX_SCALE)
        scale = NES_MAX_SCALE;
    if (scale < 1)
        scale = 1;

    int draw_w = NES_BASE_WIDTH * scale;
    int draw_h = NES_BASE_HEIGHT * scale;
    int frame_w = gpu.width;
    int frame_h = draw_h;
    int x = (gpu.width - draw_w) / 2;
    int y = (gpu.height - draw_h) / 2;

    if (screen_frame == NULL || screen_frame_w != frame_w || screen_frame_h != frame_h)
    {
        screen_frame = (uint32_t *)FCEU_malloc(frame_w * frame_h * sizeof(uint32_t));
        screen_frame_w = frame_w;
        screen_frame_h = frame_h;
    }

    for (i = 0; i < s_tlines; i++, XBuf += NWIDTH)
    {
        Blit8ToHigh(XBuf, (uint8 *)canvas_line, NWIDTH, 1, NWIDTH * 4, 1, 1);
        if (scale == 1)
        {
            memcpy(screen_frame + i * frame_w + x, canvas_line, NWIDTH * sizeof(uint32_t));
        }
        else
        {
            /*
       * NES pixels are low resolution by design.  Integer nearest-neighbour
       * scaling keeps the image sharp.  Build a full-width NES image band,
       * then publish it in one full-width draw; this lets NDL and nanos-lite
       * take their fast contiguous framebuffer path instead of hundreds of
       * row-sized syscalls per emulated frame.  The top and bottom borders stay
       * untouched after the initial black restore, so each frame moves only the
       * visible NES band instead of the whole 800x600 display.
       */
            uint32_t *dst0 = screen_frame + i * scale * frame_w + x;
            for (int px = 0; px < NWIDTH; px++)
            {
                for (int sx = 0; sx < scale; sx++)
                {
                    dst0[px * scale + sx] = canvas_line[px];
                }
            }
            for (int sy = 1; sy < scale; sy++)
            {
                memcpy(dst0 + sy * frame_w, dst0, draw_w * sizeof(uint32_t));
            }
        }
    }
    io_write(AM_GPU_FBDRAW, 0, y, screen_frame, frame_w, frame_h, true);
#else
    printf("\033[0;0H");
    for (i = 0; i < s_tlines; i += 4, XBuf += NWIDTH * 4)
    {
        Blit8ToHigh(XBuf, (uint8 *)canvas_line, NWIDTH, 1, NWIDTH * 4, 1, 1);
        for (int x = 0; x < NWIDTH; x += 2)
        {
            uint32_t color = canvas_line[x];
            const char *list = "o. *O0@#";
            char c = list[color / 0x222222u];
            putch(c);
        }
        putch('\n');
    }
#endif
}
