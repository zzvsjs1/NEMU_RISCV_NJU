NAME = fceux

ROM_PATH  = ./nes
ROMS      = $(shell ls $(ROM_PATH)/rom/*.nes)
ROM_SRC   = $(addprefix $(ROM_PATH)/gen/, $(addsuffix .c, $(notdir $(basename $(ROMS)))))
FCEUX_SRC = $(shell find -L src/ -name "*.c" -o -name "*.cpp")

INC_PATH  = $(ROM_PATH)/gen/
CFLAGS    = -DPSS_STYLE=1
USE_BUILTIN_ROMS = 0

ifneq ($(ARCH),native)
ifneq ($(strip $(NAVY_HOME)),)
ifneq ($(abspath $(AM_HOME)),$(abspath $(NAVY_HOME)))
USE_BUILTIN_ROMS = 1
endif
else
USE_BUILTIN_ROMS = 1
endif
endif

ifeq ($(USE_BUILTIN_ROMS),1)
# Bare-metal AM targets do not have a filesystem, so keep using the ROMs
# generated into C arrays.  Navy is different: it links libam as an app library
# and reaches the nanos-lite filesystem through libc open/read/lseek, so it must
# not take the built-in-ROM fallback path or carry unused ROM arrays.
CFLAGS   += -D__NO_FILE_SYSTEM__
SRCS      = $(FCEUX_SRC) $(ROM_SRC)
else
SRCS      = $(FCEUX_SRC)
endif

include $(AM_HOME)/Makefile

ifeq ($(USE_BUILTIN_ROMS),1)
$(FCEUX_SRC): $(ROM_SRC)
$(ROM_SRC): rom
endif
.PHONY: rom
rom:
	@-cd $(ROM_PATH) && python3 build-roms.py

clean: myclean

myclean:
	-rm -rf ./nes/gen
