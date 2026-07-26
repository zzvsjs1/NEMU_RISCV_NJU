CROSS_COMPILE = mips-linux-gnu-
MIPS32_VME_ENABLED := $(filter 1 y yes true,$(strip $(VME)))
LNK_ADDR = $(if $(MIPS32_VME_ENABLED),0x40000000,0x83000000)

# The paged C runtime needs to distinguish its initial stack contract from the
# direct-loader ABI.  Define the marker only for an explicitly enabled VME;
# values such as VME=0 and an empty VME remain direct-mode builds.
ifneq ($(MIPS32_VME_ENABLED),)
CFLAGS += -D__NAVY_VME__
endif

CFLAGS  += -fno-pic -march=mips32 -EL \
					 -fno-delayed-branch -mno-gpopt -mno-abicalls -mno-check-zero-division \
					 -mno-llsc -mno-imadd -mno-mad
CFLAGS  += -D_LDBL_EQ_DBL
LDFLAGS += -e_start -EL -Ttext-segment $(LNK_ADDR)
