SRCS-y += src/nemu-main.c
DIRS-y += src/cpu src/monitor src/utils
DIRS-$(CONFIG_MODE_SYSTEM) += src/memory
DIRS-BLACKLIST-$(CONFIG_TARGET_AM) += src/monitor/sdb
DIRS-BLACKLIST-$(CONFIG_TARGET_SHARE) += src/monitor
SRCS-BLACKLIST-$(CONFIG_TARGET_SHARE) += src/nemu-main.c src/cpu/difftest/dut.c src/engine/interpreter/init.c

SHARE = $(if $(CONFIG_TARGET_SHARE),1,0)
LIBS += $(if $(CONFIG_TARGET_NATIVE_ELF),-lreadline -ldl -pie,)

ifdef mainargs
ASFLAGS += -DBIN_PATH=\"$(mainargs)\"
endif
SRCS-$(CONFIG_TARGET_AM) += src/am-bin.S
.PHONY: src/am-bin.S
