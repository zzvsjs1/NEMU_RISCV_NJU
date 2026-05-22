NAME = priv-test
SRCS = main.c

ifeq ($(filter clean clean-all html init,$(MAKECMDGOALS)),)
ifneq ($(ISA),x86)
$(error priv-test is x86-only because it uses x86 segment and EFLAGS inline assembly; got ISA=$(ISA))
endif
endif

include $(NAVY_HOME)/Makefile
