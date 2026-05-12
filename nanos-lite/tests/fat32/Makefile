CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -g -I../../src

LFN_SRCS := test_fat32_lfn.c ../../src/fs/fat32_lfn.c
BPB_SRCS := test_fat32_bpb.c ../../src/fs/fat32_bpb.c
READ_SRCS := test_fat32_read.c fat32_test_disk.c ../../src/fs/fat32.c ../../src/fs/fat32_dir.c ../../src/fs/fat32_lfn.c ../../src/fs/fat32_cluster.c ../../src/fs/fat32_bpb.c
LOOKUP_SRCS := test_fat32_lookup.c fat32_test_disk.c ../../src/fs/fat32_dir.c ../../src/fs/fat32_lfn.c ../../src/fs/fat32_cluster.c ../../src/fs/fat32_bpb.c
WRITE_SRCS := test_fat32_write.c fat32_test_disk.c ../../src/fs/fat32.c ../../src/fs/fat32_dir.c ../../src/fs/fat32_lfn.c ../../src/fs/fat32_cluster.c ../../src/fs/fat32_bpb.c
POSIX_SRCS := test_fat32_posix.c fat32_test_disk.c ../../src/fs/fat32.c ../../src/fs/fat32_dir.c ../../src/fs/fat32_lfn.c ../../src/fs/fat32_cluster.c ../../src/fs/fat32_bpb.c
ABI_SRCS := test_nanos_syscall_abi.c ../../../navy-apps/libs/libos/src/nanos_abi.c
ABI_CFLAGS := $(CFLAGS) -I../../../navy-apps/libs/libc/include -I../../../navy-apps/libs/libos/src
TIME_ABI_CC ?= riscv64-linux-gnu-gcc
TIME_ABI_CFLAGS := -std=c11 -Wall -Wextra -Werror -march=rv32im_zicsr -mabi=ilp32 \
	-DARCH_H='"arch/riscv32-nemu.h"' -I../../include \
	-I../../../abstract-machine/am/include -I../../../abstract-machine/klib/include

.PHONY: all test clean

all: test_fat32_lfn test_fat32_bpb test_fat32_read test_fat32_lookup test_fat32_write test_fat32_posix test_nanos_syscall_abi test_nanos_time_abi.o

test: all
	./test_fat32_lfn
	./test_fat32_bpb
	./test_fat32_read
	./test_fat32_lookup
	./test_fat32_write
	./test_fat32_posix
	./test_nanos_syscall_abi

test_fat32_lfn: $(LFN_SRCS) ../../src/fs/fat32.h
	$(CC) $(CFLAGS) -DFAT32_HOST_TEST -o $@ $(LFN_SRCS)

test_fat32_bpb: $(BPB_SRCS) ../../src/fs/fat32.h
	$(CC) $(CFLAGS) -DFAT32_HOST_TEST -o $@ $(BPB_SRCS)

test_fat32_read: $(READ_SRCS) ../../src/fs/fat32.h
	$(CC) $(CFLAGS) -DFAT32_HOST_TEST -o $@ $(READ_SRCS)

test_fat32_lookup: $(LOOKUP_SRCS) ../../src/fs/fat32.h
	$(CC) $(CFLAGS) -DFAT32_HOST_TEST -o $@ $(LOOKUP_SRCS)

test_fat32_write: $(WRITE_SRCS) ../../src/fs/fat32.h
	$(CC) $(CFLAGS) -DFAT32_HOST_TEST -o $@ $(WRITE_SRCS)

test_fat32_posix: $(POSIX_SRCS) ../../src/fs/fat32.h
	$(CC) $(CFLAGS) -DFAT32_HOST_TEST -o $@ $(POSIX_SRCS)

test_nanos_syscall_abi: $(ABI_SRCS)
	$(CC) $(ABI_CFLAGS) -o $@ $(ABI_SRCS)

test_nanos_time_abi.o: test_nanos_time_abi.c ../../include/common.h
	$(TIME_ABI_CC) $(TIME_ABI_CFLAGS) -c -o $@ $<

clean:
	rm -rf test_fat32_lfn test_fat32_bpb test_fat32_read test_fat32_lookup test_fat32_write test_fat32_posix test_nanos_syscall_abi test_nanos_time_abi.o fat32-read-work fat32-lookup-work fat32-write-work fat32-posix-work
