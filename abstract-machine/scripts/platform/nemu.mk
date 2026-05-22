AM_SRCS := platform/nemu/trm.c \
           platform/nemu/ioe/ioe.c \
           platform/nemu/ioe/timer.c \
           platform/nemu/ioe/input.c \
           platform/nemu/ioe/gpu.c \
           platform/nemu/ioe/audio.c \
           platform/nemu/ioe/disk.c \
           platform/nemu/mpe.c

CFLAGS    += -fdata-sections -ffunction-sections
CFLAGS    += -I$(AM_HOME)/am/src/platform/nemu/include
LDSCRIPTS += $(AM_HOME)/scripts/linker.ld
LDFLAGS   += --defsym=_pmem_start=0x80000000 --defsym=_entry_offset=0x0
LDFLAGS   += --gc-sections -e _start
NEMUFLAGS += -l $(shell dirname $(IMAGE).elf)/nemu-log.txt

MAINARGS_MAX_LEN = 64
MAINARGS_PLACEHOLDER = the_insert-arg_rule_in_Makefile_will_insert_mainargs_here
CFLAGS += -DMAINARGS_MAX_LEN=$(MAINARGS_MAX_LEN) -DMAINARGS_PLACEHOLDER=\""$(MAINARGS_PLACEHOLDER)"\"

NEMU_CONFIG_ISA = $(if $(filter riscv32e,$(ISA)),riscv32,$(ISA))
NEMU_CONFIG_STAMP = $(NEMU_HOME)/.config.defconfig
NEMU_DEFCONFIG_FILE = $(NEMU_HOME)/configs/$(NEMU_DEFCONFIG)
NEMU_CONFIG_STAMP_TEXT = $(NEMU_DEFCONFIG) $(wordlist 1,2,$(shell cksum "$(NEMU_DEFCONFIG_FILE)" 2>/dev/null))

nemu-config:
ifneq ($(NEMU_DEFCONFIG),)
	@if ! grep -q '^CONFIG_ISA_$(NEMU_CONFIG_ISA)=y' "$(NEMU_HOME)/.config" 2>/dev/null || \
	    { [ "$(ISA)" = "riscv32e" ] && ! grep -q '^CONFIG_RVE=y' "$(NEMU_HOME)/.config"; } || \
	    { [ "$(ISA)" = "riscv32" ] && grep -q '^CONFIG_RVE=y' "$(NEMU_HOME)/.config"; } || \
	    { [ ! -f "$(NEMU_CONFIG_STAMP)" ] || [ "$$(cat "$(NEMU_CONFIG_STAMP)")" != "$(NEMU_CONFIG_STAMP_TEXT)" ]; }; then \
		$(MAKE) -C "$(NEMU_HOME)" "$(NEMU_DEFCONFIG)"; \
		printf '%s\n' "$(NEMU_CONFIG_STAMP_TEXT)" > "$(NEMU_CONFIG_STAMP)"; \
	fi
