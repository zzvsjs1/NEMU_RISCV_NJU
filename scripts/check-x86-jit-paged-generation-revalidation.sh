#!/usr/bin/env bash
set -euo pipefail

jit="nemu/src/isa/x86/jit.c"

fail() {
  echo "x86 paged generation revalidation check failed: $*" >&2
  exit 1
}

[[ -f "$jit" ]] || fail "missing $jit"

hash_body=$(awk '
  /^static uint32_t jit_translation_key_hash/ { in_body = 1 }
  in_body { print }
  in_body && /^}/ { exit }
' "$jit")

if grep -q 'paging_generation' <<<"$hash_body"; then
  fail "paged cache hash still includes paging_generation"
fi

invalidate_body=$(awk '
  /^void isa_jit_invalidate_paddr/ { in_body = 1 }
  in_body { print }
  in_body && /^}/ { exit }
' "$jit")

if grep -q 'jit_invalidate_paged_blocks' <<<"$invalidate_body"; then
  fail "page-table writes still invalidate every paged block"
fi

grep -q 'jit_block_revalidate_paging_generation' "$jit" \
  || fail "missing paged source revalidation after generation bumps"

grep -q 'block->translation_key.paging_generation = jit_paging_generation' "$jit" \
  || fail "revalidated blocks do not refresh their paging generation"

echo "x86 paged generation revalidation check passed"
