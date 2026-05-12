#include <SDL.h>
#include <SDL_image.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TRACE(msg) \
    do \
    { \
        printf("%s\n", msg); \
        fflush(stdout); \
    } while (0)

static uint32_t timer_hits = 0;

static uint32_t timer_once(uint32_t interval, void *param)
{
    uint32_t *hits = (uint32_t *)param;

    /*
   * Returning zero asks SDL to remove this timer.  This checks the exact
   * one-shot style ONScripter uses for script wait events.
   */
    (*hits)++;
    return 0;
}

static void check_memory_rwops(void)
{
    /*
   * ONScripter uses SDL_RWops as a common abstraction over memory and files.
   * These assertions protect seek, tell, and object-count read semantics that
   * image and script loaders rely on.
   */
    const char text[] = "abcdef";
    char out[4] = {};
    SDL_RWops *rw = SDL_RWFromMem((void *)text, sizeof(text) - 1);
    assert(rw != NULL);
    assert(SDL_RWsize(rw) == 6);
    assert(SDL_RWread(rw, out, 1, 3) == 3);
    assert(out[0] == 'a' && out[1] == 'b' && out[2] == 'c');
    assert(SDL_RWseek(rw, -2, RW_SEEK_CUR) == 1);
    assert(SDL_RWread(rw, out, 2, 1) == 1);
    assert(out[0] == 'b' && out[1] == 'c');
    assert(SDL_RWclose(rw) == 0);
}

static void check_file_rwops(void)
{
    /*
   * Use a fixed logical /share file rather than a host-relative path.  This is
   * the contract that lets the same test exercise native ramdisk and NEMU disk
   * image setups without knowing which backing store provided the bytes.
   */
    SDL_RWops *rw = SDL_RWFromFile("/share/files/num", "r");
    char out[5] = {};
    assert(rw != NULL);
    assert(SDL_RWsize(rw) == 5000);
    assert(SDL_RWread(rw, out, 1, 4) == 4);
    assert(out[0] != '\0' && out[1] != '\0' && out[2] != '\0' && out[3] != '\0');
    assert(SDL_RWclose(rw) == 0);
}

static void check_events(void)
{
    /*
   * Public pushed events must round-trip through miniSDL's queue before any
   * NDL polling is involved. This protects scripted wait/event paths that
   * inject SDL_USEREVENT directly.
   */
    SDL_Event pushed = {};
    SDL_Event got = {};
    pushed.type = SDL_USEREVENT;
    pushed.user.code = 7;
    pushed.user.data1 = (void *)0x1234;

    assert(SDL_PushEvent(&pushed) == 0);
    assert(SDL_PeepEvents(&got, 1, SDL_PEEKEVENT, SDL_EVENTMASK(SDL_USEREVENT)) == 1);
    assert(got.type == SDL_USEREVENT && got.user.code == 7);
    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_USEREVENT)) == 1);
    assert(got.type == SDL_USEREVENT && got.user.data1 == (void *)0x1234);
    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_USEREVENT)) == 0);
}

