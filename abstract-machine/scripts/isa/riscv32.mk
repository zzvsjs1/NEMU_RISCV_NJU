CROSS_COMPILE := riscv32-unknown-linux-gnu-
COMMON_FLAGS  := -fno-pic -march=rv32i_zicsr -mabi=ilp32
CFLAGS        += $(COMMON_FLAGS) -static
ASFLAGS       += $(COMMON_FLAGS) -O0
LDFLAGS       += -melf32lriscv
