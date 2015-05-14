/*
 *  CRIS emulation micro-operations for qemu.
 *
 *  Copyright (c) 2007 Edgar E. Iglesias, Axis Communications AB.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "exec.h"
#include "host-utils.h"

#define REGNAME r0
#define REG (env->regs[0])
#include "op_template.h"

#define REGNAME r1
#define REG (env->regs[1])
#include "op_template.h"

#define REGNAME r2
#define REG (env->regs[2])
#include "op_template.h"

#define REGNAME r3
#define REG (env->regs[3])
#include "op_template.h"

#define REGNAME r4
#define REG (env->regs[4])
#include "op_template.h"

#define REGNAME r5
#define REG (env->regs[5])
#include "op_template.h"

#define REGNAME r6
#define REG (env->regs[6])
#include "op_template.h"

#define REGNAME r7
#define REG (env->regs[7])
#include "op_template.h"

#define REGNAME r8
#define REG (env->regs[8])
#include "op_template.h"

#define REGNAME r9
#define REG (env->regs[9])
#include "op_template.h"

#define REGNAME r10
#define REG (env->regs[10])
#include "op_template.h"

#define REGNAME r11
#define REG (env->regs[11])
#include "op_template.h"

#define REGNAME r12
#define REG (env->regs[12])
#include "op_template.h"

#define REGNAME r13
#define REG (env->regs[13])
#include "op_template.h"

#define REGNAME r14
#define REG (env->regs[14])
#include "op_template.h"

#define REGNAME r15
#define REG (env->regs[15])
#include "op_template.h"


#define REGNAME p0
#define REG (env->pregs[0])
#include "op_template.h"

#define REGNAME p1
#define REG (env->pregs[1])
#include "op_template.h"

#define REGNAME p2
#define REG (env->pregs[2])
#include "op_template.h"

#define REGNAME p3
#define REG (env->pregs[3])
#include "op_template.h"

#define REGNAME p4
#define REG (env->pregs[4])
#include "op_template.h"

#define REGNAME p5
#define REG (env->pregs[5])
#include "op_template.h"

#define REGNAME p6
#define REG (env->pregs[6])
#include "op_template.h"

#define REGNAME p7
#define REG (env->pregs[7])
#include "op_template.h"

#define REGNAME p8
#define REG (env->pregs[8])
#include "op_template.h"

#define REGNAME p9
#define REG (env->pregs[9])
#include "op_template.h"

#define REGNAME p10
#define REG (env->pregs[10])
#include "op_template.h"

#define REGNAME p11
#define REG (env->pregs[11])
#include "op_template.h"

#define REGNAME p12
#define REG (env->pregs[12])
#include "op_template.h"

#define REGNAME p13
#define REG (env->pregs[13])
#include "op_template.h"

#define REGNAME p14
#define REG (env->pregs[14])
#include "op_template.h"

#define REGNAME p15
#define REG (env->pregs[15])
#include "op_template.h"

/* Microcode.  */

void OPPROTO op_break_im(void)
{
	env->trap_vector = PARAM1;
	env->exception_index = EXCP_BREAK;
	cpu_loop_exit();
}

void OPPROTO op_debug(void)
{
	env->exception_index = EXCP_DEBUG;
	cpu_loop_exit();
}

void OPPROTO op_exec_insn(void)
{
	env->stats.exec_insns++;
	RETURN();
}
void OPPROTO op_exec_load(void)
{
	env->stats.exec_loads++;
	RETURN();
}
void OPPROTO op_exec_store(void)
{
	env->stats.exec_stores++;
	RETURN();
}

void OPPROTO op_ccs_lshift (void)
{
	uint32_t ccs;

	/* Apply the ccs shift.  */
	ccs = env->pregs[PR_CCS];
	ccs = (ccs & 0xc0000000) | ((ccs << 12) >> 2);
	env->pregs[PR_CCS] = ccs;
	RETURN();
}
void OPPROTO op_ccs_rshift (void)
{
	register uint32_t ccs;

	/* Apply the ccs shift.  */
	ccs = env->pregs[PR_CCS];
	ccs = (ccs & 0xc0000000) | ((ccs & 0x0fffffff) >> 10);
	if (ccs & U_FLAG)
	{
		/* Enter user mode.  */
		env->ksp = env->regs[R_SP];
		env->regs[R_SP] = env->pregs[PR_USP];
	}

	env->pregs[PR_CCS] = ccs;

	RETURN();
}

