#include <common.h>
#include <device/map.h>
#include <memory/paddr.h>
#ifdef CONFIG_ISA_riscv32
#include <isa-jit.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define DISK_BLKSZ 512u
#define DISK_CMD_GO 1u
#define DISK_PATH_BUFSZ 4096
#define DISK_SOURCE_BUFSZ 64
#define PMEM_BASE CONFIG_MBASE

enum {
  reg_present,
  reg_blksz,
  reg_blkcnt,
  reg_ready,
  reg_write,
  reg_buf,
  reg_blkno,
  reg_io_blkcnt,
  reg_cmd,
  nr_reg,
};

static uint32_t *disk_base = NULL;
static FILE *disk_img = NULL;
static uint8_t *disk_map = NULL;
static size_t disk_img_size = 0;
static size_t disk_map_size = 0;
static uint32_t disk_blkcnt = 0;
static char disk_img_path[DISK_PATH_BUFSZ] = "";
static char disk_img_source[DISK_SOURCE_BUFSZ] = "";

static uint32_t blocks_for_size(size_t size)
{
  return (uint32_t)((size + DISK_BLKSZ - 1) / DISK_BLKSZ);
}

static void publish_disk_config(void)
{
  const bool present = disk_img != NULL && disk_blkcnt > 0;

  disk_base[reg_present] = present;
  disk_base[reg_blksz] = present ? DISK_BLKSZ : 0;
  disk_base[reg_blkcnt] = present ? disk_blkcnt : 0;
  disk_base[reg_ready] = 1;
}

static void unmap_disk_image(void)
{
  if (disk_map == NULL) {
    return;
  }

  int ret = munmap(disk_map, disk_map_size);
  Assert(ret == 0, "disk: munmap failed: %s", strerror(errno));

  disk_map = NULL;
  disk_map_size = 0;
}

static void map_disk_image(void)
{
  unmap_disk_image();

  if (disk_img == NULL || disk_img_size == 0) {
    return;
  }

  const int fd = fileno(disk_img);
  if (fd < 0) {
    Log("disk: cannot map %s image '%s': %s",
        disk_img_source, disk_img_path, strerror(errno));
    return;
  }

  void *map = mmap(NULL, disk_img_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    Log("disk: mmap disabled for %s image '%s': %s",
        disk_img_source, disk_img_path, strerror(errno));
    return;
  }

  disk_map = (uint8_t *)map;
  disk_map_size = disk_img_size;

  Log("disk: mapped %s image '%s' for fast reads, size = %zu bytes",
      disk_img_source, disk_img_path, disk_map_size);
}

static bool try_open_disk_image(const char *path, const char *source)
{
  if (path == NULL || path[0] == '\0') {
    return false;
  }

  FILE *fp = fopen(path, "r+b");
  if (fp == NULL) {
    Log("disk: cannot open %s image '%s': %s", source, path, strerror(errno));
    return false;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    Log("disk: cannot seek %s image '%s': %s", source, path, strerror(errno));
    fclose(fp);
    return false;
  }

  long size = ftell(fp);
  if (size <= 0) {
    Log("disk: ignore empty %s image '%s'", source, path);
    fclose(fp);
    return false;
  }

  rewind(fp);

  disk_img = fp;
  disk_img_size = (size_t)size;
  disk_blkcnt = blocks_for_size(disk_img_size);
  snprintf(disk_img_path, sizeof(disk_img_path), "%s", path);
  snprintf(disk_img_source, sizeof(disk_img_source), "%s", source);
  map_disk_image();
  Log("disk: using %s image '%s', size = %zu bytes, blocks = %u",
      source, path, disk_img_size, disk_blkcnt);
  return true;
}

static void open_disk_image(void)
{
  const char *navy_home = getenv("NAVY_HOME");
  char navy_path[DISK_PATH_BUFSZ];

  if (navy_home != NULL && navy_home[0] != '\0') {
    int n = snprintf(navy_path, sizeof(navy_path), "%s/build/ramdisk.img", navy_home);
    if (n > 0 && (size_t)n < sizeof(navy_path)) {
      if (try_open_disk_image(navy_path, "$NAVY_HOME/build/ramdisk.img")) {
        return;
      }
    } else {
      Log("disk: NAVY_HOME path is too long, skip normal disk image");
    }
  } else {
    Log("disk: NAVY_HOME is not set, skip normal disk image");
  }

  if (CONFIG_DISK_IMG_PATH[0] != '\0'
      && try_open_disk_image(CONFIG_DISK_IMG_PATH, "CONFIG_DISK_IMG_PATH")) {
    return;
  }

  Log("disk: no usable disk image, device is absent");
}

static uint8_t *guest_buffer_to_host(uint32_t guest_addr, size_t bytes, paddr_t *out_paddr)
{
  Assert(guest_addr >= PMEM_BASE,
      "disk: guest DMA address 0x%08x is below PMEM base 0x%08x",
      guest_addr, (uint32_t)PMEM_BASE);

  /*
   * The guest provides a PMEM address in the MMIO register.  Keep the offset
   * step explicit (`addr - PMEM_BASE`); this NEMU tree's guest_to_host()
   * accepts CONFIG_MBASE-based physical addresses, so the PMEM base is added
   * back after the range check has a normalised offset to reason about.
   */
  const paddr_t pmem_offset = (paddr_t)guest_addr - PMEM_BASE;
  const paddr_t paddr = (paddr_t)CONFIG_MBASE + pmem_offset;

  Assert(bytes <= CONFIG_MSIZE && paddr >= (paddr_t)CONFIG_MBASE
      && paddr + bytes <= (paddr_t)CONFIG_MBASE + CONFIG_MSIZE,
      "disk: guest DMA range [0x%08x, 0x%08x) is outside PMEM",
      guest_addr, guest_addr + (uint32_t)bytes);

  *out_paddr = paddr;
  return guest_to_host(paddr);
}

