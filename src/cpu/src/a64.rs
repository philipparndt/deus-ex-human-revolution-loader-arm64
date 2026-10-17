//! Minimal AArch64 instruction encoder for the JIT.
//!
//! Registers are plain numbers 0..31 (31 = zr/sp depending on instruction).
//! `w` variants operate on 32 bits, `x` variants on 64 bits.

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Cond {
    Eq = 0,
    Ne = 1,
    Hs = 2, // carry set
    Lo = 3, // carry clear
    Mi = 4,
    Pl = 5,
    Vs = 6,
    Vc = 7,
    Hi = 8,
    Ls = 9,
    Ge = 10,
    Lt = 11,
    Gt = 12,
    Le = 13,
    Al = 14,
}

impl Cond {
    pub fn invert(self) -> Cond {
        unsafe { std::mem::transmute((self as u8) ^ 1) }
    }
}

pub const XZR: u32 = 31;
pub const SP: u32 = 31;

pub struct Asm {
    pub code: Vec<u32>,
}

/// A forward branch to be patched once the target is known.
#[derive(Clone, Copy)]
pub struct Fixup {
    pos: usize,
    kind: u8, // 0 = b/bl imm26, 1 = b.cond/cbz imm19, 2 = tbz imm14
}

impl Asm {
    pub fn new() -> Self {
        Asm { code: Vec::with_capacity(256) }
    }
    #[inline]
    pub fn emit(&mut self, insn: u32) {
        self.code.push(insn);
    }
    pub fn pos(&self) -> usize {
        self.code.len()
    }

    // --- moves / immediates ---------------------------------------------------------------
    pub fn movz(&mut self, sf: bool, rd: u32, imm16: u32, shift: u32) {
        self.emit(((sf as u32) << 31) | 0x5280_0000 | ((shift / 16) << 21) | ((imm16 & 0xffff) << 5) | rd);
    }
    pub fn movk(&mut self, sf: bool, rd: u32, imm16: u32, shift: u32) {
        self.emit(((sf as u32) << 31) | 0x7280_0000 | ((shift / 16) << 21) | ((imm16 & 0xffff) << 5) | rd);
    }
    pub fn movn(&mut self, sf: bool, rd: u32, imm16: u32, shift: u32) {
        self.emit(((sf as u32) << 31) | 0x1280_0000 | ((shift / 16) << 21) | ((imm16 & 0xffff) << 5) | rd);
    }
    /// Load a 32-bit constant into wN.
    pub fn mov_w_imm(&mut self, rd: u32, v: u32) {
        if v & 0xffff_0000 == 0 {
            self.movz(false, rd, v, 0);
        } else if v & 0xffff == 0 {
            self.movz(false, rd, v >> 16, 16);
        } else if !v & 0xffff_0000 == 0 {
            self.movn(false, rd, !v & 0xffff, 0);
        } else {
            self.movz(false, rd, v & 0xffff, 0);
            self.movk(false, rd, v >> 16, 16);
        }
    }
    /// Load a 64-bit constant into xN.
    pub fn mov_x_imm(&mut self, rd: u32, v: u64) {
        self.movz(true, rd, (v & 0xffff) as u32, 0);
        for s in [16u32, 32, 48] {
            let part = ((v >> s) & 0xffff) as u32;
            if part != 0 {
                self.movk(true, rd, part, s);
            }
        }
    }
    /// mov wd, wn (orr wd, wzr, wn)
    pub fn mov_w(&mut self, rd: u32, rn: u32) {
        self.emit(0x2a00_03e0 | (rn << 16) | rd);
    }
    pub fn mov_x(&mut self, rd: u32, rn: u32) {
        self.emit(0xaa00_03e0 | (rn << 16) | rd);
    }
    /// mov xd, sp / mov sp, xn (add with imm 0)
    pub fn mov_sp(&mut self, rd: u32, rn: u32) {
        self.emit(0x9100_0000 | (rn << 5) | rd);
    }