static void check_mouse_events(void)
{
    /*
   * ONScripter reads both queued mouse events and the current mouse state.  This
   * covers the edge where a button event updates coordinates, then a later motion
   * event replaces button state; the cursor patch depends on those values staying
   * coherent.
   */
    SDL_Event pushed = {};
    SDL_Event got = {};

    pushed.type = SDL_MOUSEMOTION;
    pushed.motion.x = 11;
    pushed.motion.y = 22;
    pushed.motion.state = SDL_BUTTON(SDL_BUTTON_LEFT);
    assert(SDL_PushEvent(&pushed) == 0);
    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_MOUSEMOTION)) == 1);
    assert(got.motion.x == 11 && got.motion.y == 22);
    assert(got.motion.state == SDL_BUTTON(SDL_BUTTON_LEFT));

    pushed = (SDL_Event){};
    pushed.type = SDL_MOUSEBUTTONDOWN;
    pushed.button.button = SDL_BUTTON_RIGHT;
    pushed.button.x = 33;
    pushed.button.y = 44;
    assert(SDL_PushEvent(&pushed) == 0);
    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_MOUSEBUTTONDOWN)) == 1);
    assert(got.button.button == SDL_BUTTON_RIGHT);
    assert(got.button.x == 33 && got.button.y == 44);

    int x = 0, y = 0;
    uint8_t state = SDL_GetMouseState(&x, &y);
    assert(x == 33 && y == 44);
    assert((state & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0);

    pushed = (SDL_Event){};
    pushed.type = SDL_MOUSEBUTTONDOWN;
    pushed.button.button = SDL_BUTTON_RIGHT;
    pushed.button.x = 55;
    pushed.button.y = 66;
    assert(SDL_PushEvent(&pushed) == 0);

    pushed = (SDL_Event){};
    pushed.type = SDL_MOUSEMOTION;
    pushed.motion.x = 77;
    pushed.motion.y = 88;
    pushed.motion.state = SDL_BUTTON(SDL_BUTTON_LEFT);
    assert(SDL_PushEvent(&pushed) == 0);
    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_MOUSEMOTION)) == 1);

    x = 0;
    y = 0;
    state = SDL_GetMouseState(&x, &y);
    assert(x == 77 && y == 88);
    assert((state & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0);
    assert((state & SDL_BUTTON(SDL_BUTTON_RIGHT)) == 0);

    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_MOUSEBUTTONDOWN)) == 1);
}

static void check_mouse_motion_coalescing(void)
{
    SDL_Event pushed = {};
    SDL_Event got = {};

    /*
   * Raw NDL motion is coalesced inside miniSDL's private pump path, but
   * SDL_PushEvent() is a public API.  ONScripter and tests that inject events
   * directly must still get exactly the events they pushed.  This specifically
   * protects the cursor-restore fix from losing synthetic drag/move samples.
   */
    pushed.type = SDL_MOUSEMOTION;
    pushed.motion.x = 101;
    pushed.motion.y = 102;
    pushed.motion.state = 0;
    assert(SDL_PushEvent(&pushed) == 0);

    pushed.motion.x = 201;
    pushed.motion.y = 202;
    assert(SDL_PushEvent(&pushed) == 0);

    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_MOUSEMOTION)) == 1);
    assert(got.motion.x == 101 && got.motion.y == 102);
    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_MOUSEMOTION)) == 1);
    assert(got.motion.x == 201 && got.motion.y == 202);
    assert(SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENTMASK(SDL_MOUSEMOTION)) == 0);
}

static void check_timer(void)
{
    SDL_TimerID id = SDL_AddTimer(1, timer_once, &timer_hits);
    assert(id != NULL);

    /*
   * miniSDL has no host thread on Navy, so timer callbacks are pumped from
   * normal SDL activity.  Polling and delaying here gives the timer machinery
   * a deterministic place to run.
   */
    for (int i = 0; i < 10 && timer_hits == 0; i++)
    {
        SDL_Delay(1);
    }

    assert(timer_hits == 1);
}

