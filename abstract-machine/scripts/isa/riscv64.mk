CROSS_COMPILE := riscv64-linux-gnu-
COMMON_FLAGS  := -ffreestanding -fno-pic -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany -mstrict-align
CFLAGS        += $(COMMON_FLAGS) -static
ASFLAGS       += $(COMMON_FLAGS) -O0
LDFLAGS       += -melf64lriscv