    // --- arithmetic -------------------------------------------------------------------------
    fn addsub_reg(&mut self, sf: bool, op: u32, s: bool, rd: u32, rn: u32, rm: u32, shift: u32, amount: u32) {
        self.emit(
            ((sf as u32) << 31) | (op << 30) | ((s as u32) << 29) | 0x0b00_0000 | (shift << 22) | (rm << 16)
                | (amount << 10) | (rn << 5) | rd,
        );
    }
    pub fn add_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.addsub_reg(false, 0, false, rd, rn, rm, 0, 0);
    }
    pub fn adds_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.addsub_reg(false, 0, true, rd, rn, rm, 0, 0);
    }
    pub fn sub_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.addsub_reg(false, 1, false, rd, rn, rm, 0, 0);
    }
    pub fn subs_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.addsub_reg(false, 1, true, rd, rn, rm, 0, 0);
    }
    pub fn add_x(&mut self, rd: u32, rn: u32, rm: u32) {
        self.addsub_reg(true, 0, false, rd, rn, rm, 0, 0);
    }
    /// add wd, wn, wm, lsl #amount
    pub fn add_w_lsl(&mut self, rd: u32, rn: u32, rm: u32, amount: u32) {
        self.addsub_reg(false, 0, false, rd, rn, rm, 0, amount);
    }
    /// adcs / sbcs (flag-setting with carry)
    pub fn adcs_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x3a00_0000 | (rm << 16) | (rn << 5) | rd);
    }
    pub fn sbcs_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x7a00_0000 | (rm << 16) | (rn << 5) | rd);
    }
    fn addsub_imm(&mut self, sf: bool, op: u32, s: bool, rd: u32, rn: u32, imm12: u32, lsl12: bool) {
        self.emit(
            ((sf as u32) << 31) | (op << 30) | ((s as u32) << 29) | 0x1100_0000 | ((lsl12 as u32) << 22)
                | ((imm12 & 0xfff) << 10) | (rn << 5) | rd,
        );
    }
    pub fn add_w_imm(&mut self, rd: u32, rn: u32, imm: u32) {
        self.addsub_imm(false, 0, false, rd, rn, imm, false);
    }
    pub fn sub_w_imm(&mut self, rd: u32, rn: u32, imm: u32) {
        self.addsub_imm(false, 1, false, rd, rn, imm, false);
    }
    pub fn subs_w_imm(&mut self, rd: u32, rn: u32, imm: u32) {
        self.addsub_imm(false, 1, true, rd, rn, imm, false);
    }
    pub fn add_x_imm(&mut self, rd: u32, rn: u32, imm: u32) {
        self.addsub_imm(true, 0, false, rd, rn, imm, false);
    }
    pub fn sub_x_imm(&mut self, rd: u32, rn: u32, imm: u32) {
        self.addsub_imm(true, 1, false, rd, rn, imm, false);
    }
    pub fn cmp_w(&mut self, rn: u32, rm: u32) {
        self.subs_w(XZR, rn, rm);
    }

    // --- logic ---------------------------------------------------------------------------------
    fn logic_reg(&mut self, sf: bool, opc: u32, n: u32, rd: u32, rn: u32, rm: u32) {
        self.emit(((sf as u32) << 31) | (opc << 29) | 0x0a00_0000 | (n << 21) | (rm << 16) | (rn << 5) | rd);
    }
    pub fn and_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.logic_reg(false, 0, 0, rd, rn, rm);
    }
    pub fn orr_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.logic_reg(false, 1, 0, rd, rn, rm);
    }
    pub fn eor_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.logic_reg(false, 2, 0, rd, rn, rm);
    }
    pub fn ands_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.logic_reg(false, 3, 0, rd, rn, rm);
    }
    pub fn bic_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.logic_reg(false, 0, 1, rd, rn, rm);
    }
    pub fn mvn_w(&mut self, rd: u32, rm: u32) {
        self.logic_reg(false, 1, 1, rd, XZR, rm);
    }
    pub fn tst_w(&mut self, rn: u32, rm: u32) {
        self.ands_w(XZR, rn, rm);
    }

    // --- shifts / bitfields -----------------------------------------------------------------------
    pub fn lslv_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x1ac0_2000 | (rm << 16) | (rn << 5) | rd);
    }
    pub fn lsrv_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x1ac0_2400 | (rm << 16) | (rn << 5) | rd);
    }
    pub fn asrv_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x1ac0_2800 | (rm << 16) | (rn << 5) | rd);
    }
    pub fn rorv_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x1ac0_2c00 | (rm << 16) | (rn << 5) | rd);
    }
    /// ubfm wd, wn, #immr, #imms
    pub fn ubfm_w(&mut self, rd: u32, rn: u32, immr: u32, imms: u32) {
        self.emit(0x5300_0000 | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    }
    pub fn sbfm_w(&mut self, rd: u32, rn: u32, immr: u32, imms: u32) {
        self.emit(0x1300_0000 | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    }
    pub fn bfm_w(&mut self, rd: u32, rn: u32, immr: u32, imms: u32) {
        self.emit(0x3300_0000 | (immr << 16) | (imms << 10) | (rn << 5) | rd);
    }
    pub fn lsl_w_imm(&mut self, rd: u32, rn: u32, sh: u32) {
        self.ubfm_w(rd, rn, (32 - sh) & 31, 31 - sh);
    }
    pub fn lsr_w_imm(&mut self, rd: u32, rn: u32, sh: u32) {
        self.ubfm_w(rd, rn, sh, 31);
    }
    pub fn asr_w_imm(&mut self, rd: u32, rn: u32, sh: u32) {
        self.sbfm_w(rd, rn, sh, 31);
    }
    /// ubfx wd, wn, #lsb, #width
    pub fn ubfx_w(&mut self, rd: u32, rn: u32, lsb: u32, width: u32) {
        self.ubfm_w(rd, rn, lsb, lsb + width - 1);
    }
    pub fn sbfx_w(&mut self, rd: u32, rn: u32, lsb: u32, width: u32) {
        self.sbfm_w(rd, rn, lsb, lsb + width - 1);
    }
    /// bfi wd, wn, #lsb, #width (insert low bits of wn into wd)
    pub fn bfi_w(&mut self, rd: u32, rn: u32, lsb: u32, width: u32) {
        self.bfm_w(rd, rn, (32 - lsb) & 31, width - 1);
    }
    pub fn uxtb_w(&mut self, rd: u32, rn: u32) {
        self.ubfm_w(rd, rn, 0, 7);
    }
    pub fn uxth_w(&mut self, rd: u32, rn: u32) {
        self.ubfm_w(rd, rn, 0, 15);
    }
    pub fn sxtb_w(&mut self, rd: u32, rn: u32) {
        self.sbfm_w(rd, rn, 0, 7);
    }
    pub fn sxth_w(&mut self, rd: u32, rn: u32) {
        self.sbfm_w(rd, rn, 0, 15);
    }
    pub fn rev_w(&mut self, rd: u32, rn: u32) {
        self.emit(0x5ac0_0800 | (rn << 5) | rd);
    }
    pub fn clz_w(&mut self, rd: u32, rn: u32) {
        self.emit(0x5ac0_1000 | (rn << 5) | rd);
    }
    pub fn rbit_w(&mut self, rd: u32, rn: u32) {
        self.emit(0x5ac0_0000 | (rn << 5) | rd);
    }

    // --- multiply / divide -------------------------------------------------------------------------
    pub fn mul_w(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x1b00_7c00 | (rm << 16) | (rn << 5) | rd);
    }
    pub fn smull(&mut self, xd: u32, wn: u32, wm: u32) {
        self.emit(0x9b20_7c00 | (wm << 16) | (wn << 5) | xd);
    }
    pub fn umull(&mut self, xd: u32, wn: u32, wm: u32) {
        self.emit(0x9ba0_7c00 | (wm << 16) | (wn << 5) | xd);
    }
    pub fn udiv_x(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x9ac0_0800 | (rm << 16) | (rn << 5) | rd);
    }
    pub fn sdiv_x(&mut self, rd: u32, rn: u32, rm: u32) {
        self.emit(0x9ac0_0c00 | (rm << 16) | (rn << 5) | rd);
    }
    pub fn msub_x(&mut self, rd: u32, rn: u32, rm: u32, ra: u32) {
        self.emit(0x9b00_8000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
    }
    pub fn lsr_x_imm(&mut self, rd: u32, rn: u32, sh: u32) {
        // ubfm xd, xn, #sh, #63
        self.emit(0xd340_fc00 | (sh << 16) | (rn << 5) | rd);
    }

    // --- conditional ------------------------------------------------------------------------------
    pub fn csel_w(&mut self, rd: u32, rn: u32, rm: u32, c: Cond) {
        self.emit(0x1a80_0000 | (rm << 16) | ((c as u32) << 12) | (rn << 5) | rd);
    }
    /// cset wd, cond  (csinc wd, wzr, wzr, !cond)
    pub fn cset_w(&mut self, rd: u32, c: Cond) {
        self.emit(0x1a9f_07e0 | (((c.invert()) as u32) << 12) | rd);
    }

    // --- loads / stores ------------------------------------------------------------------------------
    /// size: 0 = byte, 1 = half, 2 = word, 3 = dword. Address: [xn, wm, uxtw]
    pub fn ldr_uxtw(&mut self, size: u32, rt: u32, xn: u32, wm: u32) {
        self.emit((size << 30) | 0x3860_4800 | (wm << 16) | (rn_check(xn) << 5) | rt);
    }
    pub fn str_uxtw(&mut self, size: u32, rt: u32, xn: u32, wm: u32) {
        self.emit((size << 30) | 0x3820_4800 | (wm << 16) | (rn_check(xn) << 5) | rt);
    }
    /// Signed byte/half loads into a w register: [xn, wm, uxtw]
    pub fn ldrs_w_uxtw(&mut self, size: u32, rt: u32, xn: u32, wm: u32) {
        self.emit((size << 30) | 0x38e0_4800 | (wm << 16) | (xn << 5) | rt);
    }
    /// [xn, #imm] unsigned scaled offset
    pub fn ldr_imm(&mut self, size: u32, rt: u32, xn: u32, offset: u32) {
        debug_assert!(offset % (1 << size) == 0);
        self.emit((size << 30) | 0x3940_0000 | ((offset >> size) << 10) | (xn << 5) | rt);
    }
    pub fn str_imm(&mut self, size: u32, rt: u32, xn: u32, offset: u32) {
        debug_assert!(offset % (1 << size) == 0);
        self.emit((size << 30) | 0x3900_0000 | ((offset >> size) << 10) | (xn << 5) | rt);
    }
    /// stp/ldp x registers, pre-index / post-index / signed offset
    pub fn stp_x_pre(&mut self, rt: u32, rt2: u32, xn: u32, imm: i32) {
        self.emit(0xa980_0000 | ((((imm / 8) as u32) & 0x7f) << 15) | (rt2 << 10) | (xn << 5) | rt);
    }
    pub fn ldp_x_post(&mut self, rt: u32, rt2: u32, xn: u32, imm: i32) {
        self.emit(0xa8c0_0000 | ((((imm / 8) as u32) & 0x7f) << 15) | (rt2 << 10) | (xn << 5) | rt);
    }
    pub fn stp_w(&mut self, rt: u32, rt2: u32, xn: u32, offset: u32) {
        self.emit(0x2900_0000 | (((offset / 4) & 0x7f) << 15) | (rt2 << 10) | (xn << 5) | rt);
    }
    pub fn ldp_w(&mut self, rt: u32, rt2: u32, xn: u32, offset: u32) {
        self.emit(0x2940_0000 | (((offset / 4) & 0x7f) << 15) | (rt2 << 10) | (xn << 5) | rt);
    }

    // --- branches ------------------------------------------------------------------------------------
    pub fn b_fixup(&mut self) -> Fixup {
        let f = Fixup { pos: self.pos(), kind: 0 };
        self.emit(0x1400_0000);
        f
    }
    pub fn bcond_fixup(&mut self, c: Cond) -> Fixup {
        let f = Fixup { pos: self.pos(), kind: 1 };
        self.emit(0x5400_0000 | c as u32);
        f
    }
    pub fn cbz_w_fixup(&mut self, rt: u32) -> Fixup {
        let f = Fixup { pos: self.pos(), kind: 1 };
        self.emit(0x3400_0000 | rt);
        f
    }
    pub fn cbnz_w_fixup(&mut self, rt: u32) -> Fixup {
        let f = Fixup { pos: self.pos(), kind: 1 };
        self.emit(0x3500_0000 | rt);
        f
    }
    /// tbnz wt, #bit (range +-8K instructions)
    pub fn tbnz_w_fixup(&mut self, rt: u32, bit: u32) -> Fixup {
        let f = Fixup { pos: self.pos(), kind: 2 };
        self.emit(0x3700_0000 | (bit << 19) | rt);
        f
    }
    /// Point a fixup at the current position.
    pub fn bind(&mut self, f: Fixup) {
        let here = self.pos();
        self.patch(f, here);
    }
    pub fn patch(&mut self, f: Fixup, target: usize) {
        let off = target as i64 - f.pos as i64;
        let w = &mut self.code[f.pos];
        match f.kind {
            0 => *w |= (off as u32) & 0x03ff_ffff,
            2 => {
                assert!((-0x2000..0x2000).contains(&off), "tbz/tbnz target out of range");
                *w |= ((off as u32) & 0x3fff) << 5
            }
            _ => *w |= ((off as u32) & 0x7ffff) << 5,
        }
    }
    pub fn b_to(&mut self, target: usize) {
        let off = target as i64 - self.pos() as i64;
        self.emit(0x1400_0000 | ((off as u32) & 0x03ff_ffff));
    }
    pub fn blr(&mut self, xn: u32) {
        self.emit(0xd63f_0000 | (xn << 5));
    }
    pub fn br(&mut self, xn: u32) {
        self.emit(0xd61f_0000 | (xn << 5));
    }
    pub fn ret(&mut self) {
        self.emit(0xd65f_03c0);
    }

    // --- system --------------------------------------------------------------------------------------
    pub fn mrs_nzcv(&mut self, xt: u32) {
        self.emit(0xd53b_4200 | xt);
    }
    pub fn msr_nzcv(&mut self, xt: u32) {
        self.emit(0xd51b_4200 | xt);
    }
    pub fn brk(&mut self, imm: u32) {
        self.emit(0xd420_0000 | ((imm & 0xffff) << 5));
    }
}