endif
	@if ! grep -q '^CONFIG_ISA_$(NEMU_CONFIG_ISA)=y$$' "$(NEMU_HOME)/.config" 2>/dev/null || \
	    { [ "$(ISA)" = "riscv32e" ] && ! grep -q '^CONFIG_RVE=y$$' "$(NEMU_HOME)/.config" 2>/dev/null; } || \
	    { [ "$(ISA)" = "riscv32" ] && grep -q '^CONFIG_RVE=y$$' "$(NEMU_HOME)/.config" 2>/dev/null; }; then \
		printf '%s\n' "NEMU config ISA does not match ARCH=$(ARCH)." >&2; \
		printf '%s\n' "Update NEMU with make menuconfig or run a matching defconfig before launching AM." >&2; \
		exit 1; \
	fi
	@if [ "$(ISA)" = "x86" ]; then \
		if ! grep -q '^CONFIG_MBASE=0x0$$' "$(NEMU_HOME)/.config" 2>/dev/null || \
		   ! grep -q '^CONFIG_PC_RESET_OFFSET=0x100000$$' "$(NEMU_HOME)/.config" 2>/dev/null; then \
			printf '%s\n' "adjusting NEMU memory layout for ARCH=$(ARCH): MBASE=0x0 PC_RESET_OFFSET=0x100000"; \
			if grep -q '^CONFIG_MBASE=' "$(NEMU_HOME)/.config" 2>/dev/null; then \
				sed -i 's/^CONFIG_MBASE=.*/CONFIG_MBASE=0x0/' "$(NEMU_HOME)/.config"; \
			else \
				printf '%s\n' 'CONFIG_MBASE=0x0' >>"$(NEMU_HOME)/.config"; \
			fi; \
			if grep -q '^CONFIG_PC_RESET_OFFSET=' "$(NEMU_HOME)/.config" 2>/dev/null; then \
				sed -i 's/^CONFIG_PC_RESET_OFFSET=.*/CONFIG_PC_RESET_OFFSET=0x100000/' "$(NEMU_HOME)/.config"; \
			else \
				printf '%s\n' 'CONFIG_PC_RESET_OFFSET=0x100000' >>"$(NEMU_HOME)/.config"; \
			fi; \
			$(MAKE) -C "$(NEMU_HOME)" syncconfig; \
		fi; \
	elif [ -n "$(filter riscv32 riscv32e riscv64,$(ISA))" ]; then \
		if ! grep -q '^CONFIG_MBASE=0x80000000$$' "$(NEMU_HOME)/.config" 2>/dev/null || \
		   ! grep -q '^CONFIG_PC_RESET_OFFSET=0$$' "$(NEMU_HOME)/.config" 2>/dev/null; then \
			printf '%s\n' "adjusting NEMU memory layout for ARCH=$(ARCH): MBASE=0x80000000 PC_RESET_OFFSET=0"; \
			if grep -q '^CONFIG_MBASE=' "$(NEMU_HOME)/.config" 2>/dev/null; then \
				sed -i 's/^CONFIG_MBASE=.*/CONFIG_MBASE=0x80000000/' "$(NEMU_HOME)/.config"; \
			else \
				printf '%s\n' 'CONFIG_MBASE=0x80000000' >>"$(NEMU_HOME)/.config"; \
			fi; \
			if grep -q '^CONFIG_PC_RESET_OFFSET=' "$(NEMU_HOME)/.config" 2>/dev/null; then \
				sed -i 's/^CONFIG_PC_RESET_OFFSET=.*/CONFIG_PC_RESET_OFFSET=0/' "$(NEMU_HOME)/.config"; \
			else \
				printf '%s\n' 'CONFIG_PC_RESET_OFFSET=0' >>"$(NEMU_HOME)/.config"; \
			fi; \
			$(MAKE) -C "$(NEMU_HOME)" syncconfig; \
		fi; \
	fi

insert-arg: image
	@python $(AM_HOME)/tools/insert-arg.py $(IMAGE).bin $(MAINARGS_MAX_LEN) "$(MAINARGS_PLACEHOLDER)" "$(mainargs)"

image: image-dep
	@$(OBJDUMP) -d $(IMAGE).elf > $(IMAGE).txt
	@echo + OBJCOPY "->" $(IMAGE_REL).bin
	@$(OBJCOPY) -S --set-section-flags .bss=alloc,contents -O binary $(IMAGE).elf $(IMAGE).bin

run: insert-arg nemu-config
	$(MAKE) -C $(NEMU_HOME) ISA=$(ISA) run ARGS="$(NEMUFLAGS)" IMG=$(IMAGE).bin

gdb: insert-arg nemu-config
	$(MAKE) -C $(NEMU_HOME) ISA=$(ISA) gdb ARGS="$(NEMUFLAGS)" IMG=$(IMAGE).bin

.PHONY: insert-arg nemu-config
