.PHONY: all run gdb clean latest $(ALL)

RESULT = .result
$(shell > $(RESULT))

COLOR_RED   = \033[1;31m
COLOR_GREEN = \033[1;32m
COLOR_NONE  = \033[0m

ALL = $(basename $(notdir $(shell find tests/. -name "*.c")))

all: $(addprefix Makefile., $(ALL))
	@echo "test list [$(words $(ALL)) item(s)]:" $(ALL)

$(ALL): %: Makefile.%

Makefile.%: tests/%.c latest
	@/bin/echo -e "NAME = $*\nSRCS = $<\ninclude $${AM_HOME}/Makefile" > $@
# Provide deterministic mouse events for tests that check MMIO side effects.
# The scripts make headless runs repeatable and avoid requiring real host input.
	@if [ "$*" = "jit-ldst-signext-asm" ]; then \
		NEMU_MOUSE_SCRIPT=move:17,23 make -s -f $@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	elif [ "$*" = "riscv-io-strict" ]; then \
		NEMU_MOUSE_SCRIPT='move:31,47;move:31,47;wheel:2,-3' make -s -f $@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	elif [ "$*" = "riscv-fast-mmu-priv-strict" ]; then \
		NEMU_DISABLE_JIT=1 make -s -f $@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	else \
		make -s -f $@ ARCH=$(ARCH) $(MAKECMDGOALS); \
	fi; \
	status=$$?; \
	if [ $$status -eq 0 ]; then \
		printf "[%14s] $(COLOR_GREEN)PASS$(COLOR_NONE)\n" $* >> $(RESULT); \
	else \
		printf "[%14s] $(COLOR_RED)***FAIL***$(COLOR_NONE)\n" $* >> $(RESULT); \
	fi
	-@rm -f Makefile.$*

run: all
	@cat $(RESULT)
	@rm $(RESULT)

gdb: all

clean:
	rm -rf Makefile.* build/

latest:
