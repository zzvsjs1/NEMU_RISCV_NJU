.PHONY: all run gdb clean latest

RESULT = .result
$(shell > $(RESULT))

COLOR_RED   = \033[1;31m
COLOR_GREEN = \033[1;32m
COLOR_NONE  = \033[0m

ALL_TEST_SRCS = $(sort $(shell find tests -mindepth 2 -name "*.c"))
ALL_TESTS = $(basename $(notdir $(ALL_TEST_SRCS)))
.PHONY: $(ALL_TESTS)

ARCH_ISA = $(firstword $(subst -, ,$(ARCH)))
ARCH_TEST_DIRS = common
ifneq ($(filter riscv32 riscv32e,$(ARCH_ISA)),)
ARCH_TEST_DIRS += riscv riscv32
else ifeq ($(ARCH_ISA),riscv64)
ARCH_TEST_DIRS += riscv riscv64
else ifneq ($(ARCH_ISA),)
ARCH_TEST_DIRS += $(ARCH_ISA)
endif

ARCH_TEST_SRCS = $(foreach dir,$(ARCH_TEST_DIRS),$(wildcard tests/$(dir)/*.c))
ARCH_TESTS = $(basename $(notdir $(ARCH_TEST_SRCS)))

# This guest intentionally makes NEMU reject a cross-map MMIO access.  Keep it
# available through explicit ALL=... selection without making a normal RV64
# sweep report its expected host abort as a failed architectural test.
ifeq ($(ARCH_ISA),riscv64)
ARCH_TESTS := $(filter-out riscv64-jit-mmio-boundary,$(ARCH_TESTS))
endif

# RV32D is an optional configuration layered on the default RV32F target.
# Its conformance tests deliberately require misa.D and therefore belong to
# the explicit RV32D matrix rather than the generic architecture test list.
# Naming one of them through ALL still selects it normally.
ifneq ($(filter riscv32 riscv32e,$(ARCH_ISA)),)
ARCH_TESTS := $(filter-out riscv32-d-%,$(ARCH_TESTS))
endif

ALL ?= $(ARCH_TESTS)

all: $(addprefix Makefile., $(ALL))
	@echo "test list [$(words $(ALL)) item(s)]:" $(ALL)

$(ALL_TESTS): %: Makefile.%

define def_test_rule
Makefile.$(1): $(2) latest
	@/bin/echo -e "NAME = $(1)\nSRCS = $(2)\ninclude $$$${AM_HOME}/Makefile" > $$@
# Provide deterministic mouse events for tests that check MMIO side effects.
# The scripts make headless runs repeatable and avoid requiring real host input.
	@if [ "$(1)" = "jit-ldst-signext-asm" ]; then \
		NEMU_MOUSE_SCRIPT=move:17,23 make -s -f $$@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	elif [ "$(1)" = "riscv-io-strict" ]; then \
		NEMU_MOUSE_SCRIPT='move:31,47;move:31,47;wheel:2,-3' make -s -f $$@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	elif [ "$(1)" = "riscv64-jit-memory-entry" ]; then \
		NEMU_MOUSE_SCRIPT='move:17,23;wheel:2,-3' make -s -f $$@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	elif [ "$(1)" = "riscv-fast-mmu-priv-strict" ]; then \
		NEMU_DISABLE_JIT=1 make -s -f $$@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	else \
		make -s -f $$@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	fi; \
	status=$$$$?; \
	if [ $$$$status -eq 0 ]; then \
		printf "[%14s] $(COLOR_GREEN)PASS$(COLOR_NONE)\n" $(1) >> $(RESULT); \
	else \
		printf "[%14s] $(COLOR_RED)***FAIL***$(COLOR_NONE)\n" $(1) >> $(RESULT); \
	fi
	-@rm -f Makefile.$(1)
endef

$(foreach src,$(ALL_TEST_SRCS),$(eval $(call def_test_rule,$(basename $(notdir $(src))),$(src))))

run: all
	@cat $(RESULT)
	@rm $(RESULT)

gdb: all

clean:
	rm -rf Makefile.* build/

latest:
