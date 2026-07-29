# SoftFloat integration for RV32F, RV32D, and RV64F/D

The arithmetic sources are not vendored in the parent repository. They come
from the pristine checkout managed by `tools/softfloat/Makefile`:

- repository: `https://github.com/ucb-bar/berkeley-softfloat-3.git`
- release: Berkeley SoftFloat 3e
- commit: `f74b1e48110ac3a27dd49b787d164e55e42d81d1`
- specialisation: `source/RISCV`

Run `make -C tools/softfloat prepare` from the NEMU directory to initialise or
verify the checkout.

`filelist.mk` selects the F32/F64 dependency closure required by the shared
RV32F, RV32D, and RV64F/D executor. RV32F uses the F32 paths. Both D
configurations additionally use the F64 and cross-precision paths; XLEN does
not change that arithmetic dependency. FCLASS is implemented in NEMU because
it is an architectural RISC-V operation, not part of the upstream SoftFloat
interface.