// --- SIMD & FP (used by the SSE translator) -------------------------------------------------------
// Vector registers are plain numbers 0..31. "q" forms operate on all 128 bits.

/// Element size for vector instructions: 0 = 8-bit, 1 = 16-bit, 2 = 32-bit, 3 = 64-bit.
pub type Esz = u32;

impl Asm {
    /// Emit an instruction with a 19-bit pc-relative field at bits 5..23 (patched later).
    pub fn emit_fixup19(&mut self, insn: u32) -> Fixup {
        let f = Fixup { pos: self.pos(), kind: 1 };
        self.emit(insn);
        f
    }
    pub fn cbz_w_to(&mut self, rt: u32, target: usize) {
        let off = target as i64 - self.pos() as i64;
        self.emit(0x3400_0000 | (((off as u32) & 0x7ffff) << 5) | rt);
    }
    pub fn cbz_x_fixup(&mut self, rt: u32) -> Fixup {
        self.emit_fixup19(0xb400_0000 | rt)
    }
    /// csinc wd, wn, wm, cond
    pub fn csinc_w(&mut self, rd: u32, rn: u32, rm: u32, c: Cond) {
        self.emit(0x1a80_0400 | (rm << 16) | ((c as u32) << 12) | (rn << 5) | rd);
    }
    /// orr wd, wn, wm, lsl #sh
    pub fn orr_w_lsl(&mut self, rd: u32, rn: u32, rm: u32, sh: u32) {
        self.emit(0x2a00_0000 | (rm << 16) | (sh << 10) | (rn << 5) | rd);
    }
    /// eor wd, wn, wm, lsr #sh
    pub fn eor_w_lsr(&mut self, rd: u32, rn: u32, rm: u32, sh: u32) {
        self.emit(0x4a40_0000 | (rm << 16) | (sh << 10) | (rn << 5) | rd);
    }
    /// cmp xn, wm, sxtw
    /// ldaddal ws, wt, [xn]: wt = old [xn]; [xn] = old + ws (atomic, acquire-release)
    pub fn ldaddal_w(&mut self, ws: u32, wt: u32, xn: u32) {
        self.emit(0xb8e0_0000 | (ws << 16) | (xn << 5) | wt);
    }
    /// casal ws, wt, [xn]: if [xn] == ws then [xn] = wt; ws = old [xn] (atomic, acquire-release)
    pub fn casal_w(&mut self, ws: u32, wt: u32, xn: u32) {
        self.emit(0x88e0_fc00 | (ws << 16) | (xn << 5) | wt);
    }
    /// sxtw xd, wn
    pub fn sxtw_x(&mut self, rd: u32, rn: u32) {
        self.emit(0x9340_7c00 | (rn << 5) | rd);
    }
    /// orr xd, xn, xm, lsl #sh
    pub fn orr_x_lsl(&mut self, rd: u32, rn: u32, rm: u32, sh: u32) {
        self.emit(0xaa00_0000 | (rm << 16) | (sh << 10) | (rn << 5) | rd);
    }
    pub fn cmp_x_sxtw(&mut self, xn: u32, wm: u32) {
        self.emit(0xeb20_c000 | (wm << 16) | (xn << 5) | XZR);
    }

    // loads / stores
    /// ldr qt, [xn, #off] (off multiple of 16)
    pub fn ldr_q_imm(&mut self, qt: u32, xn: u32, off: u32) {
        debug_assert!(off % 16 == 0);
        self.emit(0x3dc0_0000 | ((off / 16) << 10) | (xn << 5) | qt);
    }
    pub fn str_q_imm(&mut self, qt: u32, xn: u32, off: u32) {
        debug_assert!(off % 16 == 0);
        self.emit(0x3d80_0000 | ((off / 16) << 10) | (xn << 5) | qt);
    }
    /// SIMD load [xn, wm, uxtw]; size: 2 = S, 3 = D, 4 = Q. Narrow loads zero the rest of the register.
    pub fn ldr_v_uxtw(&mut self, size: u32, vt: u32, xn: u32, wm: u32) {
        let base = match size {
            2 => 0xbc60_4800,
            3 => 0xfc60_4800,
            _ => 0x3ce0_4800,
        };
        self.emit(base | (wm << 16) | (xn << 5) | vt);
    }
    pub fn str_v_uxtw(&mut self, size: u32, vt: u32, xn: u32, wm: u32) {
        let base = match size {
            2 => 0xbc20_4800,
            3 => 0xfc20_4800,
            _ => 0x3ca0_4800,
        };
        self.emit(base | (wm << 16) | (xn << 5) | vt);
    }
    /// ldr qt, <literal> (19-bit word offset, patched later)
    pub fn ldr_q_lit_fixup(&mut self, qt: u32) -> Fixup {
        self.emit_fixup19(0x9c00_0000 | qt)
    }

