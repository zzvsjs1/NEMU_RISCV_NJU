include $(AM_HOME)/scripts/isa/riscv.mk
NEMU_DEFCONFIG ?= riscv64-am-headless_defconfig
include $(AM_HOME)/scripts/platform/nemu.mk
CFLAGS  += -DISA_H=\"riscv/riscv.h\"
COMMON_CFLAGS += -march=rv64im_zicsr_zifencei -mabi=lp64  # overwrite

AM_SRCS += riscv/nemu/start.S \
           riscv/nemu/cte.c \
           riscv/nemu/trap.S \
           riscv/nemu/vme.c
