SOFTFLOAT_REPO_URL ?= https://github.com/ucb-bar/berkeley-softfloat-3.git
SOFTFLOAT_REPO_COMMIT := f74b1e48110ac3a27dd49b787d164e55e42d81d1
SOFTFLOAT_REPO_PATH := repo

.PHONY: prepare verify clean

prepare:
	@if [ -e "$(SOFTFLOAT_REPO_PATH)" ] && \
	    [ ! -d "$(SOFTFLOAT_REPO_PATH)/.git" ]; then \
	  echo "SoftFloat path exists but is not a Git checkout: $(SOFTFLOAT_REPO_PATH)" >&2; \
	  exit 1; \
	fi
	@if [ ! -d "$(SOFTFLOAT_REPO_PATH)/.git" ]; then \
	  git clone "$(SOFTFLOAT_REPO_URL)" "$(SOFTFLOAT_REPO_PATH)"; \
	  git -C "$(SOFTFLOAT_REPO_PATH)" checkout --detach \
	    "$(SOFTFLOAT_REPO_COMMIT)"; \
	fi
	@$(MAKE) --no-print-directory verify

verify:
	@if [ ! -d "$(SOFTFLOAT_REPO_PATH)/.git" ]; then \
	  echo "SoftFloat is not initialised; run 'make -C nemu/tools/softfloat prepare'." >&2; \
	  exit 1; \
	fi
	@actual=$$(git -C "$(SOFTFLOAT_REPO_PATH)" rev-parse HEAD); \
	if [ "$$actual" != "$(SOFTFLOAT_REPO_COMMIT)" ]; then \
	  echo "SoftFloat checkout is $$actual; expected $(SOFTFLOAT_REPO_COMMIT)." >&2; \
	  exit 1; \
	fi
	@if [ -n "$$(git -C "$(SOFTFLOAT_REPO_PATH)" status --porcelain)" ]; then \
	  echo "SoftFloat checkout has local changes; refusing to build it." >&2; \
	  exit 1; \
	fi
	@test -f "$(SOFTFLOAT_REPO_PATH)/source/include/softfloat.h"
	@test -f "$(SOFTFLOAT_REPO_PATH)/source/RISCV/specialize.h"

# NEMU's clean-all discovers every tool Makefile. Preserve the immutable,
# reusable dependency checkout; prepare/verify will reject any later drift.
clean:
	@:
