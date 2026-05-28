REPO_PATH = repo
DOOM_DIR = $(REPO_PATH)/doomgeneric
DOOM_GAME_DIR = game
DOOM_WAD = $(DOOM_GAME_DIR)/DOOM.WAD
DOOM_FSIMG_DIR = $(NAVY_HOME)/fsimg/share/games/doom

ifeq ($(wildcard $(DOOM_DIR)/doomgeneric.h),)
  $(shell git clone --depth=1 git@github.com:zzvsjs1/doomgeneric.git $(REPO_PATH))
endif

NAME = doom
EXCLUDED_SRCS = \
  $(DOOM_DIR)/doomgeneric_sdl.c \
  $(DOOM_DIR)/doomgeneric_xlib.c \
  $(DOOM_DIR)/doomgeneric_win.c \
  $(DOOM_DIR)/doomgeneric_osx.c \
  $(DOOM_DIR)/doomgeneric_fb.c \
  $(DOOM_DIR)/doomgeneric_allegro.c \
  $(DOOM_DIR)/doomgeneric_emscripten.c \
  $(DOOM_DIR)/doomgeneric_linuxvt.c \
  $(DOOM_DIR)/doomgeneric_soso.c \
  $(DOOM_DIR)/doomgeneric_sosox.c \
  $(DOOM_DIR)/i_sdlmusic.c \
  $(DOOM_DIR)/i_allegromusic.c \
  $(DOOM_DIR)/i_allegrosound.c

# Build Doom's SDL sound-effect backend, but keep the SDL music backend out of
# this Navy target.  Doom WAD music is MUS/MIDI data and Navy does not currently
# ship a MIDI/OPL synthesiser, so navy_music_stub.c supplies a quiet music module
# while i_sdlsound.c still feeds PCM sound effects into libSDL_mixer.
SRCS = $(filter-out $(EXCLUDED_SRCS),$(wildcard $(DOOM_DIR)/*.c)) navy_music_stub.c

INC_PATH += $(DOOM_DIR)
# CFLAGS += -DFEATURE_SOUND
LIBS += libminiSDL libSDL_mixer

include $(NAVY_HOME)/Makefile

install-file:
	@if [ ! -f "$(DOOM_WAD)" ]; then \
		echo "error: missing DOOM WAD: $(abspath $(DOOM_WAD))" >&2; \
		exit 1; \
	fi
	@mkdir -p "$(DOOM_FSIMG_DIR)"
	@cp "$(DOOM_WAD)" "$(DOOM_FSIMG_DIR)/doom.wad"

install: install-file

init:;

.PHONY: install-file init
