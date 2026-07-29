# Berkeley SoftFloat dependency

NEMU's RV32F, RV32D, and RV64F/D interpreters use Berkeley SoftFloat Release
3e from:

- repository: `https://github.com/ucb-bar/berkeley-softfloat-3.git`
- commit: `f74b1e48110ac3a27dd49b787d164e55e42d81d1`
- licence: BSD-3-Clause; see `LICENSE`

Initialise it with:

```bash
make -C nemu/tools/softfloat prepare
```

The clone is ignored by the parent repository. Builds write objects only below
`nemu/build`, so the upstream checkout remains pristine and can be reused for
offline rebuilds after `make clean`.

The pinned Git commit is used instead of the website's `SoftFloat-3e.zip`
because the Git tree contains Berkeley's `source/RISCV` specialisation.