    // generic encodings
    /// Advanced SIMD three same (128-bit unless q == 0).
    pub fn v3(&mut self, q: u32, u: u32, size: u32, opcode: u32, d: u32, n: u32, m: u32) {
        self.emit(0x0e20_0400 | (q << 30) | (u << 29) | (size << 22) | (m << 16) | (opcode << 11) | (n << 5) | d);
    }
    /// Advanced SIMD two-register miscellaneous.
    pub fn v2misc(&mut self, q: u32, u: u32, size: u32, opcode: u32, d: u32, n: u32) {
        self.emit(0x0e20_0800 | (q << 30) | (u << 29) | (size << 22) | (opcode << 12) | (n << 5) | d);
    }
    /// Advanced SIMD shift by immediate; `immhb` is the 7-bit immh:immb field.
    pub fn vshift(&mut self, q: u32, u: u32, immhb: u32, opcode: u32, d: u32, n: u32) {
        self.emit(0x0f00_0400 | (q << 30) | (u << 29) | (immhb << 16) | (opcode << 11) | (n << 5) | d);
    }

    // moves
    /// mov vd.16b, vn.16b
    pub fn vmov(&mut self, d: u32, n: u32) {
        if d != n {
            self.v3(1, 0, 2, 0x03, d, n, n);
        }
    }
    /// mov vd.8b, vn.8b (copies the low 64 bits, zeroes the upper half)
    pub fn vmov8(&mut self, d: u32, n: u32) {
        self.v3(0, 0, 2, 0x03, d, n, n);
    }
    /// movi vd.2d, #0
    pub fn vzero(&mut self, d: u32) {
        self.emit(0x6f00_e400 | d);
    }
    /// movi vd.4s, #imm8, lsl #shift (shift 0/8/16/24)
    pub fn movi_4s(&mut self, d: u32, imm8: u32, shift: u32) {
        let cmode = (shift / 8) << 1;
        self.emit(0x4f00_0400 | (((imm8 >> 5) & 7) << 16) | (cmode << 12) | ((imm8 & 31) << 5) | d);
    }
    /// ins vd.<sz>[i], vn.<sz>[j]
    pub fn ins_elem(&mut self, sz: Esz, d: u32, i: u32, n: u32, j: u32) {
        let imm5 = ((i << 1) | 1) << sz;
        let imm4 = j << sz;
        self.emit(0x6e00_0400 | (imm5 << 16) | (imm4 << 11) | (n << 5) | d);
    }
    /// dup vd.<sz>, vn.<sz>[j] (128-bit)
    pub fn dup_elem(&mut self, sz: Esz, d: u32, n: u32, j: u32) {
        let imm5 = ((j << 1) | 1) << sz;
        self.emit(0x4e00_0400 | (imm5 << 16) | (n << 5) | d);
    }
    /// umov wd, vn.s[j]
    pub fn umov_w_s(&mut self, d: u32, n: u32, j: u32) {
        let imm5 = ((j << 1) | 1) << 2;
        self.emit(0x0e00_3c00 | (imm5 << 16) | (n << 5) | d);
    }
    /// fmov sd, wn (upper bits of vd are zeroed)
    pub fn fmov_s_w(&mut self, d: u32, n: u32) {
        self.emit(0x1e27_0000 | (n << 5) | d);
    }
    pub fn fmov_w_s(&mut self, d: u32, n: u32) {
        self.emit(0x1e26_0000 | (n << 5) | d);
    }
    pub fn fmov_x_d(&mut self, d: u32, n: u32) {
        self.emit(0x9e66_0000 | (n << 5) | d);
    }
    /// add xd, xn, wm, uxtw
    pub fn add_x_uxtw(&mut self, rd: u32, xn: u32, wm: u32) {
        self.emit(0x8b20_4000 | (wm << 16) | (xn << 5) | rd);
    }
    /// ldapr{b,h} rt, [xn]: load-acquire (RCpc, FEAT_LRCPC); size as ldr_uxtw
    pub fn ldapr(&mut self, size: u32, rt: u32, xn: u32) {
        self.emit((size << 30) | 0x38bf_c000 | (xn << 5) | rt);
    }
    /// stlr{b,h} rt, [xn]: store-release; size as str_uxtw
    pub fn stlr(&mut self, size: u32, rt: u32, xn: u32) {
        self.emit((size << 30) | 0x089f_fc00 | (xn << 5) | rt);
    }
    /// dmb ishld: earlier loads before later loads and stores
    pub fn dmb_ishld(&mut self) {
        self.emit(0xd503_39bf);
    }
    /// dmb ishst: earlier stores before later stores
    pub fn dmb_ishst(&mut self) {
        self.emit(0xd503_3abf);
    }
    /// fmov dd, xn
    pub fn fmov_d_x(&mut self, d: u32, n: u32) {
        self.emit(0x9e67_0000 | (n << 5) | d);
    }
    /// scvtf dd, xn
    pub fn scvtf_d_x(&mut self, d: u32, n: u32) {
        self.emit(0x9e62_0000 | (n << 5) | d);
    }

