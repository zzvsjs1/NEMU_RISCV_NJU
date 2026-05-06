REPO_PATH = repo
PATCH_FILE = $(abspath patch/mouse.diff)
ifeq ($(wildcard $(REPO_PATH)/include/ONScripter.h),)
  $(shell git clone --depth=1 git@github.com:NJU-ProjectN/onscripter-navy.git $(REPO_PATH))
endif

PATCH_ALREADY_APPLIED := $(shell cd $(REPO_PATH) && git apply --reverse --check $(PATCH_FILE) >/dev/null 2>&1 && echo yes)
ifeq ($(PATCH_ALREADY_APPLIED),)
  PATCH_CAN_APPLY := $(shell cd $(REPO_PATH) && git apply --check $(PATCH_FILE) >/dev/null 2>&1 && echo yes)
  ifeq ($(PATCH_CAN_APPLY),yes)
    PATCH_APPLIED := $(shell cd $(REPO_PATH) && git apply $(PATCH_FILE) >/dev/null 2>&1 && echo yes)
    ifneq ($(PATCH_APPLIED),yes)
      $(error failed to apply ONScripter mouse patch: $(PATCH_FILE))
    endif
  else
    $(error failed to apply ONScripter mouse patch: $(PATCH_FILE))
  endif
endif

NAME = onscripter
SRCS = $(shell find $(REPO_PATH)/src/ -name "*.cpp")
INC_PATH = $(REPO_PATH)/include
LIBS += libminiSDL libSDL_ttf libSDL_image libSDL_mixer
include $(NAVY_HOME)/Makefile

init:;
.PHONY: init
