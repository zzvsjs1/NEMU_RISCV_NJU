REPO_PATH = repo
ifeq ($(wildcard repo/include/BirdGame.h),)
  $(shell git clone --depth=1 git@github.com:zzvsjs1/doomgeneric.git $(REPO_PATH))
endif

NAME = doom
SRCS = $(shell find $(REPO_PATH)/src/ -name "*.c")
INC_PATH = $(REPO_PATH)/doomgeneric
LIBS += libminiSDL libSDL_image libfixedptc libSDL_mixer
include $(NAVY_HOME)/Makefile

install: install-file
init:;

install-file:
	ln -sf -T $(abspath $(REPO_PATH)/res) $(NAVY_HOME)/fsimg/share/games/doom

.PHONY: install-file init