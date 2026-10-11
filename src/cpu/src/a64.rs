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

impl Default for Asm {
    fn default() -> Self {
        Self::new()
    }
}

#[inline]
fn rn_check(x: u32) -> u32 {
    x
}
