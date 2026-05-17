#include <memory/host.h>
#include <memory/paddr.h>
#include <device/mmio.h>
#include <isa.h>
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64)
#include <isa-jit.h>
#endif
#include <utils.h>

#if defined(CONFIG_TARGET_AM)
static uint8_t *pmem = NULL;
#else
static uint8_t pmem[CONFIG_MSIZE] PG_ALIGN = {};
#endif

static inline uint8_t *pmem_host_addr(paddr_t paddr)
{
    return pmem + paddr - CONFIG_MBASE;
}

uint8_t *guest_to_host(paddr_t paddr)
{
    return pmem_host_addr(paddr);
}

paddr_t host_to_guest(uint8_t *haddr)
{
    return haddr - pmem + CONFIG_MBASE;
}

static word_t pmem_read(paddr_t addr, int len)
{
    return host_read(pmem_host_addr(addr), len);
}

static void pmem_write(paddr_t addr, int len, word_t data)
{
    host_write(pmem_host_addr(addr), len, data);
}

#ifndef CONFIG_TARGET_AM
/* Snapshot files store the whole physical memory array after CPU state. */
bool pmem_save(FILE *fp)
{
    return fwrite(pmem, CONFIG_MSIZE, 1, fp) == 1;
}

bool pmem_load(FILE *fp)
{
    return fread(pmem, CONFIG_MSIZE, 1, fp) == 1;
}
#endif

void init_mem()
{
#if defined(CONFIG_TARGET_AM)
    pmem = malloc(CONFIG_MSIZE);
    assert(pmem);
#endif

    IFDEF(CONFIG_MEM_RANDOM, memset(pmem, rand(), CONFIG_MSIZE));
    Log("physical memory area [" FMT_PADDR ", " FMT_PADDR "]",
        (paddr_t)CONFIG_MBASE, (paddr_t)CONFIG_MBASE + CONFIG_MSIZE);
}

#ifdef CONFIG_MTRACE
static bool mtrace_in_range(paddr_t addr, int len)
{
    /* Treat the traced access as the half-open interval [addr, addr + len). */
    paddr_t end = addr + len;
    return addr >= (paddr_t)CONFIG_MTRACE_START && end <= (paddr_t)CONFIG_MTRACE_END;
}
#endif

word_t paddr_ifetch(paddr_t addr)
{
    if (likely(in_pmem_range(addr, 4)))
    {
        word_t data = *(uint32_t *)pmem_host_addr(addr);
#ifdef CONFIG_MTRACE
        if (mtrace_in_range(addr, 4))
        {
            log_write("mtrace read  pc=" FMT_WORD " addr=" FMT_PADDR " len=4 data=" FMT_WORD "\n",
                      cpu.pc, addr, data);
        }
#endif
        return data;
    }

    return paddr_read(addr, 4);
}

word_t paddr_read(paddr_t addr, int len)
{
    if (likely(in_pmem_range(addr, len)))
    {
        word_t data = pmem_read(addr, len);
#ifdef CONFIG_MTRACE
        /* Log after the read so the trace includes the returned value. */

        if (mtrace_in_range(addr, len))
        {
            log_write("mtrace read  pc=" FMT_WORD " addr=" FMT_PADDR " len=%d data=" FMT_WORD "\n",
                      cpu.pc, addr, len, data);
        }
#endif
        return data;
    }

    MUXDEF(
        CONFIG_DEVICE,
        return mmio_read(addr, len),
        panic("address = " FMT_PADDR " is out of bound of pmem [" FMT_PADDR ", " FMT_PADDR ") at pc = " FMT_WORD,
              addr,
              CONFIG_MBASE,
              CONFIG_MBASE + CONFIG_MSIZE,
              cpu.pc));
}

void paddr_write(paddr_t addr, int len, word_t data)
{
    if (likely(in_pmem_range(addr, len)))
    {
#ifdef CONFIG_MTRACE
        /* Log before the write so the trace records the value being committed. */

        if (mtrace_in_range(addr, len))
        {
            log_write("mtrace write pc=" FMT_WORD " addr=" FMT_PADDR " len=%d data=" FMT_WORD "\n",
                      cpu.pc, addr, len, data);
        }
#endif
        pmem_write(addr, len, data);
#if defined(CONFIG_ISA_riscv32) || defined(CONFIG_ISA_riscv64)
        /*
         * PMEM writes are the common meeting point for interpreter stores and
         * any device path that writes through paddr_write(). Tell the JIT about
         * the physical byte range after the new value is visible, so a later
         * fetch, block lookup, or JIT-local translation cannot use stale
         * PMEM state.
         *
         * Do not move this above pmem_write(): a translated block discarded
         * because of self-modifying code must see the replacement instruction
         * bytes if it is compiled again immediately afterwards.
         */
        if (unlikely(isa_jit_invalidation_active))
        {
            isa_jit_invalidate_paddr(addr, len);
        }
#endif
        return;
    }
    MUXDEF(CONFIG_DEVICE, mmio_write(addr, len, data),
           panic("address = " FMT_PADDR " is out of bound of pmem [" FMT_PADDR ", " FMT_PADDR ") at pc = " FMT_WORD,
                 addr, CONFIG_MBASE, CONFIG_MBASE + CONFIG_MSIZE, cpu.pc));
}