static void check_image_rwops(void)
{
    /*
   * Image detection should inspect signatures without consuming the stream,
   * then IMG_Load_RW should accept a ramdisk BMP copied into memory. This is
   * the same split used by ONScripter asset loading.
   */
    static const uint8_t png_sig[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    SDL_RWops *sig = SDL_RWFromMem((void *)png_sig, sizeof(png_sig));
    assert(sig != NULL);
    assert(IMG_isPNG(sig) == 1);
    assert(SDL_RWtell(sig) == 0);
    assert(SDL_RWclose(sig) == 0);

    FILE *fp = fopen("/share/pictures/projectn.bmp", "rb");
    assert(fp != NULL);
    assert(fseek(fp, 0, SEEK_END) == 0);
    long size = ftell(fp);
    assert(size > 0);
    assert(fseek(fp, 0, SEEK_SET) == 0);

    uint8_t *buf = malloc((size_t)size);
    assert(buf != NULL);
    assert(fread(buf, (size_t)size, 1, fp) == 1);
    fclose(fp);

    SDL_RWops *rw = SDL_RWFromMem(buf, (int)size);
    assert(rw != NULL);
    SDL_Surface *surface = IMG_Load_RW(rw, 0);
    assert(surface != NULL);
    assert(surface->w > 0 && surface->h > 0);
    SDL_FreeSurface(surface);
    assert(SDL_RWclose(rw) == 0);
    free(buf);
}

static void check_savebmp_rwops(void)
{
    /*
     * ONScripter's savescreenshot2 command writes a BMP through SDL_RWops.
     * This verifies that miniSDL serialises a 32-bit surface to a normal BMP
     * file and closes the stream when requested, matching SDL_SaveBMP_RW(..., 1).
     */
    const char *path = "ons-sdl-savebmp-test.bmp";
    SDL_Surface *surface = SDL_CreateRGBSurface(0, 2, 2, 32,
                                                DEFAULT_RMASK, DEFAULT_GMASK,
                                                DEFAULT_BMASK, DEFAULT_AMASK);
    assert(surface != NULL);

    uint32_t *pixels = (uint32_t *)surface->pixels;
    pixels[0] = SDL_MapRGBA(surface->format, 0x11, 0x22, 0x33, 0xff);
    pixels[1] = SDL_MapRGBA(surface->format, 0x44, 0x55, 0x66, 0xff);
    pixels[2] = SDL_MapRGBA(surface->format, 0x77, 0x88, 0x99, 0xff);
    pixels[3] = SDL_MapRGBA(surface->format, 0xaa, 0xbb, 0xcc, 0xff);

    SDL_RWops *rw = SDL_RWFromFile(path, "wb");
    assert(rw != NULL);
    assert(SDL_SaveBMP_RW(surface, rw, 1) == 0);

    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    unsigned char header[54];
    unsigned char pixels_on_disk[16];
    assert(fread(header, 1, sizeof(header), fp) == sizeof(header));
    assert(fread(pixels_on_disk, 1, sizeof(pixels_on_disk), fp) == sizeof(pixels_on_disk));
    assert(header[0] == 'B' && header[1] == 'M');
    assert(header[10] == 54);
    assert(header[18] == 2 && header[22] == 2);
    assert(header[28] == 24);
    assert(pixels_on_disk[0] == 0x99 && pixels_on_disk[1] == 0x88 && pixels_on_disk[2] == 0x77);
    assert(pixels_on_disk[3] == 0xcc && pixels_on_disk[4] == 0xbb && pixels_on_disk[5] == 0xaa);
    assert(pixels_on_disk[6] == 0x00 && pixels_on_disk[7] == 0x00);
    assert(pixels_on_disk[8] == 0x33 && pixels_on_disk[9] == 0x22 && pixels_on_disk[10] == 0x11);
    assert(pixels_on_disk[11] == 0x66 && pixels_on_disk[12] == 0x55 && pixels_on_disk[13] == 0x44);
    assert(pixels_on_disk[14] == 0x00 && pixels_on_disk[15] == 0x00);
    fclose(fp);
    unlink(path);
    SDL_FreeSurface(surface);
}

int main(void)
{
    setbuf(stdout, NULL);
    TRACE("init");
    SDL_Init(0);
    TRACE("memory");
    check_memory_rwops();
    TRACE("file");
    check_file_rwops();
    TRACE("events");
    check_events();
    TRACE("mouse");
    check_mouse_events();
    TRACE("mouse-coalesce");
    check_mouse_motion_coalescing();
    TRACE("timer");
    check_timer();
    TRACE("image");
    check_image_rwops();
    TRACE("savebmp");
    check_savebmp_rwops();
    TRACE("quit");
    SDL_Quit();
    printf("ons-sdl-test PASS\n");
    return 0;
}
