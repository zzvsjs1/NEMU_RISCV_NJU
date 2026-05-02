#include <utils.h>

#ifndef CONFIG_TARGET_AM
#include <isa.h>
#include <memory/paddr.h>
#include <stdio.h>

extern uint64_t g_nr_guest_instr;

typedef struct {
  uint32_t magic;
  uint32_t version;
  paddr_t mbase;
  paddr_t msize;
  size_t cpu_size;
  size_t nemu_state_size;
  uint64_t nr_guest_instr;
} SnapshotHeader;

/*
 * Magic bytes identify files written by NEMU snapshots before we trust sizes.
 * The integer is built from ASCII "NEMU" so the value is not a bare hex number.
 */
#define SNAPSHOT_MAGIC \
  (((uint32_t)'N' << 0) | ((uint32_t)'E' << 8) | ((uint32_t)'M' << 16) | ((uint32_t)'U' << 24))

/* Bump this if SnapshotHeader layout or payload order changes. */
#define SNAPSHOT_VERSION 1

static bool write_exact(FILE *fp, const void *buf, size_t size) {
  return fwrite(buf, size, 1, fp) == 1;
}

static bool read_exact(FILE *fp, void *buf, size_t size) {
  return fread(buf, size, 1, fp) == 1;
}

void save_snapshot(const char *path) {
  if (path == NULL || path[0] == '\0') {
    printf("Usage: save [path]\n");
    return;
  }

  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    perror("save snapshot");
    return;
  }

  SnapshotHeader header = {
    .magic = SNAPSHOT_MAGIC,
    .version = SNAPSHOT_VERSION,
    .mbase = CONFIG_MBASE,
    .msize = CONFIG_MSIZE,
    .cpu_size = sizeof(cpu),
    .nemu_state_size = sizeof(nemu_state),
    .nr_guest_instr = g_nr_guest_instr,
  };

  /* Payload order is fixed by SNAPSHOT_VERSION: header, CPU, NEMU state, PMEM. */
  bool ok = write_exact(fp, &header, sizeof(header)) &&
            write_exact(fp, &cpu, sizeof(cpu)) &&
            write_exact(fp, &nemu_state, sizeof(nemu_state)) &&
            pmem_save(fp);

  if (fclose(fp) != 0) {
    ok = false;
  }

  printf("%s snapshot: %s\n", ok ? "Saved" : "Failed to save", path);
}

void load_snapshot(const char *path) {
  if (path == NULL || path[0] == '\0') {
    printf("Usage: load [path]\n");
    return;
  }

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    perror("load snapshot");
    return;
  }

  SnapshotHeader header;
  bool ok = read_exact(fp, &header, sizeof(header));
  /* Reject snapshots from a different format or physical-memory layout. */
  ok = ok &&
       header.magic == SNAPSHOT_MAGIC &&
       header.version == SNAPSHOT_VERSION &&
       header.mbase == (paddr_t)CONFIG_MBASE &&
       header.msize == (paddr_t)CONFIG_MSIZE &&
       header.cpu_size == sizeof(cpu) &&
       header.nemu_state_size == sizeof(nemu_state);

  CPU_state saved_cpu;
  NEMUState saved_state;
  if (ok) {
    ok = read_exact(fp, &saved_cpu, sizeof(saved_cpu)) &&
         read_exact(fp, &saved_state, sizeof(saved_state)) &&
         pmem_load(fp);
  }

  if (fclose(fp) != 0) {
    ok = false;
  }

  if (!ok) {
    printf("Failed to load snapshot: %s\n", path);
    return;
  }

  /* Install restored state only after the whole file has been validated. */
  cpu = saved_cpu;
  nemu_state = saved_state;
  g_nr_guest_instr = header.nr_guest_instr;
  printf("Loaded snapshot: %s\n", path);
}
#endif
