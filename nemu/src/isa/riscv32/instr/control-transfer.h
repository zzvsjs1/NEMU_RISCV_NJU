// Jump and link
// PAGE: 20
// The offset is sign-extended and added to the address of the
// jump instruction to form the jump target address
def_EHelper(jal)
{
	// Save next instuctions address to rd, and set new insturction address to pc.

	// Store next inst to ddest.
	rtl_addi(s, ddest, &s->pc, 4);

	// Sign Extend.
	rtl_li(s, s0, id_src1->imm);
	rtl_sign_ext_pos(s, s0, s0, 20);

	// Compute the inst address jump to.
	rtl_add(s, s0, &s->pc, s0);

	// Jump *s0
	rtl_jr(s, s0);
}

// Jump and link register
//
// The indirect jump instruction JALR (jump and link register) uses the I-type encoding. The target
// address is obtained by adding the sign-extended 12-bit I-immediate to the register rs1, then setting
// the least-significant bit of the result to zero. The address of the instruction following the jump
// (pc+4) is written to register rd. Register x0 can be used as the destination if the result is not
// required.
def_EHelper(jalr)
{
	// Store the next instuction(pc + 4) to ddest(rd).
	rtl_addi(s, ddest, &s->pc, 4);

	// Adding sign-extended 12-bit I-immediate to the register rs1.
	
	// s0 = imm[11:0].
	rtl_li(s, s0, id_src2->imm);

	// sign ext imm[11:0] in s0.
	rtl_sign_ext_pos(s, s0, s0, 11);

	// rd = src_1(rs1) (NOTE: The original register!) + s0(imm[11:0])
	rtl_add(s, s0, s0, dsrc1);

	// setting the least-significant bit of the result to zero
	rtl_srli(s, s0, s0, 1);
	rtl_slli(s, s0, s0, 1);

	// Jump to *s0.
	rtl_jr(s, s0);
}

// To return after handling a trap, there are separate trap return instructions per privilege level, MRET
// and SRET. MRET is always provided. SRET must be provided if supervisor mode is supported, and
// should raise an illegal-instruction exception otherwise. SRET should also raise an illegal-instruction
// exception when TSR=1 in mstatus, as described in Section 3.1.6.5. An xRET instruction can be
// executed in privilege mode x or higher, where executing a lower-privilege xRET instruction will pop
// the relevant lower-privilege interrupt enable and privilege mode stack. In addition to manipulating
// the privilege stack as described in Section 3.1.6.1, xRET sets the pc to the value stored in the xepc
// register.
def_EHelper(mret)
{
	// The MRET instruction is used to return from a trap taken into M-mode. MRET first determines what
    // the new privilege mode will be according to the values of MPP and MPV in mstatus or mstatush, as
    // encoded in Table 35. MRET then in mstatus/mstatush sets MPV=0, MPP=0, MIE=MPIE, and MPIE=1.
    // Lastly, MRET sets the privilege mode as previously determined, and sets pc=mepc

	// mstatus.MPP  = bits 12–11 (2 bits)
    // mstatus.MPIE = bit 7
    // mstatus.MIE  = bit 3

	// Step 1: Clear MPP (bits 12–11)
	// Clear both bits
	// I am lazy, just use C......
	cpu.csr.mstatus &= ~(0b11 << 11);

	// Step 2: Set MIE (bit 3) = MPIE (bit 7)
	const uint32_t mpie = (cpu.csr.mstatus >> 7) & 0x1;
	cpu.csr.mstatus = (cpu.csr.mstatus & ~(1 << 3)) | (mpie << 3);
	
	// Step 3: Set MPIE (bit 7) = 1
	cpu.csr.mstatus |= (1 << 7);

	// PC <- mepc
	// Just jump!
	rtl_jr(s, &cpu.csr.mepc);
}