static void read_blocks(uint8_t *buf, uint32_t blkno, uint32_t blkcnt)
{
  const size_t bytes = (size_t)blkcnt * DISK_BLKSZ;
  const size_t offset = (size_t)blkno * DISK_BLKSZ;
  const size_t available = offset < disk_img_size
      ? (bytes < disk_img_size - offset ? bytes : disk_img_size - offset)
      : 0;

  if (available > 0) {
    if (disk_map != NULL && offset + available <= disk_map_size) {
      memcpy(buf, disk_map + offset, available);
    } else {
      int ret = fseek(disk_img, (long)offset, SEEK_SET);
      Assert(ret == 0, "disk: seek before read failed: %s", strerror(errno));

      size_t got = fread(buf, 1, available, disk_img);
      Assert(got == available,
          "disk: read failed at block %u count %u: %s",
          blkno, blkcnt, strerror(errno));
      clearerr(disk_img);
    }
  }

  /*
   * The ramdisk image size is not required to be a multiple of 512 bytes.
   * Only the short padded tail needs clearing; full-block reads should avoid
   * doubling the memory traffic with a redundant memset().
   */
  if (available < bytes) {
    memset(buf + available, 0, bytes - available);
  }
}

static void write_blocks(const uint8_t *buf, uint32_t blkno, uint32_t blkcnt)
{
  const size_t bytes = (size_t)blkcnt * DISK_BLKSZ;
  const size_t offset = (size_t)blkno * DISK_BLKSZ;
  const size_t end = ((size_t)blkno + blkcnt) * DISK_BLKSZ;

  int ret = fseek(disk_img, (long)offset, SEEK_SET);
  Assert(ret == 0, "disk: seek before write failed: %s", strerror(errno));

  size_t put = fwrite(buf, 1, bytes, disk_img);
  Assert(put == bytes, "disk: write failed at block %u count %u: %s",
      blkno, blkcnt, strerror(errno));
  Assert(fflush(disk_img) == 0, "disk: flush failed: %s", strerror(errno));

  /*
   * Keep the mapped read view coherent with guest writes.  Writes are uncommon
   * for the Navy ramdisk, but file-test uses them and later reads must observe
   * the updated bytes even if the host has not yet refreshed the shared page.
   */
  if (disk_map != NULL && offset + bytes <= disk_map_size) {
    memcpy(disk_map + offset, buf, bytes);
  }

  if (end > disk_img_size) {
    disk_img_size = end;
    disk_blkcnt = blocks_for_size(disk_img_size);
    map_disk_image();
    publish_disk_config();
  }
}

static void do_blkio(void)
{
  Assert(disk_img != NULL, "disk: guest requested I/O but no image is present");

  const uint32_t blkno = disk_base[reg_blkno];
  const uint32_t blkcnt = disk_base[reg_io_blkcnt];
  const size_t bytes = (size_t)blkcnt * DISK_BLKSZ;

  Assert(blkcnt > 0, "disk: block count must be positive");
  Assert(blkno <= disk_blkcnt && blkcnt <= disk_blkcnt - blkno,
      "disk: block range [%u, %u) exceeds block count %u",
      blkno, blkno + blkcnt, disk_blkcnt);

  disk_base[reg_ready] = 0;
  paddr_t dma_paddr = 0;
  uint8_t *buf = guest_buffer_to_host(disk_base[reg_buf], bytes, &dma_paddr);

  if (disk_base[reg_write]) {
    write_blocks(buf, blkno, blkcnt);
  } else {
    read_blocks(buf, blkno, blkcnt);
#ifdef CONFIG_ISA_riscv32
    Assert(bytes <= INT32_MAX, "disk: DMA read is too large for JIT invalidation");
    isa_jit_invalidate_paddr(dma_paddr, (int)bytes);
#endif
  }

  disk_base[reg_ready] = 1;
}

static void disk_io_handler(uint32_t offset, int len, bool is_write)
{
  Assert(offset % sizeof(uint32_t) == 0 && len == sizeof(uint32_t),
      "disk: only aligned 32-bit MMIO accesses are supported");

  const uint32_t reg = offset / sizeof(uint32_t);
  Assert(reg < nr_reg, "disk: register %u is outside the disk MMIO range", reg);

  if (is_write && reg == reg_cmd) {
    Assert(disk_base[reg_cmd] == DISK_CMD_GO,
        "disk: unsupported command %u", disk_base[reg_cmd]);
    do_blkio();
    disk_base[reg_cmd] = 0;
  }
}

void init_disk()
{
  const uint32_t space_size = sizeof(uint32_t) * nr_reg;
  disk_base = (uint32_t *)new_space(space_size);

  open_disk_image();
  publish_disk_config();

#ifdef CONFIG_HAS_PORT_IO
  add_pio_map("disk", CONFIG_DISK_CTL_PORT, disk_base, space_size,
              disk_io_handler);
#else
  add_mmio_map("disk", CONFIG_DISK_CTL_MMIO, disk_base, space_size,
               disk_io_handler);
#endif
}
