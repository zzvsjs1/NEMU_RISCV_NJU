NAME = pf-cause-test
SRCS = main.c

ifeq ($(filter clean clean-all html init,$(MAKECMDGOALS)),)
ifneq ($(ISA),x86)
$(error pf-cause-test is x86-only because it checks x86 page-fault event cause; got ISA=$(ISA))
endif
endif

include $(NAVY_HOME)/Makefile