void OPPROTO op_setf (void)
{
	if (!(env->pregs[PR_CCS] & U_FLAG) && (PARAM1 & U_FLAG))
	{
		/* Enter user mode.  */
		env->ksp = env->regs[R_SP];
		env->regs[R_SP] = env->pregs[PR_USP];
	}

	env->pregs[PR_CCS] |= PARAM1;
	RETURN();
}

void OPPROTO op_clrf (void)
{
	env->pregs[PR_CCS] &= ~PARAM1;
	RETURN();
}

void OPPROTO op_movl_debug1_T0 (void)
{
	env->debug1 = T0;
	RETURN();
}

void OPPROTO op_movl_debug2_T0 (void)
{
	env->debug2 = T0;
	RETURN();
}

void OPPROTO op_movl_debug3_T0 (void)
{
	env->debug3 = T0;
	RETURN();
}
void OPPROTO op_movl_debug1_T1 (void)
{
	env->debug1 = T1;
	RETURN();
}

void OPPROTO op_movl_debug2_T1 (void)
{
	env->debug2 = T1;
	RETURN();
}

void OPPROTO op_movl_debug3_T1 (void)
{
	env->debug3 = T1;
	RETURN();
}
void OPPROTO op_movl_debug3_im (void)
{
	env->debug3 = PARAM1;
	RETURN();
}
void OPPROTO op_movl_T0_flags (void)
{
	T0 = env->pregs[PR_CCS];
	RETURN();
}
void OPPROTO op_movl_flags_T0 (void)
{
	env->pregs[PR_CCS] = T0;
	RETURN();
}

void OPPROTO op_movl_sreg_T0 (void)
{
	uint32_t srs;
	srs = env->pregs[PR_SRS];
	srs &= 3;

	env->sregs[srs][PARAM1] = T0;
	RETURN();
}

void OPPROTO op_movl_tlb_hi_T0 (void)
{
	uint32_t srs;
	srs = env->pregs[PR_SRS];
	if (srs == 1 || srs == 2)
	{
		/* Writes to tlb-hi write to mm_cause as a side effect.  */
		env->sregs[SFR_RW_MM_TLB_HI] = T0;
		env->sregs[SFR_R_MM_CAUSE] = T0;
	}
	RETURN();
}

void OPPROTO op_movl_tlb_lo_T0 (void)
{
	uint32_t srs;

	env->pregs[PR_SRS] &= 3;
	srs = env->pregs[PR_SRS];

	if (srs == 1 || srs == 2)
	{
		uint32_t set;
		uint32_t idx;
		uint32_t lo, hi;

		idx = set = env->sregs[SFR_RW_MM_TLB_SEL];
		set >>= 4;
		set &= 3;

		idx &= 15;
		/* We've just made a write to tlb_lo.  */
		lo = env->sregs[SFR_RW_MM_TLB_LO];
		/* Writes are done via r_mm_cause.  */
		hi = env->sregs[SFR_R_MM_CAUSE];
		env->tlbsets[srs - 1][set][idx].lo = lo;
		env->tlbsets[srs - 1][set][idx].hi = hi;
	}
	RETURN();
}

void OPPROTO op_movl_T0_sreg (void)
{
	uint32_t srs;
	env->pregs[PR_SRS] &= 3;
	srs = env->pregs[PR_SRS];
	
	if (srs == 1 || srs == 2)
	{
		uint32_t set;
		uint32_t idx;
		uint32_t lo, hi;

		idx = set = env->sregs[SFR_RW_MM_TLB_SEL];
		set >>= 4;
		set &= 3;
		idx &= 15;

		/* Update the mirror regs.  */
		hi = env->tlbsets[srs - 1][set][idx].hi;
		lo = env->tlbsets[srs - 1][set][idx].lo;
		env->sregs[SFR_RW_MM_TLB_HI] = hi;
		env->sregs[SFR_RW_MM_TLB_LO] = lo;
	}
	T0 = env->sregs[srs][PARAM1];
	RETURN();
}

void OPPROTO op_update_cc (void)
{
	env->cc_op = PARAM1;
	env->cc_dest = PARAM2;
	env->cc_src = PARAM3;
	RETURN();
}

void OPPROTO op_update_cc_op (void)
{
	env->cc_op = PARAM1;
	RETURN();
}

void OPPROTO op_update_cc_mask (void)
{
	env->cc_mask = PARAM1;
	RETURN();
}

void OPPROTO op_update_cc_dest_T0 (void)
{
	env->cc_dest = T0;
	RETURN();
}

void OPPROTO op_update_cc_result_T0 (void)
{
	env->cc_result = T0;
	RETURN();
}

void OPPROTO op_update_cc_size_im (void)
{
	env->cc_size = PARAM1;
	RETURN();
}

