ifeq ($(CONFIG_RV64_FPU),y)

# Keep the upstream checkout outside src/ so NEMU never discovers unrelated
# ARM, x86, binary16, extended, or binary128 implementations recursively.
# This explicit closure contains only the binary32/binary64 operations used by
# RV64 F/D and the helpers they reference with the platform options below.
SOFTFLOAT_REPO := tools/softfloat/repo
SOFTFLOAT_SOURCE := $(SOFTFLOAT_REPO)/source

SOFTFLOAT_CORE_SRCS := \
  f32_add.c f32_div.c f32_eq.c f32_le.c f32_lt.c f32_lt_quiet.c \
  f32_mul.c f32_mulAdd.c f32_sqrt.c f32_sub.c f32_to_f64.c \
  f32_to_i32.c f32_to_i64.c f32_to_ui32.c f32_to_ui64.c \
  f64_add.c f64_div.c f64_eq.c f64_le.c f64_lt.c f64_lt_quiet.c \
  f64_mul.c f64_mulAdd.c f64_sqrt.c f64_sub.c f64_to_f32.c \
  f64_to_i32.c f64_to_i64.c f64_to_ui32.c f64_to_ui64.c \
  i32_to_f32.c i32_to_f64.c i64_to_f32.c i64_to_f64.c \
  ui32_to_f32.c ui32_to_f64.c ui64_to_f32.c ui64_to_f64.c \
  s_addMagsF32.c s_addMagsF64.c \
  s_approxRecipSqrt32_1.c s_approxRecipSqrt_1Ks.c \
  s_countLeadingZeros8.c s_countLeadingZeros64.c s_mul64To128.c \
  s_mulAddF32.c s_mulAddF64.c \
  s_normRoundPackToF32.c s_normRoundPackToF64.c \
  s_normSubnormalF32Sig.c s_normSubnormalF64Sig.c \
  s_roundPackToF32.c s_roundPackToF64.c \
  s_roundToI32.c s_roundToI64.c s_roundToUI32.c s_roundToUI64.c \
  s_shiftRightJam128.c s_subMagsF32.c s_subMagsF64.c \
  softfloat_state.c

# These three files provide RISC-V exception accrual and canonical-NaN
# propagation. Other upstream specialisations have observably different NaN
# or conversion behaviour and must never be mixed into the guest model.
SOFTFLOAT_RISCV_SRCS := \
  softfloat_raiseFlags.c \
  s_propagateNaNF32UI.c \
  s_propagateNaNF64UI.c

SOFTFLOAT_SRCS := \
  $(addprefix $(SOFTFLOAT_SOURCE)/,$(SOFTFLOAT_CORE_SRCS)) \
  $(addprefix $(SOFTFLOAT_SOURCE)/RISCV/,$(SOFTFLOAT_RISCV_SRCS))

SRCS-y += $(SOFTFLOAT_SRCS)
INC_PATH += \
  $(NEMU_HOME)/src/softfloat \
  $(NEMU_HOME)/$(SOFTFLOAT_SOURCE)/include \
  $(NEMU_HOME)/$(SOFTFLOAT_SOURCE)/RISCV

# A fresh checkout has no ignored dependency directory. Give every upstream
# source and both paths used for the shared RV32/RV64 executor an explicit
# order-only preparation barrier. This makes first-time parallel builds wait
# for clone verification before Make checks sources or compiles softfloat.h.
.PHONY: nemu-softfloat-prepare
nemu-softfloat-prepare:
	+@$(MAKE) --no-print-directory -C $(NEMU_HOME)/tools/softfloat prepare

$(SOFTFLOAT_SRCS) \
src/isa/riscv32/fpu.c \
src/isa/riscv64/fpu.c: | nemu-softfloat-prepare

endif
