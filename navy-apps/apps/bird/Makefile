REPO_PATH = repo
ifeq ($(wildcard repo/include/BirdGame.h),)
  $(shell git clone --depth=1 git@github.com:NJU-ProjectN/sdlbird.git $(REPO_PATH))
endif

NAME = bird
BIRD_REPO_SRCS := $(shell find $(REPO_PATH)/src/ -name "*.cpp")

# sdlbird's audio source redeclares SDL_AudioCVT, which current MiniSDL already
# provides. Keep the MIPS32 Bird build usable without changing MiniSDL or
# altering the source selection for any other architecture.
ifeq ($(ISA),mips32)
SRCS := $(filter-out $(REPO_PATH)/src/Audio.cpp,$(BIRD_REPO_SRCS)) audio_stub.cpp
else
SRCS := $(BIRD_REPO_SRCS)
endif

INC_PATH = $(REPO_PATH)/include
LIBS += libminiSDL libSDL_image libfixedptc
include $(NAVY_HOME)/Makefile

install: install-file
init:;

install-file:
	ln -sf -T $(abspath $(REPO_PATH)/res) $(NAVY_HOME)/fsimg/share/games/bird

.PHONY: install-file init
