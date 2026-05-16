NAME = klib
SRCS = $(shell find src/ -name "*.c")
ifneq ($(filter riscv64-%,$(ARCH)),)
SRCS := $(filter-out src/int64.c,$(SRCS))
endif
include $(AM_HOME)/Makefile