    // permutes: opc 1 = uzp1, 2 = trn1, 3 = zip1, 5 = uzp2, 6 = trn2, 7 = zip2
    pub fn vperm(&mut self, opc: u32, sz: Esz, d: u32, n: u32, m: u32) {
        self.emit(0x4e00_0800 | (sz << 22) | (m << 16) | (opc << 12) | (n << 5) | d);
    }
    /// ext vd.16b, vn.16b, vm.16b, #idx
    pub fn ext16(&mut self, d: u32, n: u32, m: u32, idx: u32) {
        self.emit(0x6e00_0000 | (m << 16) | (idx << 11) | (n << 5) | d);
    }

    // logic (128-bit)
    pub fn vand(&mut self, d: u32, n: u32, m: u32) { self.v3(1, 0, 0, 0x03, d, n, m) }
    /// d = n & !m
    pub fn vbic(&mut self, d: u32, n: u32, m: u32) { self.v3(1, 0, 1, 0x03, d, n, m) }
    pub fn vorr(&mut self, d: u32, n: u32, m: u32) { self.v3(1, 0, 2, 0x03, d, n, m) }
    pub fn veor(&mut self, d: u32, n: u32, m: u32) { self.v3(1, 1, 0, 0x03, d, n, m) }
    /// d = d ? n : m (bitwise)
    pub fn vbsl(&mut self, d: u32, n: u32, m: u32) { self.v3(1, 1, 1, 0x03, d, n, m) }
    /// d = m ? d : n (insert n where m is clear)
    pub fn vbif(&mut self, d: u32, n: u32, m: u32) { self.v3(1, 1, 3, 0x03, d, n, m) }
    pub fn vnot(&mut self, d: u32, n: u32) { self.v2misc(1, 1, 0, 0x05, d, n) }

