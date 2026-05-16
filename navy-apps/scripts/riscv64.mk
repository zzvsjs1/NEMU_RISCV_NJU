include $(NAVY_HOME)/scripts/riscv/common.mk
CFLAGS  += -march=rv64im_zicsr_zifencei -mabi=lp64
