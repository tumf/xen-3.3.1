#ifndef EXEC_SPARC_H
#define EXEC_SPARC_H 1
#include "config.h"
#include "dyngen-exec.h"

register struct CPUSPARCState *env asm(AREG0);
#define REGWPTR env->regwptr

#define FT0 (env->ft0)
#define FT1 (env->ft1)
#define DT0 (env->dt0)
#define DT1 (env->dt1)
#define QT0 (env->qt0)
#define QT1 (env->qt1)

#include "cpu.h"
#include "exec-all.h"

static inline void env_to_regs(void)
{
#if defined(reg_REGWPTR)
    REGWPTR = env->regbase + (env->cwp * 16);
    env->regwptr = REGWPTR;
#endif
}

static inline void regs_to_env(void)
{
}

int cpu_sparc_handle_mmu_fault(CPUState *env1, target_ulong address, int rw,
                               int mmu_idx, int is_softmmu);
void do_interrupt(int intno);

static inline int cpu_halted(CPUState *env1) {
    if (!env1->halted)
        return 0;
    if ((env1->interrupt_request & CPU_INTERRUPT_HARD) && (env1->psret != 0)) {
        env1->halted = 0;
        return 0;
    }
    return EXCP_HALTED;
}

#endif
