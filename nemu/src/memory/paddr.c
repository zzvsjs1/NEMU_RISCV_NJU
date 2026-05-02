#include <memory/host.h>
#include <memory/paddr.h>
#include <device/mmio.h>
#include <isa.h>
#include <utils.h>

#if   defined(CONFIG_TARGET_AM)
static uint8_t *pmem = NULL;
#else
static uint8_t pmem[CONFIG_MSIZE] PG_ALIGN = {};
#endif

uint8_t* guest_to_host(paddr_t paddr) 
{ 
    return pmem + paddr - CONFIG_MBASE; 
}

paddr_t host_to_guest(uint8_t *haddr) 
{ 
    return haddr - pmem + CONFIG_MBASE; 
}

static word_t pmem_read(paddr_t addr, int len) 
{
  word_t ret = host_read(guest_to_host(addr), len);
  return ret;
}

static void pmem_write(paddr_t addr, int len, word_t data) {
  host_write(guest_to_host(addr), len, data);
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
#if   defined(CONFIG_TARGET_AM)
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

word_t paddr_read(paddr_t addr, int len) 
{
  if (likely(in_pmem(addr))) 
  {
    word_t data = pmem_read(addr, len);
#ifdef CONFIG_MTRACE
    /* Log after the read so the trace includes the returned value. */
    if (mtrace_in_range(addr, len)) {
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
      cpu.pc
    )
  );
}

void paddr_write(paddr_t addr, int len, word_t data) {
  if (likely(in_pmem(addr))) {
#ifdef CONFIG_MTRACE
    /* Log before the write so the trace records the value being committed. */
    if (mtrace_in_range(addr, len)) {
      log_write("mtrace write pc=" FMT_WORD " addr=" FMT_PADDR " len=%d data=" FMT_WORD "\n",
          cpu.pc, addr, len, data);
    }
#endif
    pmem_write(addr, len, data);
    return;
  }
  MUXDEF(CONFIG_DEVICE, mmio_write(addr, len, data),
    panic("address = " FMT_PADDR " is out of bound of pmem [" FMT_PADDR ", " FMT_PADDR ") at pc = " FMT_WORD,
      addr, CONFIG_MBASE, CONFIG_MBASE + CONFIG_MSIZE, cpu.pc));
}
