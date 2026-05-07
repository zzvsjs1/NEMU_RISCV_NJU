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
# Provide one deterministic mouse event for jit-ldst-signext-asm, so the
# lw x0, 0(mouse_type) read has an observable side effect and the following
# read must see an empty queue.
	@if [ "$*" = "jit-ldst-signext-asm" ]; then \
		NEMU_MOUSE_SCRIPT=move:17,23 make -s -f $@ ARCH=$(ARCH) $(MAKECMDGOALS); \
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
