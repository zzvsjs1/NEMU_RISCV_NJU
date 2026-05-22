HAS_NAVY = 1
RAMDISK_FILE = build/ramdisk.img
NANOS_INIT ?= games

NAME = nanos-lite
SRCS = $(shell find -L ./src/ -name "*.c" -o -name "*.cpp" -o -name "*.S")
SRCS := $(filter-out ./src/fs/%.c,$(SRCS))
FS_MODE ?= fat32
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
INC_PATH += include $(NAVY_HOME)/libs/libc/include $(NAVY_HOME)/libs/libos/src
endif

ifeq ($(NANOS_INIT),dummy)
CFLAGS += -DNANOS_INIT_DUMMY=1
NAVY_APPS ?=
NAVY_TESTS ?= dummy
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),hello)
CFLAGS += -DNANOS_INIT_HELLO=1
NAVY_APPS ?=
NAVY_TESTS ?= hello
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),preempt)
CFLAGS += -DNANOS_INIT_PREEMPT=1
NAVY_APPS ?=
NAVY_TESTS ?= hello dummy
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),hello-kthread)
CFLAGS += -DNANOS_INIT_HELLO_KTHREAD=1
NAVY_APPS ?=
NAVY_TESTS ?= hello
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),pal-kthread)
CFLAGS += -DNANOS_INIT_PAL_KTHREAD=1
NAVY_APPS ?= pal
NAVY_TESTS ?=
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),nterm-hello)
CFLAGS += -DNANOS_INIT_NTERM_HELLO=1 -DNANOS_EXIT_RESTART_NTERM=1
NAVY_APPS ?= nterm pal
NAVY_TESTS ?= hello
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),nterm-selftest)
CFLAGS += -DNANOS_INIT_NTERM_SELFTEST=1 -DNANOS_EXIT_RESTART_NTERM=1
NAVY_APPS ?= nterm
NAVY_TESTS ?=
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),priv-test)
CFLAGS += -DNANOS_INIT_SINGLE=1 -DNANOS_INIT_PRIV_TEST=1 -DNANOS_INIT_PATH=\"/bin/priv-test\"
NAVY_APPS ?=
NAVY_TESTS ?= priv-test
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),pf-cause-test)
CFLAGS += -DNANOS_INIT_SINGLE=1 -DNANOS_INIT_PF_CAUSE_TEST=1 -DNANOS_INIT_PATH=\"/bin/pf-cause-test\"
NAVY_APPS ?=
NAVY_TESTS ?= pf-cause-test
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
else ifeq ($(NANOS_INIT),games)
CFLAGS += -DNANOS_INIT_GAMES=1
else
CFLAGS += -DNANOS_INIT_SINGLE=1 -DNANOS_INIT_PATH=\"/bin/$(NANOS_INIT)\"
NAVY_APPS ?=
NAVY_TESTS ?= $(NANOS_INIT)
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
endif

NANOS_CONFIG_STAMP = $(DST_DIR)/.nanos-config

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

ifeq ($(ISA),riscv64)
NAVY_APPS ?= pal bird nslider
NAVY_TESTS ?= hello
NAVY_UPDATE_APP_ARGS = APPS="$(NAVY_APPS)" TESTS="$(NAVY_TESTS)"
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
	$(MAKE) -s -C $(NAVY_HOME) ISA=$(ISA) FS_MODE=$(FS_MODE) $(NAVY_UPDATE_APP_ARGS) ramdisk
	@ln -sf $(NAVY_HOME)/build/ramdisk.img $(RAMDISK_FILE)
ifeq ($(FS_MODE),flat)
	@ln -sf $(NAVY_HOME)/build/ramdisk.h src/files.h
else ifeq ($(FS_MODE),fat32)
	@if [ -L src/files.h ]; then rm -f src/files.h; fi
endif
	@ln -sf $(NAVY_HOME)/libs/libos/src/syscall.h src/syscall.h

.PHONY: update
endif
