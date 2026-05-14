#include <NDL.h>
#include <sdl-audio.h>
#include <sdl-video.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "%s:%d: check failed: %s\n",                   \
                    __FILE__, __LINE__, #cond);                             \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

static int fake_audio_capacity = 65536;
static int fake_audio_free = 65536;
static int callback_calls = 0;
static int draw_calls = 0;
static int calls_after_prefill = -1;

uint32_t NDL_GetTicks(void)
{
    return 0;
}

void NDL_OpenAudio(int freq, int channels, int samples)
{
    (void)freq;
    (void)channels;
    (void)samples;
    fake_audio_free = fake_audio_capacity;
}

void NDL_CloseAudio(void)
{
}

int NDL_PlayAudio(void *buf, int len)
{
    CHECK(buf != NULL);
    CHECK(len >= 0);

    int accepted = len;
    if (accepted > fake_audio_free)
        accepted = fake_audio_free;

    fake_audio_free -= accepted;
    return accepted;
}

int NDL_QueryAudio(void)
{
    return fake_audio_free;
}

void NDL_OpenCanvas(int *w, int *h)
{
    (void)w;
    (void)h;
}

void NDL_DrawRect(uint32_t *pixels, int x, int y, int w, int h)
{
    (void)pixels;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    CHECK(callback_calls > calls_after_prefill);
    draw_calls++;
}

void NDL_TranslateMouse(int *x, int *y)
{
    (void)x;
    (void)y;
}

int NDL_PollEvent(char *buf, int len)
{
    (void)buf;
    (void)len;
    return 0;
}

static void fill_audio(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    CHECK(stream != NULL);
    CHECK(len > 0);
    callback_calls++;
    memset(stream, 0, (size_t)len);
}

int main(void)
{
    SDL_AudioSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.freq = 44100;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;
    spec.callback = fill_audio;

    CHECK(SDL_OpenAudio(&spec, NULL) == 0);
    SDL_PauseAudio(0);

    calls_after_prefill = callback_calls;
    CHECK(calls_after_prefill > 0);

    /*
     * Simulate the host device consuming all queued audio while ONScripter is
     * busy rendering.  A following video flush must pump audio too, otherwise
     * voice playback stops after only the prefilled beginning.
     */
    fake_audio_free = fake_audio_capacity;

    SDL_Surface *surface = SDL_CreateRGBSurface(0, 2, 2, 32,
                                                DEFAULT_RMASK, DEFAULT_GMASK,
                                                DEFAULT_BMASK, DEFAULT_AMASK);
    CHECK(surface != NULL);
    SDL_UpdateRect(surface, 0, 0, 0, 0);

    CHECK(draw_calls == 1);
    CHECK(callback_calls > calls_after_prefill);

    SDL_FreeSurface(surface);
    SDL_CloseAudio();
    return 0;
}