void OPPROTO op_update_cc_src_T1 (void)
{
	env->cc_src = T1;
	RETURN();
}
void OPPROTO op_update_cc_x (void)
{
	env->cc_x_live = PARAM1;
	env->cc_x = PARAM2;
	RETURN();
}

void OPPROTO op_extb_T0_T0 (void)
{
	T0 = ((int8_t)T0);
	RETURN();
}
void OPPROTO op_extb_T1_T0 (void)
{
	T1 = ((int8_t)T0);
	RETURN();
}
void OPPROTO op_extb_T1_T1 (void)
{
	T1 = ((int8_t)T1);
	RETURN();
}
void OPPROTO op_zextb_T0_T0 (void)
{
	T0 = ((uint8_t)T0);
	RETURN();
}
void OPPROTO op_zextb_T1_T0 (void)
{
	T1 = ((uint8_t)T0);
	RETURN();
}
void OPPROTO op_zextb_T1_T1 (void)
{
	T1 = ((uint8_t)T1);
	RETURN();
}
void OPPROTO op_extw_T0_T0 (void)
{
	T0 = ((int16_t)T0);
	RETURN();
}
void OPPROTO op_extw_T1_T0 (void)
{
	T1 = ((int16_t)T0);
	RETURN();
}
void OPPROTO op_extw_T1_T1 (void)
{
	T1 = ((int16_t)T1);
	RETURN();
}

void OPPROTO op_zextw_T0_T0 (void)
{
	T0 = ((uint16_t)T0);
	RETURN();
}
void OPPROTO op_zextw_T1_T0 (void)
{
	T1 = ((uint16_t)T0);
	RETURN();
}

void OPPROTO op_zextw_T1_T1 (void)
{
	T1 = ((uint16_t)T1);
	RETURN();
}

void OPPROTO op_movl_T0_im (void)
{
	T0 = PARAM1;
	RETURN();
}
void OPPROTO op_movl_T1_im (void)
{
	T1 = PARAM1;
	RETURN();
}

void OPPROTO op_addl_T0_im (void)
{
	T0 += PARAM1;
	RETURN();
}

void OPPROTO op_addl_T1_im (void)
{
	T1 += PARAM1;
	RETURN();

}
void OPPROTO op_subl_T0_im (void)
{
	T0 -= PARAM1;
	RETURN();
}

void OPPROTO op_addxl_T0_C (void)
{
	if (env->pregs[PR_CCS] & X_FLAG)
		T0 += !!(env->pregs[PR_CCS] & C_FLAG);
	RETURN();
}
void OPPROTO op_subxl_T0_C (void)
{
	if (env->pregs[PR_CCS] & X_FLAG)
		T0 -= !!(env->pregs[PR_CCS] & C_FLAG);
	RETURN();
}
void OPPROTO op_addl_T0_C (void)
{
	T0 += !!(env->pregs[PR_CCS] & C_FLAG);
	RETURN();
}
void OPPROTO op_addl_T0_R (void)
{
	T0 += !!(env->pregs[PR_CCS] & R_FLAG);
	RETURN();
}

void OPPROTO op_clr_R (void)
{
	env->pregs[PR_CCS] &= ~R_FLAG;
	RETURN();
}


void OPPROTO op_andl_T0_im (void)
{
	T0 &= PARAM1;
	RETURN();
}

void OPPROTO op_andl_T1_im (void)
{
	T1 &= PARAM1;
	RETURN();
}

void OPPROTO op_movl_T0_T1 (void)
{
	T0 = T1;
	RETURN();
}

void OPPROTO op_swp_T0_T1 (void)
{
	T0 ^= T1;
	T1 ^= T0;
	T0 ^= T1;
	RETURN();
}

void OPPROTO op_movl_T1_T0 (void)
{
	T1 = T0;
	RETURN();
}

void OPPROTO op_movl_pc_T0 (void)
{
	env->pc = T0;
	RETURN();
}

void OPPROTO op_movl_T0_0 (void)
{
	T0 = 0;
	RETURN();
}

void OPPROTO op_addl_T0_T1 (void)
{
	T0 += T1;
	RETURN();
}

void OPPROTO op_subl_T0_T1 (void)
{
	T0 -= T1;
	RETURN();
}

void OPPROTO op_absl_T1_T1 (void)
{
	int32_t st = T1;

	T1 = st < 0 ? -st : st;
	RETURN();
}

void OPPROTO op_muls_T0_T1 (void)
{
	int64_t tmp, t0 ,t1;

	/* cast into signed values to make GCC sign extend these babies.  */
	t0 = (int32_t)T0;
	t1 = (int32_t)T1;

	tmp = t0 * t1;
	T0 = tmp & 0xffffffff;
	env->pregs[PR_MOF] = tmp >> 32;
	RETURN();
}

