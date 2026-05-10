HAS_NAVY = 1
RAMDISK_FILE = build/ramdisk.img

NAME = nanos-lite
SRCS = $(shell find -L ./src/ -name "*.c" -o -name "*.cpp" -o -name "*.S")
SRCS := $(filter-out ./src/fs/%.c,$(SRCS))
FS_MODE ?= flat
SUPPORTED_FS_MODES = flat fat32
FS_MODE_HELPER_GOALS = clean clean-all html init
FS_MODE_CHECK_GOALS := $(if $(MAKECMDGOALS),$(filter-out $(FS_MODE_HELPER_GOALS),$(MAKECMDGOALS)),__default__)

ifneq ($(FS_MODE_CHECK_GOALS),)
ifneq ($(words $(FS_MODE)),1)
$(error unsupported FS_MODE=$(FS_MODE), expected exactly one of: $(SUPPORTED_FS_MODES))
endif
ifeq ($(filter $(SUPPORTED_FS_MODES),$(FS_MODE)),)
$(error unsupported FS_MODE=$(FS_MODE), expected exactly one of: $(SUPPORTED_FS_MODES))
endif
ifeq ($(FS_MODE),flat)
SRCS += src/fs/flat.c
else ifeq ($(FS_MODE),fat32)
SRCS += src/fs/fat32_backend.c
SRCS += src/fs/fat32.c
SRCS += src/fs/fat32_bpb.c
SRCS += src/fs/fat32_cluster.c
SRCS += src/fs/fat32_dir.c
SRCS += src/fs/fat32_lfn.c
endif
endif

ifeq ($(FS_MODE_CHECK_GOALS),)
  ifeq ($(wildcard $(AM_HOME)/Makefile),)
WORK_DIR = $(shell pwd)
clean:
	rm -rf Makefile.html $(WORK_DIR)/build/
.PHONY: clean
  else
include $(AM_HOME)/Makefile
  endif
else
include $(AM_HOME)/Makefile
endif

ifeq ($(ARCH),native)
ISA = am_native
else
INC_PATH += include $(NAVY_HOME)/libs/libc/include
endif

./src/resources.S: $(RAMDISK_FILE)
	@touch $@

ifeq ($(HAS_NAVY),)
files = $(RAMDISK_FILE) src/files.h src/syscall.h
# create an empty file if it does not exist
$(foreach f,$(files),$(if $(wildcard $f),, $(shell touch $f)))
else

ifneq ($(FS_MODE_CHECK_GOALS),)
  ifeq ($(wildcard $(NAVY_HOME)/libs/libos/src/syscall.h),)
  $(error $$NAVY_HOME must be a Navy-apps repo)
  endif
endif

update:
	$(MAKE) -s -C $(NAVY_HOME) ISA=$(ISA) FS_MODE=$(FS_MODE) ramdisk
	@ln -sf $(NAVY_HOME)/build/ramdisk.img $(RAMDISK_FILE)
ifeq ($(FS_MODE),flat)
	@ln -sf $(NAVY_HOME)/build/ramdisk.h src/files.h
else ifeq ($(FS_MODE),fat32)
	@if [ -L src/files.h ]; then rm -f src/files.h; fi
endif
	@ln -sf $(NAVY_HOME)/libs/libos/src/syscall.h src/syscall.h

.PHONY: update
endif
