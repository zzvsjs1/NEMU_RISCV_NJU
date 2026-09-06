/*
 * This host-only fixture includes the implementation to exercise its private
 * predicate without adding a production testing interface. Normal NEMU builds
 * still compile the core as its own translation unit.
 */
#include "../../nemu/src/isa/riscv32/jit-rv64-internal.h"

#undef Assert
#define Assert(condition, ...) assert(condition)

#include "../../nemu/src/isa/riscv32/jit-rv64-core.c"

typedef enum
{
    LINK_ACCEPT_DIRECT,
    LINK_ACCEPT_THUNK,
    LINK_ACCEPT_DYNAMIC,
    LINK_ALREADY_PATCHED,
    LINK_INELIGIBLE,
    LINK_NO_SOURCE,
    LINK_INVALID_SOURCE,
    LINK_INVALID_TARGET,
    LINK_NO_ENTRY,
    LINK_NO_CHAIN_ENTRY,
    LINK_TRANSLATED,
    LINK_DATA_STATE,
    LINK_WRONG_PC,
    LINK_WRONG_SATP,
    LINK_WRONG_IFETCH,
    LINK_NO_GENERATION,
    LINK_WRONG_GENERATION,
    LINK_CASE_COUNT,
} link_case_t;

void rv64_jit_link_pilot_checks(void)
{
    static const char *const names[LINK_CASE_COUNT] = {
        "direct",         "thunk",          "dynamic",      "already-patched", "ineligible",       "no-source",
        "invalid-source", "invalid-target", "no-entry",     "no-chain-entry",  "translated",       "data-state",
        "wrong-pc",       "wrong-satp",     "wrong-ifetch", "no-generation",   "wrong-generation",
    };

    for (link_case_t test = 0; test < LINK_CASE_COUNT; test++)
    {
        uint8_t code[128];
        memset(code, 0xa5, sizeof(code));
        memset(&rv64_jit_stats, 0, sizeof(rv64_jit_stats));

        rv64_jit_block_t source = {.valid = true};
        rv64_jit_block_t target = {
            .valid = true,
            .entry = (rv64_jit_entry_t)(uintptr_t)(code + 96),
            .chain_entry = (rv64_jit_entry_t)(uintptr_t)(code + 112),
            .pc = UINT64_C(0x80000100),
            .satp = 0,
            .ifetch_state = 3,
            .generation = 7,
        };
        rv64_jit_link_t link = {
            .source = &source,
            .selector_disp = code + 16,
            .target_pc = target.pc,
            .target_satp = target.satp,
            .target_ifetch_state = target.ifetch_state,
            .target_generation = target.generation,
            .patch_eligible = true,
        };
        rv64_jit_block_t *target_arg = &target;

        switch (test)
        {
        case LINK_ACCEPT_DIRECT:
            /* A static link deliberately ignores dynamic generation identity. */
            link.target_generation = 0;
            break;
        case LINK_ACCEPT_THUNK:
            link.target_disp = code + 32;
            link.patched_path = code + 64;
            break;
        case LINK_ACCEPT_DYNAMIC:
            link.dynamic = true;
            break;
        case LINK_ALREADY_PATCHED:
            link.patched = true;
            link.source = NULL;
            target_arg = NULL;
            break;
        case LINK_INELIGIBLE:
            link.patch_eligible = false;
            link.source = NULL;
            target_arg = NULL;
            break;
        case LINK_NO_SOURCE:
            link.source = NULL;
            target_arg = NULL;
            break;
        case LINK_INVALID_SOURCE:
            source.valid = false;
            target_arg = NULL;
            break;
        case LINK_INVALID_TARGET:
            target.valid = false;
            break;
        case LINK_NO_ENTRY:
            target.entry = NULL;
            break;
        case LINK_NO_CHAIN_ENTRY:
            target.chain_entry = NULL;
            break;
        case LINK_TRANSLATED:
            target.translated = true;
            break;
        case LINK_DATA_STATE:
            target.uses_data_state = true;
            break;
        case LINK_WRONG_PC:
            target.pc += RISCV_BASE_INSN_BYTES;
            break;
        case LINK_WRONG_SATP:
            target.satp++;
            break;
        case LINK_WRONG_IFETCH:
            target.ifetch_state++;
            break;
        case LINK_NO_GENERATION:
            link.dynamic = true;
            link.target_generation = 0;
            break;
        case LINK_WRONG_GENERATION:
            link.dynamic = true;
            link.target_generation++;
            break;
        case LINK_CASE_COUNT:
            abort();
        }

        jit_link_try_patch(&link, target_arg);
        const bool accepted = test <= LINK_ACCEPT_DYNAMIC;
        assert(link.patched == (accepted || test == LINK_ALREADY_PATCHED));

        if (accepted)
        {
            int32_t displacement;
            memcpy(&displacement, link.selector_disp, sizeof(displacement));
            const uint8_t *destination = test == LINK_ACCEPT_THUNK ? code + 64 : code + 112;
            assert(displacement == destination - (link.selector_disp + sizeof(displacement)));

            if (test == LINK_ACCEPT_THUNK)
            {
                memcpy(&displacement, link.target_disp, sizeof(displacement));
                assert(displacement == (code + 112) - (link.target_disp + sizeof(displacement)));
            }
        }

        for (size_t i = 0; i < sizeof(code); i++)
        {
            const bool selector_byte = accepted && i >= 16 && i < 16 + sizeof(int32_t);
            const bool target_byte = test == LINK_ACCEPT_THUNK && i >= 32 && i < 32 + sizeof(int32_t);

            if (!selector_byte && !target_byte)
            {
                assert(code[i] == 0xa5);
            }
        }

#if RV64_JIT_STATS
        assert(rv64_jit_stats.direct_link_patch_resolutions == (uint64_t)(accepted && !link.dynamic));
        assert(rv64_jit_stats.indirect_pic_patch_resolutions[0] == (uint64_t)(accepted && link.dynamic));
#else
        assert(rv64_jit_stats.direct_link_patch_resolutions == 0);
        assert(rv64_jit_stats.indirect_pic_patch_resolutions[0] == 0);
#endif
        printf("link %s accepted=%u\n", names[test], accepted);
    }
}