    // float vector ops; dbl selects .2d instead of .4s
    pub fn fadd_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 0, dbl as u32, 0x1a, d, n, m) }
    pub fn fsub_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 0, 2 | dbl as u32, 0x1a, d, n, m) }
    pub fn fmul_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 1, dbl as u32, 0x1b, d, n, m) }
    pub fn fdiv_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 1, dbl as u32, 0x1f, d, n, m) }
    pub fn fcmeq_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 0, dbl as u32, 0x1c, d, n, m) }
    pub fn fcmge_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 1, dbl as u32, 0x1c, d, n, m) }
    pub fn fcmgt_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 1, 2 | dbl as u32, 0x1c, d, n, m) }
    pub fn fsqrt_v(&mut self, dbl: bool, d: u32, n: u32) { self.v2misc(1, 1, 2 | dbl as u32, 0x1f, d, n) }
    pub fn scvtf_v(&mut self, dbl: bool, d: u32, n: u32) { self.v2misc(1, 0, dbl as u32, 0x1d, d, n) }
    /// fcvtzs vd.4s, vn.4s
    pub fn fcvtzs_4s(&mut self, d: u32, n: u32) { self.v2misc(1, 0, 2, 0x1b, d, n) }
    /// fcvtns vd.4s, vn.4s
    pub fn fcvtns_4s(&mut self, d: u32, n: u32) { self.v2misc(1, 0, 0, 0x1a, d, n) }
    /// fcvtl vd.2d, vn.2s
    pub fn fcvtl_2d(&mut self, d: u32, n: u32) { self.v2misc(0, 0, 1, 0x17, d, n) }
    /// fcvtn vd.2s, vn.2d (upper half zeroed)
    pub fn fcvtn_2s(&mut self, d: u32, n: u32) { self.v2misc(0, 0, 1, 0x16, d, n) }
    /// sxtl vd.2d, vn.2s
    pub fn sxtl_2d(&mut self, d: u32, n: u32) { self.vshift(0, 0, 32, 0x14, d, n) }
    /// uminv sd, vn.4s
    pub fn uminv_4s(&mut self, d: u32, n: u32) { self.emit(0x6eb1_a800 | (n << 5) | d) }
    /// addv sd, vn.4s
    pub fn addv_4s(&mut self, d: u32, n: u32) { self.emit(0x4eb1_b800 | (n << 5) | d) }

    // float scalar ops; dbl selects d registers instead of s
    fn fp2(&mut self, dbl: bool, opcode: u32, d: u32, n: u32, m: u32) {
        self.emit(0x1e20_0800 | ((dbl as u32) << 22) | (m << 16) | (opcode << 12) | (n << 5) | d);
    }
    pub fn fmul_s(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.fp2(dbl, 0, d, n, m) }
    pub fn fdiv_s(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.fp2(dbl, 1, d, n, m) }
    pub fn fadd_s(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.fp2(dbl, 2, d, n, m) }
    pub fn fsub_s(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.fp2(dbl, 3, d, n, m) }
    pub fn fsqrt_s(&mut self, dbl: bool, d: u32, n: u32) {
        self.emit(0x1e21_c000 | ((dbl as u32) << 22) | (n << 5) | d);
    }
    /// fcvt dd, sn
    pub fn fcvt_d_s(&mut self, d: u32, n: u32) { self.emit(0x1e22_c000 | (n << 5) | d) }
    /// fcvt sd, dn
    pub fn fcvt_s_d(&mut self, d: u32, n: u32) { self.emit(0x1e62_4000 | (n << 5) | d) }
    pub fn fcmp_s(&mut self, dbl: bool, n: u32, m: u32) {
        self.emit(0x1e20_2000 | ((dbl as u32) << 22) | (m << 16) | (n << 5));
    }
    /// fcmeq (scalar) sd/dd
    pub fn fcmeq_s(&mut self, dbl: bool, d: u32, n: u32, m: u32) {
        self.emit(0x5e20_e400 | ((dbl as u32) << 22) | (m << 16) | (n << 5) | d);
    }
    /// scvtf sd/dd, wn
    pub fn scvtf_s_w(&mut self, dbl: bool, d: u32, n: u32) {
        self.emit(0x1e22_0000 | ((dbl as u32) << 22) | (n << 5) | d);
    }
    /// fcvtzs xd, sn/dn
    pub fn fcvtzs_x(&mut self, dbl: bool, d: u32, n: u32) {
        self.emit(0x9e38_0000 | ((dbl as u32) << 22) | (n << 5) | d);
    }
    /// fcvtns xd, sn/dn
    pub fn fcvtns_x(&mut self, dbl: bool, d: u32, n: u32) {
        self.emit(0x9e20_0000 | ((dbl as u32) << 22) | (n << 5) | d);
    }

    // integer vector ops (sz = element size)
    pub fn vadd(&mut self, sz: Esz, d: u32, n: u32, m: u32) { self.v3(1, 0, sz, 0x10, d, n, m) }
    pub fn vsub(&mut self, sz: Esz, d: u32, n: u32, m: u32) { self.v3(1, 1, sz, 0x10, d, n, m) }
    pub fn vmul(&mut self, sz: Esz, d: u32, n: u32, m: u32) { self.v3(1, 0, sz, 0x13, d, n, m) }
    pub fn vcmeq(&mut self, sz: Esz, d: u32, n: u32, m: u32) { self.v3(1, 1, sz, 0x11, d, n, m) }
    pub fn vcmgt(&mut self, sz: Esz, d: u32, n: u32, m: u32) { self.v3(1, 0, sz, 0x06, d, n, m) }
    /// ushl vd, vn, vm (per-lane shift by signed amount)
    pub fn vushl(&mut self, sz: Esz, d: u32, n: u32, m: u32) { self.v3(1, 1, sz, 0x08, d, n, m) }
    /// shl #sh (0 <= sh < esize)
    pub fn vshl(&mut self, sz: Esz, d: u32, n: u32, sh: u32) { self.vshift(1, 0, (8 << sz) + sh, 0x0a, d, n) }
    /// ushr #sh (1 <= sh <= esize)
    pub fn vushr(&mut self, sz: Esz, d: u32, n: u32, sh: u32) { self.vshift(1, 1, (16 << sz) - sh, 0x00, d, n) }
    /// sshr #sh (1 <= sh <= esize)
    pub fn vsshr(&mut self, sz: Esz, d: u32, n: u32, sh: u32) { self.vshift(1, 0, (16 << sz) - sh, 0x00, d, n) }
    /// faddp vd, vn, vm (pairwise: [n0+n1, n2+n3, m0+m1, m2+m3])
    pub fn faddp_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 1, dbl as u32, 0x1a, d, n, m) }
    /// facgt vd, vn, vm (|n| > |m|)
    pub fn facgt_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 1, 2 | dbl as u32, 0x1d, d, n, m) }
    /// facge vd, vn, vm (|n| >= |m|)
    pub fn facge_v(&mut self, dbl: bool, d: u32, n: u32, m: u32) { self.v3(1, 1, dbl as u32, 0x1d, d, n, m) }
    /// rev64 vd.4s, vn.4s (swap 32-bit lanes within each 64-bit half)
    pub fn rev64_4s(&mut self, d: u32, n: u32) { self.v2misc(1, 0, 2, 0x00, d, n) }
    /// fcvtl(2) vd.2d, vn.2s / vn.4s upper half
    pub fn fcvtl_2d_hi(&mut self, d: u32, n: u32) { self.v2misc(1, 0, 1, 0x17, d, n) }
    /// fcvtn2 vd.4s, vn.2d (writes the upper half, keeps the lower)
    pub fn fcvtn_4s_hi(&mut self, d: u32, n: u32) { self.v2misc(1, 0, 1, 0x16, d, n) }
    /// xtn vd.2s, vn.2d
    pub fn xtn_2s(&mut self, d: u32, n: u32) { self.v2misc(0, 0, 2, 0x12, d, n) }
    /// Advanced SIMD three different: smull/umull(2) with opcode 1100.
    pub fn vmull(&mut self, upper: bool, unsigned: bool, sz: Esz, d: u32, n: u32, m: u32) {
        self.emit(0x0e20_c000 | ((upper as u32) << 30) | ((unsigned as u32) << 29) | (sz << 22) | (m << 16) | (n << 5) | d);
    }
    /// addp vd, vn, vm (integer pairwise add)
    pub fn vaddp(&mut self, sz: Esz, d: u32, n: u32, m: u32) { self.v3(1, 0, sz, 0x17, d, n, m) }
    /// fmov vd.2d, #1.0
    pub fn fmov_2d_one(&mut self, d: u32) { self.emit(0x6f03_f600 | d) }
    /// addv bd, vn.8b
    pub fn addv_8b(&mut self, d: u32, n: u32) { self.emit(0x0e31_b800 | (n << 5) | d) }
    /// umov wd, vn.b[j] / vn.h[j]
    pub fn umov_w(&mut self, sz: Esz, d: u32, n: u32, j: u32) {
        let imm5 = ((j << 1) | 1) << sz;
        self.emit(0x0e00_3c00 | (imm5 << 16) | (n << 5) | d);
    }
    /// umov xd, vn.d[j]
    pub fn umov_x_d(&mut self, d: u32, n: u32, j: u32) {
        let imm5 = ((j << 1) | 1) << 3;
        self.emit(0x4e00_3c00 | (imm5 << 16) | (n << 5) | d);
    }
    /// ins vd.<sz>[i], wn
    pub fn ins_gen(&mut self, sz: Esz, d: u32, i: u32, n: u32) {
        let imm5 = ((i << 1) | 1) << sz;
        self.emit(0x4e00_1c00 | (imm5 << 16) | (n << 5) | d);
    }
    /// ushr vd.16b/8h/4s/2d with explicit Q (q = 0: 64-bit vector)
    pub fn vushr_q(&mut self, q: u32, sz: Esz, d: u32, n: u32, sh: u32) { self.vshift(q, 1, (16 << sz) - sh, 0x00, d, n) }
    /// fcmlt vd.4s, vn.4s, #0.0
    pub fn fcmlt_zero_4s(&mut self, d: u32, n: u32) { self.v2misc(1, 0, 2, 0x0e, d, n) }
    /// sqxtn(2) vd, vn: narrow from element size sz+1 to sz with signed saturation
    pub fn sqxtn(&mut self, upper: bool, sz: Esz, d: u32, n: u32) { self.v2misc(upper as u32, 0, sz, 0x14, d, n) }
    /// sqxtun(2): signed to unsigned saturation
    pub fn sqxtun(&mut self, upper: bool, sz: Esz, d: u32, n: u32) { self.v2misc(upper as u32, 1, sz, 0x12, d, n) }
}

impl Default for Asm {
    fn default() -> Self {
        Self::new()
    }
}

#[inline]
fn rn_check(x: u32) -> u32 {
    x
}