void OPPROTO op_mulu_T0_T1 (void)
{
	uint64_t tmp, t0 ,t1;
	t0 = T0;
	t1 = T1;

	tmp = t0 * t1;
	T0 = tmp & 0xffffffff;
	env->pregs[PR_MOF] = tmp >> 32;
	RETURN();
}

void OPPROTO op_dstep_T0_T1 (void)
{
	T0 <<= 1;
	if (T0 >= T1)
		T0 -= T1;
	RETURN();
}

void OPPROTO op_orl_T0_T1 (void)
{
	T0 |= T1;
	RETURN();
}

void OPPROTO op_andl_T0_T1 (void)
{
	T0 &= T1;
	RETURN();
}

void OPPROTO op_xorl_T0_T1 (void)
{
	T0 ^= T1;
	RETURN();
}

void OPPROTO op_lsll_T0_T1 (void)
{
	int s = T1;
	if (s > 31)
		T0 = 0;
	else
		T0 <<= s;
	RETURN();
}

void OPPROTO op_lsll_T0_im (void)
{
	T0 <<= PARAM1;
	RETURN();
}

void OPPROTO op_lsrl_T0_T1 (void)
{
	int s = T1;
	if (s > 31)
		T0 = 0;
	else
		T0 >>= s;
	RETURN();
}

/* Rely on GCC emitting an arithmetic shift for signed right shifts.  */
void OPPROTO op_asrl_T0_T1 (void)
{
	int s = T1;
	if (s > 31)
		T0 = T0 & 0x80000000 ? -1 : 0;
	else
		T0 = (int32_t)T0 >> s;
	RETURN();
}

void OPPROTO op_btst_T0_T1 (void)
{
	/* FIXME: clean this up.  */

	/* des ref:
	   The N flag is set according to the selected bit in the dest reg.
	   The Z flag is set if the selected bit and all bits to the right are
	   zero.
	   The X flag is cleared.
	   Other flags are left untouched.
	   The destination reg is not affected.*/
	unsigned int fz, sbit, bset, mask, masked_t0;

	sbit = T1 & 31;
        bset = !!(T0 & (1 << sbit));
	mask = sbit == 31 ? -1 : (1 << (sbit + 1)) - 1;
	masked_t0 = T0 & mask;
	fz = !(masked_t0 | bset);

	/* Clear the X, N and Z flags.  */
	T0 = env->pregs[PR_CCS] & ~(X_FLAG | N_FLAG | Z_FLAG);
	/* Set the N and Z flags accordingly.  */
	T0 |= (bset << 3) | (fz << 2);
	RETURN();
}

void OPPROTO op_bound_T0_T1 (void)
{
	if (T0 > T1)
		T0 = T1;
	RETURN();
}

void OPPROTO op_lz_T0_T1 (void)
{
	T0 = clz32(T1);
	RETURN();
}

void OPPROTO op_negl_T0_T1 (void)
{
	T0 = -T1;
	RETURN();
}

void OPPROTO op_negl_T1_T1 (void)
{
	T1 = -T1;
	RETURN();
}

void OPPROTO op_not_T0_T0 (void)
{
	T0 = ~(T0);
	RETURN();
}
void OPPROTO op_not_T1_T1 (void)
{
	T1 = ~(T1);
	RETURN();
}

void OPPROTO op_swapw_T0_T0 (void)
{
	T0 = (T0 << 16) | ((T0 >> 16));
	RETURN();
}

void OPPROTO op_swapb_T0_T0 (void)
{
	T0 = ((T0 << 8) & 0xff00ff00) | ((T0 >> 8) & 0x00ff00ff);
	RETURN();
}

void OPPROTO op_swapr_T0_T0 (void)
{
	T0 = (((T0 << 7) & 0x80808080) |
	      ((T0 << 5) & 0x40404040) |
	      ((T0 << 3) & 0x20202020) |
	      ((T0 << 1) & 0x10101010) |
	      ((T0 >> 1) & 0x08080808) |
	      ((T0 >> 3) & 0x04040404) |
	      ((T0 >> 5) & 0x02020202) |
	      ((T0 >> 7) & 0x01010101));
	RETURN();
}

void OPPROTO op_tst_cc_eq (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int z_set;

	z_set = !!(flags & Z_FLAG);
	T0 = z_set;
	RETURN();
}

void OPPROTO op_tst_cc_eq_fast (void) {
	T0 = !(env->cc_result);
	RETURN();
}

