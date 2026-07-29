HAS_NAVY = 1
RAMDISK_FILE = build/ramdisk.img
NANOS_INIT ?= games

NAME = nanos-lite-mt

NANOS_BASE := ../nanos-lite
NANOS_BASE_SRC := $(NANOS_BASE)/src

# Keep unchanged services shared with Nanos-lite so filesystem, loader, device,
# and ABI fixes continue to reach both targets. The MT target supplies only its
# own process/task scheduler and resource path.
SRCS = $(filter-out $(NANOS_BASE_SRC)/proc.c,$(wildcard $(NANOS_BASE_SRC)/*.c))
SRCS += src/proc.c src/thread-table.c src/resources.S

FS_MODE ?= fat32
SUPPORTED_FS_MODES = flat fat32
FS_MODE_HELPER_GOALS = clean clean-all html init
FS_MODE_CHECK_GOALS := $(if $(MAKECMDGOALS),$(filter-out $(FS_MODE_HELPER_GOALS),$(MAKECMDGOALS)),__default__)

ifneq ($(FS_MODE_CHECK_GOALS),)
ifneq ($(ARCH),riscv64-nemu)
$(error nanos-lite-mt supports only ARCH=riscv64-nemu)
endif
ifneq ($(words $(FS_MODE)),1)
$(error unsupported FS_MODE=$(FS_MODE), expected exactly one of: $(SUPPORTED_FS_MODES))
endif
ifeq ($(filter $(SUPPORTED_FS_MODES),$(FS_MODE)),)
$(error unsupported FS_MODE=$(FS_MODE), expected exactly one of: $(SUPPORTED_FS_MODES))
endif
ifeq ($(FS_MODE),flat)
SRCS += $(NANOS_BASE_SRC)/fs/flat.c
else ifeq ($(FS_MODE),fat32)
SRCS += $(NANOS_BASE_SRC)/fs/fat32_backend.c
SRCS += $(NANOS_BASE_SRC)/fs/fat32.c
SRCS += $(NANOS_BASE_SRC)/fs/fat32_bpb.c
SRCS += $(NANOS_BASE_SRC)/fs/fat32_cluster.c
SRCS += $(NANOS_BASE_SRC)/fs/fat32_dir.c
SRCS += $(NANOS_BASE_SRC)/fs/fat32_lfn.c
endif
endif

CFLAGS += -DNANOS_LITE_MT -DMULTIPROGRAM -DTIME_SHARING

ifeq ($(NANOS_INIT),miniSDL-thread-test)
CFLAGS += -DNANOS_INIT_MINISDL_THREAD_TEST
NAVY_APPS :=
NAVY_TESTS := miniSDL-thread-test
NAVY_UPDATE_APP_ARGS += APPS="" TESTS="$(NAVY_TESTS)"
else ifneq ($(NANOS_INIT),games)
$(error unsupported NANOS_INIT=$(NANOS_INIT), expected one of: games miniSDL-thread-test)
endif

INC_PATH += include
INC_PATH += $(NANOS_BASE)/include
INC_PATH += $(NANOS_BASE_SRC)
INC_PATH += $(NANOS_BASE_SRC)/fs

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

INC_PATH += $(NAVY_HOME)/libs/libc/include $(NAVY_HOME)/libs/libos/src

NANOS_CONFIG_STAMP = $(DST_DIR)/.nanos-mt-config

$(NANOS_CONFIG_STAMP): force
	@mkdir -p $(dir $@)
	@{ \
		printf '%s\n' 'NANOS_INIT=$(NANOS_INIT)'; \
		printf '%s\n' 'FS_MODE=$(FS_MODE)'; \
		printf '%s\n' 'NAVY_APPS=$(NAVY_APPS)'; \
		printf '%s\n' 'NAVY_TESTS=$(NAVY_TESTS)'; \
	} > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(OBJS): $(NANOS_CONFIG_STAMP)

./src/resources.S: $(RAMDISK_FILE)
	@touch $@

ifeq ($(HAS_NAVY),)
files = $(RAMDISK_FILE) src/files.h src/syscall.h
$(foreach f,$(files),$(if $(wildcard $f),, $(shell touch $f)))
else

ifneq ($(FS_MODE_CHECK_GOALS),)
  ifeq ($(wildcard $(NAVY_HOME)/libs/libos/src/syscall.h),)
  $(error $$NAVY_HOME must be a Navy-apps repo)
  endif
endif

update:
	$(MAKE) -s -C $(NAVY_HOME) ISA=$(ISA) FS_MODE=$(FS_MODE) $(NAVY_UPDATE_APP_ARGS) ramdisk
	@mkdir -p $(dir $(RAMDISK_FILE))
	@ln -sf $(NAVY_HOME)/build/ramdisk.img $(RAMDISK_FILE)
ifeq ($(FS_MODE),flat)
	@ln -sf $(NAVY_HOME)/build/ramdisk.h $(NANOS_BASE_SRC)/files.h
else ifeq ($(FS_MODE),fat32)
	@if [ -L $(NANOS_BASE_SRC)/files.h ]; then rm -f $(NANOS_BASE_SRC)/files.h; fi
endif
	@ln -sf $(NAVY_HOME)/libs/libos/src/syscall.h $(NANOS_BASE_SRC)/syscall.h

.PHONY: update
endif
