#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

require_literal() {
  local file="$1"
  local text="$2"
  local description="$3"

  if ! grep -Fq "$text" "$file"; then
    fail "$description"
  fi
}

require_literal fceux-am/src/x6502.cpp "writeb(A, V);" "CPU write helper missing"
require_literal fceux-am/src/x6502.cpp "_DB = V;" "CPU writes must refresh the open-bus data latch"

require_literal fceux-am/src/ops.inc "GetABIWR(A,_X); A = ((_Y&((A>>8)+1)) << 8) | (A & 0xff); WrMem(A,A>>8);" "SYA illegal opcode fix missing"
require_literal fceux-am/src/ops.inc "GetABIWR(A,_Y); A = ((_X&((A>>8)+1)) << 8) | (A & 0xff); WrMem(A,A>>8);" "SXA illegal opcode fix missing"

require_literal fceux-am/src/ppu.cpp "int start=0;" "Old PPU sprite copy must honour the left-edge sprite mask"
require_literal fceux-am/src/ppu.cpp "if(PPU[1] & 0x04)" "Old PPU sprite copy must test the sprite left-edge mask bit"
require_literal fceux-am/src/ppu.cpp "for(int i=start;i<256;i++)" "Old PPU sprite copy must skip masked left-edge pixels"

require_literal fceux-am/src/ppu.cpp "#define READPALNOGS(ofs)" "New PPU needs raw palette reads before per-pixel grayscale"
require_literal fceux-am/src/ppu.cpp "uint8 ppu1[8];" "New PPU must sample PPU mask bits for each fetched pixel"
require_literal fceux-am/src/ppu.cpp "if(bgdata.main[xt+2].ppu1[xp]&1)" "New PPU must apply grayscale per pixel"

require_literal fceux-am/src/ppu.cpp "FCEU_MemoryRand(NTARAM, 0x800, true);" "PPU nametable power-on memory initialisation missing"
require_literal fceux-am/src/ppu.cpp "PALRAM[0x1C] = PALRAM[0x18] = PALRAM[0x14] = PALRAM[0x10];" "PPU palette mirror initialisation missing"

require_literal fceux-am/src/cart.cpp "if (CHRmask1[chip] >= (unsigned int)(-1)) CHRmask1[chip] = 0;" "CHR 1 KiB mask underflow guard missing"
require_literal fceux-am/src/ines.cpp "int mCHRRAMSize = (CHRRAMSize < 1024) ? 1024 : CHRRAMSize;" "iNES CHR-RAM minimum allocation missing"

require_literal fceux-am/src/ines.cpp "int rom_size_bytes = 0;" "iNES loader must track PRG size in bytes"
require_literal fceux-am/src/ines.cpp "int vrom_size_bytes = 0;" "iNES loader must track CHR size in bytes"
require_literal fceux-am/src/ines.cpp "FCEU_fread(ROM, 1, (round) ? rom_size_bytes : not_round_size, fp);" "iNES loader must read byte-accurate PRG data"
require_literal fceux-am/src/ines.cpp "SetupCartCHRMapping(0, VROM, vrom_size_bytes, 0);" "iNES loader must map byte-accurate CHR data"

require_literal fceux-am/src/boards/mmc5.cpp "setprg32(0x8000, ((PRGBanks[3] & 0x7F) >> 2));" "MMC5 32 KiB PRG mode must use register 5117"
require_literal fceux-am/src/boards/mmc5.cpp "for (x = 0; x < 8; x++) CHRBanksA[x] = 0;" "MMC5 CHR bank reset values must be zero"
require_literal fceux-am/src/boards/mmc5.cpp "NTAMirroring = NTFill = ATFill = 0;" "MMC5 nametable fill reset values must be zero"

require_literal fceux-am/src/boards/mmc3.cpp "ws = (info->wram_size + info->battery_wram_size) / 1024;" "MMC3 must honour NES 2.0 WRAM size"
require_literal fceux-am/src/boards/bandai.cpp "if ((x24c0x_addr & 0x78) != 0x50)" "Bandai X24C02 must reject wrong I2C device addresses"
require_literal fceux-am/src/boards/bandai.cpp "SetWriteHandler(0x6000, 0xFFFF, BandaiWrite);" "Bandai mapper 16 submapper writes must include 0x6000-0x7fff"
require_literal fceux-am/src/boards/datalatch.cpp "info->ines2 && info->submapper == 2" "Mapper 2/3 bus conflicts must be gated by NES 2.0 submapper 2"
require_literal fceux-am/src/boards/71.cpp "static int hardmirr;" "Mapper 71 must preserve hardwired mirroring"
require_literal fceux-am/src/boards/n106.cpp "LengthCache[w] = 256 - (V & 0xFC);" "N106 audio length cache fix missing"
require_literal fceux-am/src/boards/n106.cpp "SyncMirror();" "Mapper 19 restore must sync mirroring before FixNTAR"
require_literal fceux-am/src/boards/vrc2and4.cpp "IRQMode = V & 4;" "VRC2/4 cycle IRQ mode missing"
require_literal fceux-am/src/boards/vrc7.cpp "IRQMode = V & 4;" "VRC7 cycle IRQ mode missing"

printf 'fceux-am core port checks passed\n'