void OPPROTO op_tst_cc_ne (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int z_set;

	z_set = !!(flags & Z_FLAG);
	T0 = !z_set;
	RETURN();
}
void OPPROTO op_tst_cc_ne_fast (void) {
	T0 = !!(env->cc_result);
	RETURN();
}

void OPPROTO op_tst_cc_cc (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int c_set;

	c_set = !!(flags & C_FLAG);
	T0 = !c_set;
	RETURN();
}
void OPPROTO op_tst_cc_cs (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int c_set;

	c_set = !!(flags & C_FLAG);
	T0 = c_set;
	RETURN();
}

void OPPROTO op_tst_cc_vc (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int v_set;

	v_set = !!(flags & V_FLAG);
	T0 = !v_set;
	RETURN();
}
void OPPROTO op_tst_cc_vs (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int v_set;

	v_set = !!(flags & V_FLAG);
	T0 = v_set;
	RETURN();
}
void OPPROTO op_tst_cc_pl (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int n_set;

	n_set = !!(flags & N_FLAG);
	T0 = !n_set;
	RETURN();
}
void OPPROTO op_tst_cc_pl_fast (void) {
	T0 = ((int32_t)env->cc_result) >= 0;
	RETURN();
}

void OPPROTO op_tst_cc_mi (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int n_set;

	n_set = !!(flags & N_FLAG);
	T0 = n_set;
	RETURN();
}
void OPPROTO op_tst_cc_mi_fast (void) {
	T0 = ((int32_t)env->cc_result) < 0;
	RETURN();
}

void OPPROTO op_tst_cc_ls (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int c_set;
	int z_set;

	c_set = !!(flags & C_FLAG);
	z_set = !!(flags & Z_FLAG);
	T0 = c_set || z_set;
	RETURN();
}
void OPPROTO op_tst_cc_hi (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int z_set;
	int c_set;

	z_set = !!(flags & Z_FLAG);
	c_set = !!(flags & C_FLAG);
	T0 = !c_set && !z_set;
	RETURN();

}

void OPPROTO op_tst_cc_ge (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int n_set;
	int v_set;

	n_set = !!(flags & N_FLAG);
	v_set = !!(flags & V_FLAG);
	T0 = (n_set && v_set) || (!n_set && !v_set);
	RETURN();
}

void OPPROTO op_tst_cc_ge_fast (void) {
	T0 = ((int32_t)env->cc_src < (int32_t)env->cc_dest);
	RETURN();
}

void OPPROTO op_tst_cc_lt (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int n_set;
	int v_set;

	n_set = !!(flags & N_FLAG);
	v_set = !!(flags & V_FLAG);
	T0 = (n_set && !v_set) || (!n_set && v_set);
	RETURN();
}

void OPPROTO op_tst_cc_gt (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int n_set;
	int v_set;
	int z_set;

	n_set = !!(flags & N_FLAG);
	v_set = !!(flags & V_FLAG);
	z_set = !!(flags & Z_FLAG);
	T0 = (n_set && v_set && !z_set)
		|| (!n_set && !v_set && !z_set);
	RETURN();
}

void OPPROTO op_tst_cc_le (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int n_set;
	int v_set;
	int z_set;

	n_set = !!(flags & N_FLAG);
	v_set = !!(flags & V_FLAG);
	z_set = !!(flags & Z_FLAG);
	T0 = z_set || (n_set && !v_set) || (!n_set && v_set);
	RETURN();
}

void OPPROTO op_tst_cc_p (void) {
	uint32_t flags = env->pregs[PR_CCS];
	int p_set;

	p_set = !!(flags & P_FLAG);
	T0 = p_set;
	RETURN();
}

/* Evaluate the if the branch should be taken or not. Needs to be done in
   the original sequence. The acutal branch is rescheduled to right after the
   delay-slot.  */
void OPPROTO op_evaluate_bcc (void)
{
	env->btaken = T0;
	RETURN();
}

/* this one is used on every alu op, optimize it!.  */
void OPPROTO op_goto_if_not_x (void)
{
	if (env->pregs[PR_CCS] & X_FLAG)
		GOTO_LABEL_PARAM(1);
	RETURN();
}

void OPPROTO op_cc_jmp (void)
{
	if (env->btaken)
		env->pc = PARAM1;
	else
		env->pc = PARAM2;
	RETURN();
}

void OPPROTO op_cc_ngoto (void)
{
	if (!env->btaken)
		GOTO_LABEL_PARAM(1);
	RETURN();
}

void OPPROTO op_movl_btarget_T0 (void)
{
	env->btarget = T0;
	RETURN();
}

void OPPROTO op_jmp1 (void)
{
	env->pc = env->btarget;
	RETURN();
}
