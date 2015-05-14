/*
 * Tiny Code Generator for QEMU
 *
 * Copyright (c) 2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* define it to suppress various consistency checks (faster) */
#define NDEBUG

/* define it to use liveness analysis (better code) */
#define USE_LIVENESS_ANALYSIS

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#ifdef _WIN32
#include <malloc.h>
#endif

#include "config.h"
#include "qemu-common.h"

/* Note: the long term plan is to reduce the dependancies on the QEMU
   CPU definitions. Currently they are used for qemu_ld/st
   instructions */
#define NO_CPU_IO_DEFS
#include "cpu.h"
#include "exec-all.h"

#include "tcg-op.h"
#include "elf.h"


static void patch_reloc(uint8_t *code_ptr, int type, 
                        tcg_target_long value, tcg_target_long addend);

TCGOpDef tcg_op_defs[] = {
#define DEF(s, n, copy_size) { #s, 0, 0, n, n, 0, copy_size },
#define DEF2(s, iargs, oargs, cargs, flags) { #s, iargs, oargs, cargs, iargs + oargs + cargs, flags, 0 },
#include "tcg-opc.h"
#undef DEF
#undef DEF2
};

TCGRegSet tcg_target_available_regs[2];
TCGRegSet tcg_target_call_clobber_regs;

/* XXX: move that inside the context */
uint16_t *gen_opc_ptr;
TCGArg *gen_opparam_ptr;

static inline void tcg_out8(TCGContext *s, uint8_t v)
{
    *s->code_ptr++ = v;
}

static inline void tcg_out16(TCGContext *s, uint16_t v)
{
    *(uint16_t *)s->code_ptr = v;
    s->code_ptr += 2;
}

static inline void tcg_out32(TCGContext *s, uint32_t v)
{
    *(uint32_t *)s->code_ptr = v;
    s->code_ptr += 4;
}

/* label relocation processing */

void tcg_out_reloc(TCGContext *s, uint8_t *code_ptr, int type, 
                   int label_index, long addend)
{
    TCGLabel *l;
    TCGRelocation *r;

    l = &s->labels[label_index];
    if (l->has_value) {
        /* FIXME: This may break relocations on RISC targets that
           modify instruction fields in place.  The caller may not have 
           written the initial value.  */
        patch_reloc(code_ptr, type, l->u.value, addend);
    } else {
        /* add a new relocation entry */
        r = tcg_malloc(sizeof(TCGRelocation));
        r->type = type;
        r->ptr = code_ptr;
        r->addend = addend;
        r->next = l->u.first_reloc;
        l->u.first_reloc = r;
    }
}

static void tcg_out_label(TCGContext *s, int label_index, 
                          tcg_target_long value)
{
    TCGLabel *l;
    TCGRelocation *r;

    l = &s->labels[label_index];
    if (l->has_value)
        tcg_abort();
    r = l->u.first_reloc;
    while (r != NULL) {
        patch_reloc(r->ptr, r->type, value, r->addend);
        r = r->next;
    }
    l->has_value = 1;
    l->u.value = value;
}

int gen_new_label(void)
{
    TCGContext *s = &tcg_ctx;
    int idx;
    TCGLabel *l;

    if (s->nb_labels >= TCG_MAX_LABELS)
        tcg_abort();
    idx = s->nb_labels++;
    l = &s->labels[idx];
    l->has_value = 0;
    l->u.first_reloc = NULL;
    return idx;
}

#include "tcg-target.c"

/* pool based memory allocation */
void *tcg_malloc_internal(TCGContext *s, int size)
{
    TCGPool *p;
    int pool_size;
    
    if (size > TCG_POOL_CHUNK_SIZE) {
        /* big malloc: insert a new pool (XXX: could optimize) */
        p = qemu_malloc(sizeof(TCGPool) + size);
        p->size = size;
        if (s->pool_current)
            s->pool_current->next = p;
        else
            s->pool_first = p;
        p->next = s->pool_current;
    } else {
        p = s->pool_current;
        if (!p) {
            p = s->pool_first;
            if (!p)
                goto new_pool;
        } else {
            if (!p->next) {
            new_pool:
                pool_size = TCG_POOL_CHUNK_SIZE;
                p = qemu_malloc(sizeof(TCGPool) + pool_size);
                p->size = pool_size;
                p->next = NULL;
                if (s->pool_current) 
                    s->pool_current->next = p;
                else
                    s->pool_first = p;
            } else {
                p = p->next;
            }
        }
    }
    s->pool_current = p;
    s->pool_cur = p->data + size;
    s->pool_end = p->data + p->size;
    return p->data;
}

void tcg_pool_reset(TCGContext *s)
{
    s->pool_cur = s->pool_end = NULL;
    s->pool_current = NULL;
}

/* free all the pool */
void tcg_pool_free(TCGContext *s)
{
    TCGPool *p, *p1;

    for(p = s->pool_first; p != NULL; p = p1) {
        p1 = p->next;
        qemu_free(p);
    }
    s->pool_first = NULL;
    s->pool_cur = s->pool_end = NULL;
}

void tcg_context_init(TCGContext *s)
{
    int op, total_args, n;
    TCGOpDef *def;
    TCGArgConstraint *args_ct;
    int *sorted_args;

    memset(s, 0, sizeof(*s));
    s->temps = s->static_temps;
    s->nb_globals = 0;
    
    /* Count total number of arguments and allocate the corresponding
       space */
    total_args = 0;
    for(op = 0; op < NB_OPS; op++) {
        def = &tcg_op_defs[op];
        n = def->nb_iargs + def->nb_oargs;
        total_args += n;
    }

    args_ct = qemu_malloc(sizeof(TCGArgConstraint) * total_args);
    sorted_args = qemu_malloc(sizeof(int) * total_args);

    for(op = 0; op < NB_OPS; op++) {
        def = &tcg_op_defs[op];
        def->args_ct = args_ct;
        def->sorted_args = sorted_args;
        n = def->nb_iargs + def->nb_oargs;
        sorted_args += n;
        args_ct += n;
    }
    
    tcg_target_init(s);

    /* init global prologue and epilogue */
    s->code_buf = code_gen_prologue;
    s->code_ptr = s->code_buf;
    tcg_target_qemu_prologue(s);
    flush_icache_range((unsigned long)s->code_buf, 
                       (unsigned long)s->code_ptr);
}

void tcg_set_frame(TCGContext *s, int reg,
                   tcg_target_long start, tcg_target_long size)
{
    s->frame_start = start;
    s->frame_end = start + size;
    s->frame_reg = reg;
}

void tcg_set_macro_func(TCGContext *s, TCGMacroFunc *func)
{
    s->macro_func = func;
}

void tcg_func_start(TCGContext *s)
{
    tcg_pool_reset(s);
    s->nb_temps = s->nb_globals;
    s->labels = tcg_malloc(sizeof(TCGLabel) * TCG_MAX_LABELS);
    s->nb_labels = 0;
    s->current_frame_offset = s->frame_start;

    gen_opc_ptr = gen_opc_buf;
    gen_opparam_ptr = gen_opparam_buf;
}

static inline void tcg_temp_alloc(TCGContext *s, int n)
{
    if (n > TCG_MAX_TEMPS)
        tcg_abort();
}

TCGv tcg_global_reg_new(TCGType type, int reg, const char *name)
{
    TCGContext *s = &tcg_ctx;
    TCGTemp *ts;
    int idx;

#if TCG_TARGET_REG_BITS == 32
    if (type != TCG_TYPE_I32)
        tcg_abort();
#endif
    if (tcg_regset_test_reg(s->reserved_regs, reg))
        tcg_abort();
    idx = s->nb_globals;
    tcg_temp_alloc(s, s->nb_globals + 1);
    ts = &s->temps[s->nb_globals];
    ts->base_type = type;
    ts->type = type;
    ts->fixed_reg = 1;
    ts->reg = reg;
    ts->val_type = TEMP_VAL_REG;
    ts->name = name;
    s->nb_globals++;
    tcg_regset_set_reg(s->reserved_regs, reg);
    return MAKE_TCGV(idx);
}

#if TCG_TARGET_REG_BITS == 32
/* temporary hack to avoid register shortage for tcg_qemu_st64() */
TCGv tcg_global_reg2_new_hack(TCGType type, int reg1, int reg2, 
                              const char *name)
{
    TCGContext *s = &tcg_ctx;
    TCGTemp *ts;
    int idx;
    char buf[64];

    if (type != TCG_TYPE_I64)
        tcg_abort();
    idx = s->nb_globals;
    tcg_temp_alloc(s, s->nb_globals + 2);
    ts = &s->temps[s->nb_globals];
    ts->base_type = type;
    ts->type = TCG_TYPE_I32;
    ts->fixed_reg = 1;
    ts->reg = reg1;
    ts->val_type = TEMP_VAL_REG;
    pstrcpy(buf, sizeof(buf), name);
    pstrcat(buf, sizeof(buf), "_0");
    ts->name = strdup(buf);

    ts++;
    ts->base_type = type;
    ts->type = TCG_TYPE_I32;
    ts->fixed_reg = 1;
    ts->reg = reg2;
    ts->val_type = TEMP_VAL_REG;
    pstrcpy(buf, sizeof(buf), name);
    pstrcat(buf, sizeof(buf), "_1");
    ts->name = strdup(buf);

    s->nb_globals += 2;
    return MAKE_TCGV(idx);
}
#endif

TCGv tcg_global_mem_new(TCGType type, int reg, tcg_target_long offset,
                        const char *name)
{
    TCGContext *s = &tcg_ctx;
    TCGTemp *ts;
    int idx;

    idx = s->nb_globals;
#if TCG_TARGET_REG_BITS == 32
    if (type == TCG_TYPE_I64) {
        char buf[64];
        tcg_temp_alloc(s, s->nb_globals + 1);
        ts = &s->temps[s->nb_globals];
        ts->base_type = type;
        ts->type = TCG_TYPE_I32;
        ts->fixed_reg = 0;
        ts->mem_allocated = 1;
        ts->mem_reg = reg;
#ifdef TCG_TARGET_WORDS_BIGENDIAN
        ts->mem_offset = offset + 4;
#else
        ts->mem_offset = offset;
#endif
        ts->val_type = TEMP_VAL_MEM;
        pstrcpy(buf, sizeof(buf), name);
        pstrcat(buf, sizeof(buf), "_0");
        ts->name = strdup(buf);
        ts++;

        ts->base_type = type;
        ts->type = TCG_TYPE_I32;
        ts->fixed_reg = 0;
        ts->mem_allocated = 1;
        ts->mem_reg = reg;
#ifdef TCG_TARGET_WORDS_BIGENDIAN
        ts->mem_offset = offset;
#else
        ts->mem_offset = offset + 4;
#endif
        ts->val_type = TEMP_VAL_MEM;
        pstrcpy(buf, sizeof(buf), name);
        pstrcat(buf, sizeof(buf), "_1");
        ts->name = strdup(buf);

        s->nb_globals += 2;
    } else
#endif
    {
        tcg_temp_alloc(s, s->nb_globals + 1);
        ts = &s->temps[s->nb_globals];
        ts->base_type = type;
        ts->type = type;
        ts->fixed_reg = 0;
        ts->mem_allocated = 1;
        ts->mem_reg = reg;
        ts->mem_offset = offset;
        ts->val_type = TEMP_VAL_MEM;
        ts->name = name;
        s->nb_globals++;
    }
    return MAKE_TCGV(idx);
}

TCGv tcg_temp_new(TCGType type)
{
    TCGContext *s = &tcg_ctx;
    TCGTemp *ts;
    int idx;

    idx = s->nb_temps;
#if TCG_TARGET_REG_BITS == 32
    if (type == TCG_TYPE_I64) {
        tcg_temp_alloc(s, s->nb_temps + 1);
        ts = &s->temps[s->nb_temps];
        ts->base_type = type;
        ts->type = TCG_TYPE_I32;
        ts->fixed_reg = 0;
        ts->val_type = TEMP_VAL_DEAD;
        ts->mem_allocated = 0;
        ts->name = NULL;
        ts++;
        ts->base_type = TCG_TYPE_I32;
        ts->type = TCG_TYPE_I32;
        ts->val_type = TEMP_VAL_DEAD;
        ts->fixed_reg = 0;
        ts->mem_allocated = 0;
        ts->name = NULL;
        s->nb_temps += 2;
    } else
#endif
    {
        tcg_temp_alloc(s, s->nb_temps + 1);
        ts = &s->temps[s->nb_temps];
        ts->base_type = type;
        ts->type = type;
        ts->fixed_reg = 0;
        ts->val_type = TEMP_VAL_DEAD;
        ts->mem_allocated = 0;
        ts->name = NULL;
        s->nb_temps++;
    }
    return MAKE_TCGV(idx);
}

TCGv tcg_const_i32(int32_t val)
{
    TCGContext *s = &tcg_ctx;
    TCGTemp *ts;
    int idx;

    idx = s->nb_temps;
    tcg_temp_alloc(s, idx + 1);
    ts = &s->temps[idx];
    ts->base_type = ts->type = TCG_TYPE_I32;
    ts->val_type = TEMP_VAL_CONST;
    ts->name = NULL;
    ts->val = val;
    s->nb_temps++;
    return MAKE_TCGV(idx);
}

TCGv tcg_const_i64(int64_t val)
{
    TCGContext *s = &tcg_ctx;
    TCGTemp *ts;
    int idx;

    idx = s->nb_temps;
#if TCG_TARGET_REG_BITS == 32
    tcg_temp_alloc(s, idx + 2);
    ts = &s->temps[idx];
    ts->base_type = TCG_TYPE_I64;
    ts->type = TCG_TYPE_I32;
    ts->val_type = TEMP_VAL_CONST;
    ts->name = NULL;
    ts->val = val;
    ts++;
    ts->base_type = TCG_TYPE_I32;
    ts->type = TCG_TYPE_I32;
    ts->val_type = TEMP_VAL_CONST;
    ts->name = NULL;
    ts->val = val >> 32;
    s->nb_temps += 2;
#else
    tcg_temp_alloc(s, idx + 1);
    ts = &s->temps[idx];
    ts->base_type = ts->type = TCG_TYPE_I64;
    ts->val_type = TEMP_VAL_CONST;
    ts->name = NULL;
    ts->val = val;
    s->nb_temps++;
#endif    
    return MAKE_TCGV(idx);
}

void tcg_register_helper(void *func, const char *name)
{
    TCGContext *s = &tcg_ctx;
    int n;
    if ((s->nb_helpers + 1) > s->allocated_helpers) {
        n = s->allocated_helpers;
        if (n == 0) {
            n = 4;
        } else {
            n *= 2;
        }
        s->helpers = realloc(s->helpers, n * sizeof(TCGHelperInfo));
        s->allocated_helpers = n;
    }
    s->helpers[s->nb_helpers].func = func;
    s->helpers[s->nb_helpers].name = name;
    s->nb_helpers++;
}

const char *tcg_helper_get_name(TCGContext *s, void *func)
{
    int i;

    for(i = 0; i < s->nb_helpers; i++) {
        if (s->helpers[i].func == func)
            return s->helpers[i].name;
    }
    return NULL;
}

static inline TCGType tcg_get_base_type(TCGContext *s, TCGv arg)
{
    return s->temps[GET_TCGV(arg)].base_type;
}

static void tcg_gen_call_internal(TCGContext *s, TCGv func, 
                                  unsigned int flags,
                                  unsigned int nb_rets, const TCGv *rets,
                                  unsigned int nb_params, const TCGv *params)
{
    int i;
    *gen_opc_ptr++ = INDEX_op_call;
    *gen_opparam_ptr++ = (nb_rets << 16) | (nb_params + 1);
    for(i = 0; i < nb_rets; i++) {
        *gen_opparam_ptr++ = GET_TCGV(rets[i]);
    }
    for(i = 0; i < nb_params; i++) {
        *gen_opparam_ptr++ = GET_TCGV(params[i]);
    }
    *gen_opparam_ptr++ = GET_TCGV(func);

    *gen_opparam_ptr++ = flags;
    /* total parameters, needed to go backward in the instruction stream */
    *gen_opparam_ptr++ = 1 + nb_rets + nb_params + 3;
}


#if TCG_TARGET_REG_BITS < 64
/* Note: we convert the 64 bit args to 32 bit */
void tcg_gen_call(TCGContext *s, TCGv func, unsigned int flags,
                  unsigned int nb_rets, const TCGv *rets,
                  unsigned int nb_params, const TCGv *args1)
{
    TCGv ret, *args2, rets_2[2], arg;
    int j, i, call_type;

    if (nb_rets == 1) {
        ret = rets[0];
        if (tcg_get_base_type(s, ret) == TCG_TYPE_I64) {
            nb_rets = 2;
            rets_2[0] = ret;
            rets_2[1] = TCGV_HIGH(ret);
            rets = rets_2;
        }
    }
    args2 = alloca((nb_params * 2) * sizeof(TCGv));
    j = 0;
    call_type = (flags & TCG_CALL_TYPE_MASK);
    for(i = 0; i < nb_params; i++) {
        arg = args1[i];
        if (tcg_get_base_type(s, arg) == TCG_TYPE_I64) {
#ifdef TCG_TARGET_I386
            /* REGPARM case: if the third parameter is 64 bit, it is
               allocated on the stack */
            if (j == 2 && call_type == TCG_CALL_TYPE_REGPARM) {
                call_type = TCG_CALL_TYPE_REGPARM_2;
                flags = (flags & ~TCG_CALL_TYPE_MASK) | call_type;
            }
            args2[j++] = arg;
            args2[j++] = TCGV_HIGH(arg);
#else
#ifdef TCG_TARGET_WORDS_BIGENDIAN
            args2[j++] = TCGV_HIGH(arg);
            args2[j++] = arg;
#else
            args2[j++] = arg;
            args2[j++] = TCGV_HIGH(arg);
#endif
#endif
        } else {
            args2[j++] = arg;
        }
    }
    tcg_gen_call_internal(s, func, flags, 
                          nb_rets, rets, j, args2);
}
#else
void tcg_gen_call(TCGContext *s, TCGv func, unsigned int flags,
                  unsigned int nb_rets, const TCGv *rets,
                  unsigned int nb_params, const TCGv *args1)
{
    tcg_gen_call_internal(s, func, flags, 
                          nb_rets, rets, nb_params, args1);
}
#endif

#if TCG_TARGET_REG_BITS == 32
void tcg_gen_shifti_i64(TCGv ret, TCGv arg1, 
                        int c, int right, int arith)
{
    if (c == 0)
        return;
    if (c >= 32) {
        c -= 32;
        if (right) {
            if (arith) {
                tcg_gen_sari_i32(ret, TCGV_HIGH(arg1), c);
                tcg_gen_sari_i32(TCGV_HIGH(ret), TCGV_HIGH(arg1), 31);
            } else {
                tcg_gen_shri_i32(ret, TCGV_HIGH(arg1), c);
                tcg_gen_movi_i32(TCGV_HIGH(ret), 0);
            }
        } else {
            tcg_gen_shli_i32(TCGV_HIGH(ret), arg1, c);
            tcg_gen_movi_i32(ret, 0);
        }
    } else {
        TCGv t0, t1;

        t0 = tcg_temp_new(TCG_TYPE_I32);
        t1 = tcg_temp_new(TCG_TYPE_I32);
        if (right) {
            tcg_gen_shli_i32(t0, TCGV_HIGH(arg1), 32 - c);
            if (arith)
                tcg_gen_sari_i32(t1, TCGV_HIGH(arg1), c);
            else 
                tcg_gen_shri_i32(t1, TCGV_HIGH(arg1), c);
            tcg_gen_shri_i32(ret, arg1, c); 
            tcg_gen_or_i32(ret, ret, t0);
            tcg_gen_mov_i32(TCGV_HIGH(ret), t1);
        } else {
            tcg_gen_shri_i32(t0, arg1, 32 - c);
            /* Note: ret can be the same as arg1, so we use t1 */
            tcg_gen_shli_i32(t1, arg1, c); 
            tcg_gen_shli_i32(TCGV_HIGH(ret), TCGV_HIGH(arg1), c);
            tcg_gen_or_i32(TCGV_HIGH(ret), TCGV_HIGH(ret), t0);
            tcg_gen_mov_i32(ret, t1);
        }
    }
}
#endif

void tcg_reg_alloc_start(TCGContext *s)
{
    int i;
    TCGTemp *ts;
    for(i = 0; i < s->nb_globals; i++) {
        ts = &s->temps[i];
        if (ts->fixed_reg) {
            ts->val_type = TEMP_VAL_REG;
        } else {
            ts->val_type = TEMP_VAL_MEM;
        }
    }
    for(i = 0; i < TCG_TARGET_NB_REGS; i++) {
        s->reg_to_temp[i] = -1;
    }
}

static char *tcg_get_arg_str_idx(TCGContext *s, char *buf, int buf_size,
                                 int idx)
{
    TCGTemp *ts;

    ts = &s->temps[idx];
    if (idx < s->nb_globals) {
        pstrcpy(buf, buf_size, ts->name);
    } else {
        if (ts->val_type == TEMP_VAL_CONST) {
            snprintf(buf, buf_size, "$0x%" TCG_PRIlx , ts->val);
        } else {
            snprintf(buf, buf_size, "tmp%d", idx - s->nb_globals);
        }
    }
    return buf;
}

char *tcg_get_arg_str(TCGContext *s, char *buf, int buf_size, TCGv arg)
{
    return tcg_get_arg_str_idx(s, buf, buf_size, GET_TCGV(arg));
}

void tcg_dump_ops(TCGContext *s, FILE *outfile)
{
    const uint16_t *opc_ptr;
    const TCGArg *args;
    TCGArg arg;
    int c, i, k, nb_oargs, nb_iargs, nb_cargs;
    const TCGOpDef *def;
    char buf[128];

    opc_ptr = gen_opc_buf;
    args = gen_opparam_buf;
    while (opc_ptr < gen_opc_ptr) {
        c = *opc_ptr++;
        def = &tcg_op_defs[c];
        fprintf(outfile, " %s ", def->name);
        if (c == INDEX_op_call) {
            TCGArg arg;
            /* variable number of arguments */
            arg = *args++;
            nb_oargs = arg >> 16;
            nb_iargs = arg & 0xffff;
            nb_cargs = def->nb_cargs;

            /* function name */
            /* XXX: dump helper name for call */
            fprintf(outfile, "%s",
                    tcg_get_arg_str_idx(s, buf, sizeof(buf), args[nb_oargs + nb_iargs - 1]));
            /* flags */
            fprintf(outfile, ",$0x%" TCG_PRIlx,
                    args[nb_oargs + nb_iargs]);
            /* nb out args */
            fprintf(outfile, ",$%d", nb_oargs);
            for(i = 0; i < nb_oargs; i++) {
                fprintf(outfile, ",");
                fprintf(outfile, "%s",
                        tcg_get_arg_str_idx(s, buf, sizeof(buf), args[i]));
            }
            for(i = 0; i < (nb_iargs - 1); i++) {
                fprintf(outfile, ",");
                fprintf(outfile, "%s",
                        tcg_get_arg_str_idx(s, buf, sizeof(buf), args[nb_oargs + i]));
            }
        } else {
            if (c == INDEX_op_nopn) {
                /* variable number of arguments */
                nb_cargs = *args;
                nb_oargs = 0;
                nb_iargs = 0;
            } else {
                nb_oargs = def->nb_oargs;
                nb_iargs = def->nb_iargs;
                nb_cargs = def->nb_cargs;
            }
            
            k = 0;
            for(i = 0; i < nb_oargs; i++) {
                if (k != 0)
                    fprintf(outfile, ",");
                fprintf(outfile, "%s",
                        tcg_get_arg_str_idx(s, buf, sizeof(buf), args[k++]));
            }
            for(i = 0; i < nb_iargs; i++) {
                if (k != 0)
                    fprintf(outfile, ",");
                fprintf(outfile, "%s",
                        tcg_get_arg_str_idx(s, buf, sizeof(buf), args[k++]));
            }
            for(i = 0; i < nb_cargs; i++) {
                if (k != 0)
                    fprintf(outfile, ",");
                arg = args[k++];
                fprintf(outfile, "$0x%" TCG_PRIlx, arg);
            }
        }
        fprintf(outfile, "\n");
        args += nb_iargs + nb_oargs + nb_cargs;
    }
}

/* we give more priority to constraints with less registers */
static int get_constraint_priority(const TCGOpDef *def, int k)
{
    const TCGArgConstraint *arg_ct;

    int i, n;
    arg_ct = &def->args_ct[k];
    if (arg_ct->ct & TCG_CT_ALIAS) {
        /* an alias is equivalent to a single register */
        n = 1;
    } else {
        if (!(arg_ct->ct & TCG_CT_REG))
            return 0;
        n = 0;
        for(i = 0; i < TCG_TARGET_NB_REGS; i++) {
            if (tcg_regset_test_reg(arg_ct->u.regs, i))
                n++;
        }
    }
    return TCG_TARGET_NB_REGS - n + 1;
}

/* sort from highest priority to lowest */
static void sort_constraints(TCGOpDef *def, int start, int n)
{
    int i, j, p1, p2, tmp;

    for(i = 0; i < n; i++)
        def->sorted_args[start + i] = start + i;
    if (n <= 1)
        return;
    for(i = 0; i < n - 1; i++) {
        for(j = i + 1; j < n; j++) {
            p1 = get_constraint_priority(def, def->sorted_args[start + i]);
            p2 = get_constraint_priority(def, def->sorted_args[start + j]);
            if (p1 < p2) {
                tmp = def->sorted_args[start + i];
                def->sorted_args[start + i] = def->sorted_args[start + j];
                def->sorted_args[start + j] = tmp;
            }
        }
    }
}

void tcg_add_target_add_op_defs(const TCGTargetOpDef *tdefs)
{
    int op;
    TCGOpDef *def;
    const char *ct_str;
    int i, nb_args;

    for(;;) {
        if (tdefs->op < 0)
            break;
        op = tdefs->op;
        assert(op >= 0 && op < NB_OPS);
        def = &tcg_op_defs[op];
        nb_args = def->nb_iargs + def->nb_oargs;
        for(i = 0; i < nb_args; i++) {
            ct_str = tdefs->args_ct_str[i];
            tcg_regset_clear(def->args_ct[i].u.regs);
            def->args_ct[i].ct = 0;
            if (ct_str[0] >= '0' && ct_str[0] <= '9') {
                int oarg;
                oarg = ct_str[0] - '0';
                assert(oarg < def->nb_oargs);
                assert(def->args_ct[oarg].ct & TCG_CT_REG);
                /* TCG_CT_ALIAS is for the output arguments. The input
                   argument is tagged with TCG_CT_IALIAS. */
                def->args_ct[i] = def->args_ct[oarg];
                def->args_ct[oarg].ct = TCG_CT_ALIAS;
                def->args_ct[oarg].alias_index = i;
                def->args_ct[i].ct |= TCG_CT_IALIAS;
                def->args_ct[i].alias_index = oarg;
            } else {
                for(;;) {
                    if (*ct_str == '\0')
                        break;
                    switch(*ct_str) {
                    case 'i':
                        def->args_ct[i].ct |= TCG_CT_CONST;
                        ct_str++;
                        break;
                    default:
                        if (target_parse_constraint(&def->args_ct[i], &ct_str) < 0) {
                            fprintf(stderr, "Invalid constraint '%s' for arg %d of operation '%s'\n",
                                    ct_str, i, def->name);
                            exit(1);
                        }
                    }
                }
            }
        }

        /* sort the constraints (XXX: this is just an heuristic) */
        sort_constraints(def, 0, def->nb_oargs);
        sort_constraints(def, def->nb_oargs, def->nb_iargs);

#if 0
        {
            int i;

            printf("%s: sorted=", def->name);
            for(i = 0; i < def->nb_oargs + def->nb_iargs; i++)
                printf(" %d", def->sorted_args[i]);
            printf("\n");
        }
#endif
        tdefs++;
    }

}

#ifdef USE_LIVENESS_ANALYSIS

/* set a nop for an operation using 'nb_args' */
static inline void tcg_set_nop(TCGContext *s, uint16_t *opc_ptr, 
                               TCGArg *args, int nb_args)
{
    if (nb_args == 0) {
        *opc_ptr = INDEX_op_nop;
    } else {
        *opc_ptr = INDEX_op_nopn;
        args[0] = nb_args;
        args[nb_args - 1] = nb_args;
    }
}

/* liveness analysis: end of basic block: globals are live, temps are dead */
static inline void tcg_la_bb_end(TCGContext *s, uint8_t *dead_temps)
{
    memset(dead_temps, 0, s->nb_globals);
    memset(dead_temps + s->nb_globals, 1, s->nb_temps - s->nb_globals);
}

/* Liveness analysis : update the opc_dead_iargs array to tell if a
   given input arguments is dead. Instructions updating dead
   temporaries are removed. */
void tcg_liveness_analysis(TCGContext *s)
{
    int i, op_index, op, nb_args, nb_iargs, nb_oargs, arg, nb_ops;
    TCGArg *args;
    const TCGOpDef *def;
    uint8_t *dead_temps;
    unsigned int dead_iargs;
    
    gen_opc_ptr++; /* skip end */

    nb_ops = gen_opc_ptr - gen_opc_buf;

    /* XXX: make it really dynamic */
    s->op_dead_iargs = tcg_malloc(OPC_BUF_SIZE * sizeof(uint16_t));
    
    dead_temps = tcg_malloc(s->nb_temps);
    memset(dead_temps, 1, s->nb_temps);

    args = gen_opparam_ptr;
    op_index = nb_ops - 1;
    while (op_index >= 0) {
        op = gen_opc_buf[op_index];
        def = &tcg_op_defs[op];
        switch(op) {
        case INDEX_op_call:
            nb_args = args[-1];
            args -= nb_args;
            nb_iargs = args[0] & 0xffff;
            nb_oargs = args[0] >> 16;
            args++;

            /* output args are dead */
            for(i = 0; i < nb_oargs; i++) {
                arg = args[i];
                dead_temps[arg] = 1;
            }
            
            /* globals are live (they may be used by the call) */
            memset(dead_temps, 0, s->nb_globals);

            /* input args are live */
            dead_iargs = 0;
            for(i = 0; i < nb_iargs; i++) {
                arg = args[i + nb_oargs];
                if (dead_temps[arg]) {
                    dead_iargs |= (1 << i);
                }
                dead_temps[arg] = 0;
            }
            s->op_dead_iargs[op_index] = dead_iargs;
            args--;
            break;
        case INDEX_op_set_label:
            args--;
            /* mark end of basic block */
            tcg_la_bb_end(s, dead_temps);
            break;
        case INDEX_op_nopn:
            nb_args = args[-1];
            args -= nb_args;
            break;
        case INDEX_op_discard:
            args--;
            /* mark the temporary as dead */
            dead_temps[args[0]] = 1;
            break;
        case INDEX_op_macro_2:
            {
                int dead_args[2], macro_id;
                int saved_op_index, saved_arg_index;
                int macro_op_index, macro_arg_index;
                int macro_end_op_index, macro_end_arg_index;
                int last_nb_temps;
                
                nb_args = 3;
                args -= nb_args;
                dead_args[0] = dead_temps[args[0]];
                dead_args[1] = dead_temps[args[1]];
                macro_id = args[2];

                /* call the macro function which generate code
                   depending on the live outputs */
                saved_op_index = op_index;
                saved_arg_index = args - gen_opparam_buf;

                /* add a macro start instruction */
                *gen_opc_ptr++ = INDEX_op_macro_start;
                *gen_opparam_ptr++ = saved_op_index;
                *gen_opparam_ptr++ = saved_arg_index;

                macro_op_index = gen_opc_ptr - gen_opc_buf;
                macro_arg_index = gen_opparam_ptr -  gen_opparam_buf;

                last_nb_temps = s->nb_temps;

                s->macro_func(s, macro_id, dead_args);

                /* realloc temp info (XXX: make it faster) */
                if (s->nb_temps > last_nb_temps) {
                    uint8_t *new_dead_temps;

                    new_dead_temps = tcg_malloc(s->nb_temps);
                    memcpy(new_dead_temps, dead_temps, last_nb_temps);
                    memset(new_dead_temps + last_nb_temps, 1, 
                           s->nb_temps - last_nb_temps);
                    dead_temps = new_dead_temps;
                }

                macro_end_op_index = gen_opc_ptr - gen_opc_buf;
                macro_end_arg_index = gen_opparam_ptr - gen_opparam_buf;

                /* end of macro: add a goto to the next instruction */
                *gen_opc_ptr++ = INDEX_op_macro_end;
                *gen_opparam_ptr++ = op_index + 1;
                *gen_opparam_ptr++ = saved_arg_index + nb_args;

                /* modify the macro operation to be a macro_goto */
                gen_opc_buf[op_index] = INDEX_op_macro_goto;
                args[0] = macro_op_index;
                args[1] = macro_arg_index;
                args[2] = 0; /* dummy third arg to match the 
                                macro parameters */

                /* set the next instruction to the end of the macro */
                op_index = macro_end_op_index;
                args = macro_end_arg_index + gen_opparam_buf;
            }
            break;
        case INDEX_op_macro_start:
            args -= 2;
            op_index = args[0];
            args = gen_opparam_buf + args[1];
            break;
        case INDEX_op_macro_goto:
        case INDEX_op_macro_end:
            tcg_abort(); /* should never happen in liveness analysis */
        case INDEX_op_end:
            break;
            /* XXX: optimize by hardcoding common cases (e.g. triadic ops) */
        default:
            if (op > INDEX_op_end) {
                args -= def->nb_args;
                nb_iargs = def->nb_iargs;
                nb_oargs = def->nb_oargs;

                /* Test if the operation can be removed because all
                   its outputs are dead. We assume that nb_oargs == 0
                   implies side effects */
                if (!(def->flags & TCG_OPF_SIDE_EFFECTS) && nb_oargs != 0) {
                    for(i = 0; i < nb_oargs; i++) {
                        arg = args[i];
                        if (!dead_temps[arg])
                            goto do_not_remove;
                    }
                    tcg_set_nop(s, gen_opc_buf + op_index, args, def->nb_args);
#ifdef CONFIG_PROFILER
                    {
                        extern int64_t dyngen_tcg_del_op_count;
                        dyngen_tcg_del_op_count++;
                    }
#endif
                } else {
                do_not_remove:

                    /* output args are dead */
                    for(i = 0; i < nb_oargs; i++) {
                        arg = args[i];
                        dead_temps[arg] = 1;
                    }
                    
                    /* if end of basic block, update */
                    if (def->flags & TCG_OPF_BB_END) {
                        tcg_la_bb_end(s, dead_temps);
                    } else if (def->flags & TCG_OPF_CALL_CLOBBER) {
                        /* globals are live */
                        memset(dead_temps, 0, s->nb_globals);
                    }
                    
                    /* input args are live */
                    dead_iargs = 0;
                    for(i = 0; i < nb_iargs; i++) {
                        arg = args[i + nb_oargs];
                        if (dead_temps[arg]) {
                            dead_iargs |= (1 << i);
                        }
                        dead_temps[arg] = 0;
                    }
                    s->op_dead_iargs[op_index] = dead_iargs;
                }
            } else {
                /* legacy dyngen operations */
                args -= def->nb_args;
                /* mark end of basic block */
                tcg_la_bb_end(s, dead_temps);
            }
            break;
        }
        op_index--;
    }

    if (args != gen_opparam_buf)
        tcg_abort();
}
#else
/* dummy liveness analysis */
void tcg_liveness_analysis(TCGContext *s)
{
    int nb_ops;
    nb_ops = gen_opc_ptr - gen_opc_buf;

    s->op_dead_iargs = tcg_malloc(nb_ops * sizeof(uint16_t));
    memset(s->op_dead_iargs, 0, nb_ops * sizeof(uint16_t));
}
#endif

#ifndef NDEBUG
static void dump_regs(TCGContext *s)
{
    TCGTemp *ts;
    int i;
    char buf[64];

    for(i = 0; i < s->nb_temps; i++) {
        ts = &s->temps[i];
        printf("  %10s: ", tcg_get_arg_str_idx(s, buf, sizeof(buf), i));
        switch(ts->val_type) {
        case TEMP_VAL_REG:
            printf("%s", tcg_target_reg_names[ts->reg]);
            break;
        case TEMP_VAL_MEM:
            printf("%d(%s)", (int)ts->mem_offset, tcg_target_reg_names[ts->mem_reg]);
            break;
        case TEMP_VAL_CONST:
            printf("$0x%" TCG_PRIlx, ts->val);
            break;
        case TEMP_VAL_DEAD:
            printf("D");
            break;
        default:
            printf("???");
            break;
        }
        printf("\n");
    }

    for(i = 0; i < TCG_TARGET_NB_REGS; i++) {
        if (s->reg_to_temp[i] >= 0) {
            printf("%s: %s\n", 
                   tcg_target_reg_names[i], 
                   tcg_get_arg_str_idx(s, buf, sizeof(buf), s->reg_to_temp[i]));
        }
    }
}

static void check_regs(TCGContext *s)
{
    int reg, k;
    TCGTemp *ts;
    char buf[64];

    for(reg = 0; reg < TCG_TARGET_NB_REGS; reg++) {
        k = s->reg_to_temp[reg];
        if (k >= 0) {
            ts = &s->temps[k];
            if (ts->val_type != TEMP_VAL_REG ||
                ts->reg != reg) {
                printf("Inconsistency for register %s:\n", 
                       tcg_target_reg_names[reg]);
                goto fail;
            }
        }
    }
    for(k = 0; k < s->nb_temps; k++) {
        ts = &s->temps[k];
        if (ts->val_type == TEMP_VAL_REG &&
            !ts->fixed_reg &&
            s->reg_to_temp[ts->reg] != k) {
                printf("Inconsistency for temp %s:\n", 
                       tcg_get_arg_str_idx(s, buf, sizeof(buf), k));
        fail:
                printf("reg state:\n");
                dump_regs(s);
                tcg_abort();
        }
        if (ts->val_type == TEMP_VAL_CONST && k < s->nb_globals) {
            printf("constant forbidden in global %s\n",
                   tcg_get_arg_str_idx(s, buf, sizeof(buf), k));
            goto fail;
        }
    }
}
#endif

static void temp_allocate_frame(TCGContext *s, int temp)
{
    TCGTemp *ts;
    ts = &s->temps[temp];
    s->current_frame_offset = (s->current_frame_offset + sizeof(tcg_target_long) - 1) & ~(sizeof(tcg_target_long) - 1);
    if (s->current_frame_offset + sizeof(tcg_target_long) > s->frame_end)
        tcg_abort();
    ts->mem_offset = s->current_frame_offset;
    ts->mem_reg = s->frame_reg;
    ts->mem_allocated = 1;
    s->current_frame_offset += sizeof(tcg_target_long);
}

/* free register 'reg' by spilling the corresponding temporary if necessary */
static void tcg_reg_free(TCGContext *s, int reg)
{
    TCGTemp *ts;
    int temp;

    temp = s->reg_to_temp[reg];
    if (temp != -1) {
        ts = &s->temps[temp];
        assert(ts->val_type == TEMP_VAL_REG);
        if (!ts->mem_coherent) {
            if (!ts->mem_allocated) 
                temp_allocate_frame(s, temp);
            tcg_out_st(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
        }
        ts->val_type = TEMP_VAL_MEM;
        s->reg_to_temp[reg] = -1;
    }
}

/* Allocate a register belonging to reg1 & ~reg2 */
static int tcg_reg_alloc(TCGContext *s, TCGRegSet reg1, TCGRegSet reg2)
{
    int i, reg;
    TCGRegSet reg_ct;

    tcg_regset_andnot(reg_ct, reg1, reg2);

    /* first try free registers */
    for(i = 0; i < ARRAY_SIZE(tcg_target_reg_alloc_order); i++) {
        reg = tcg_target_reg_alloc_order[i];
        if (tcg_regset_test_reg(reg_ct, reg) && s->reg_to_temp[reg] == -1)
            return reg;
    }

    /* XXX: do better spill choice */
    for(i = 0; i < ARRAY_SIZE(tcg_target_reg_alloc_order); i++) {
        reg = tcg_target_reg_alloc_order[i];
        if (tcg_regset_test_reg(reg_ct, reg)) {
            tcg_reg_free(s, reg);
            return reg;
        }
    }

    tcg_abort();
}

/* at the end of a basic block, we assume all temporaries are dead and
   all globals are stored at their canonical location */
/* XXX: optimize by handling constants in another array ? */
void tcg_reg_alloc_bb_end(TCGContext *s)
{
    TCGTemp *ts;
    int i;

    for(i = 0; i < s->nb_globals; i++) {
        ts = &s->temps[i];
        if (!ts->fixed_reg) {
            if (ts->val_type == TEMP_VAL_REG) {
                tcg_reg_free(s, ts->reg);
            }
        }
    }

    for(i = s->nb_globals; i < s->nb_temps; i++) {
        ts = &s->temps[i];
        if (ts->val_type != TEMP_VAL_CONST) {
            if (ts->val_type == TEMP_VAL_REG) {
                s->reg_to_temp[ts->reg] = -1;
            }
            ts->val_type = TEMP_VAL_DEAD;
        }
    }
}

#define IS_DEAD_IARG(n) ((dead_iargs >> (n)) & 1)

static void tcg_reg_alloc_mov(TCGContext *s, const TCGOpDef *def,
                              const TCGArg *args,
                              unsigned int dead_iargs)
{
    TCGTemp *ts, *ots;
    int reg;
    const TCGArgConstraint *arg_ct;

    ots = &s->temps[args[0]];
    ts = &s->temps[args[1]];
    arg_ct = &def->args_ct[0];

    if (ts->val_type == TEMP_VAL_REG) {
        if (IS_DEAD_IARG(0) && !ts->fixed_reg && !ots->fixed_reg) {
            /* the mov can be suppressed */
            if (ots->val_type == TEMP_VAL_REG)
                s->reg_to_temp[ots->reg] = -1;
            reg = ts->reg;
            s->reg_to_temp[reg] = -1;
            ts->val_type = TEMP_VAL_DEAD;
        } else {
            if (ots->val_type == TEMP_VAL_REG) {
                reg = ots->reg;
            } else {
                reg = tcg_reg_alloc(s, arg_ct->u.regs, s->reserved_regs);
            }
            if (ts->reg != reg) {
                tcg_out_mov(s, reg, ts->reg);
            }
        }
    } else if (ts->val_type == TEMP_VAL_MEM) {
        if (ots->val_type == TEMP_VAL_REG) {
            reg = ots->reg;
        } else {
            reg = tcg_reg_alloc(s, arg_ct->u.regs, s->reserved_regs);
        }
        tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
    } else if (ts->val_type == TEMP_VAL_CONST) {
        if (ots->val_type == TEMP_VAL_REG) {
            reg = ots->reg;
        } else {
            reg = tcg_reg_alloc(s, arg_ct->u.regs, s->reserved_regs);
        }
        tcg_out_movi(s, ots->type, reg, ts->val);
    } else {
        tcg_abort();
    }
    s->reg_to_temp[reg] = args[0];
    ots->reg = reg;
    ots->val_type = TEMP_VAL_REG;
    ots->mem_coherent = 0;
}

static void tcg_reg_alloc_op(TCGContext *s, 
                             const TCGOpDef *def, int opc,
                             const TCGArg *args,
                             unsigned int dead_iargs)
{
    TCGRegSet allocated_regs;
    int i, k, nb_iargs, nb_oargs, reg;
    TCGArg arg;
    const TCGArgConstraint *arg_ct;
    TCGTemp *ts;
    TCGArg new_args[TCG_MAX_OP_ARGS];
    int const_args[TCG_MAX_OP_ARGS];

    nb_oargs = def->nb_oargs;
    nb_iargs = def->nb_iargs;

    /* copy constants */
    memcpy(new_args + nb_oargs + nb_iargs, 
           args + nb_oargs + nb_iargs, 
           sizeof(TCGArg) * def->nb_cargs);

    /* satisfy input constraints */ 
    tcg_regset_set(allocated_regs, s->reserved_regs);
    for(k = 0; k < nb_iargs; k++) {
        i = def->sorted_args[nb_oargs + k];
        arg = args[i];
        arg_ct = &def->args_ct[i];
        ts = &s->temps[arg];
        if (ts->val_type == TEMP_VAL_MEM) {
            reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
            tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
            ts->val_type = TEMP_VAL_REG;
            ts->reg = reg;
            ts->mem_coherent = 1;
            s->reg_to_temp[reg] = arg;
        } else if (ts->val_type == TEMP_VAL_CONST) {
            if (tcg_target_const_match(ts->val, arg_ct)) {
                /* constant is OK for instruction */
                const_args[i] = 1;
                new_args[i] = ts->val;
                goto iarg_end;
            } else {
                /* need to move to a register*/
                reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
                tcg_out_movi(s, ts->type, reg, ts->val);
                goto iarg_end1;
            }
        }
        assert(ts->val_type == TEMP_VAL_REG);
        if (arg_ct->ct & TCG_CT_IALIAS) {
            if (ts->fixed_reg) {
                /* if fixed register, we must allocate a new register
                   if the alias is not the same register */
                if (arg != args[arg_ct->alias_index])
                    goto allocate_in_reg;
            } else {
                /* if the input is aliased to an output and if it is
                   not dead after the instruction, we must allocate
                   a new register and move it */
                if (!IS_DEAD_IARG(i - nb_oargs)) 
                    goto allocate_in_reg;
            }
        }
        reg = ts->reg;
        if (tcg_regset_test_reg(arg_ct->u.regs, reg)) {
            /* nothing to do : the constraint is satisfied */
        } else {
        allocate_in_reg:
            /* allocate a new register matching the constraint 
               and move the temporary register into it */
            reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
            tcg_out_mov(s, reg, ts->reg);
        }
    iarg_end1:
        new_args[i] = reg;
        const_args[i] = 0;
        tcg_regset_set_reg(allocated_regs, reg);
    iarg_end: ;
    }
    
    /* mark dead temporaries and free the associated registers */
    for(i = 0; i < nb_iargs; i++) {
        arg = args[nb_oargs + i];
        if (IS_DEAD_IARG(i)) {
            ts = &s->temps[arg];
            if (ts->val_type != TEMP_VAL_CONST && !ts->fixed_reg) {
                if (ts->val_type == TEMP_VAL_REG)
                    s->reg_to_temp[ts->reg] = -1;
                ts->val_type = TEMP_VAL_DEAD;
            }
        }
    }

    if (def->flags & TCG_OPF_CALL_CLOBBER) {
        /* XXX: permit generic clobber register list ? */ 
        for(reg = 0; reg < TCG_TARGET_NB_REGS; reg++) {
            if (tcg_regset_test_reg(tcg_target_call_clobber_regs, reg)) {
                tcg_reg_free(s, reg);
            }
        }
        /* XXX: for load/store we could do that only for the slow path
           (i.e. when a memory callback is called) */

        /* store globals and free associated registers (we assume the insn
           can modify any global. */
        for(i = 0; i < s->nb_globals; i++) {
            ts = &s->temps[i];
            if (!ts->fixed_reg) {
                if (ts->val_type == TEMP_VAL_REG) {
                    tcg_reg_free(s, ts->reg);
                }
            }
        }
    }

    /* satisfy the output constraints */
    tcg_regset_set(allocated_regs, s->reserved_regs);
    for(k = 0; k < nb_oargs; k++) {
        i = def->sorted_args[k];
        arg = args[i];
        arg_ct = &def->args_ct[i];
        ts = &s->temps[arg];
        if (arg_ct->ct & TCG_CT_ALIAS) {
            reg = new_args[arg_ct->alias_index];
        } else {
            /* if fixed register, we try to use it */
            reg = ts->reg;
            if (ts->fixed_reg &&
                tcg_regset_test_reg(arg_ct->u.regs, reg)) {
                goto oarg_end;
            }
            reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
        }
        tcg_regset_set_reg(allocated_regs, reg);
        /* if a fixed register is used, then a move will be done afterwards */
        if (!ts->fixed_reg) {
            if (ts->val_type == TEMP_VAL_REG)
                s->reg_to_temp[ts->reg] = -1;
            ts->val_type = TEMP_VAL_REG;
            ts->reg = reg;
            /* temp value is modified, so the value kept in memory is
               potentially not the same */
            ts->mem_coherent = 0; 
            s->reg_to_temp[reg] = arg;
        }
    oarg_end:
        new_args[i] = reg;
    }

    if (def->flags & TCG_OPF_BB_END)
        tcg_reg_alloc_bb_end(s);

    /* emit instruction */
    tcg_out_op(s, opc, new_args, const_args);
    
    /* move the outputs in the correct register if needed */
    for(i = 0; i < nb_oargs; i++) {
        ts = &s->temps[args[i]];
        reg = new_args[i];
        if (ts->fixed_reg && ts->reg != reg) {
            tcg_out_mov(s, ts->reg, reg);
        }
    }
}

#ifdef TCG_TARGET_STACK_GROWSUP
#define STACK_DIR(x) (-(x))
#else
#define STACK_DIR(x) (x)
#endif

static int tcg_reg_alloc_call(TCGContext *s, const TCGOpDef *def,
                              int opc, const TCGArg *args,
                              unsigned int dead_iargs)
{
    int nb_iargs, nb_oargs, flags, nb_regs, i, reg, nb_params;
    TCGArg arg, func_arg;
    TCGTemp *ts;
    tcg_target_long stack_offset, call_stack_size, func_addr;
    int const_func_arg, allocate_args;
    TCGRegSet allocated_regs;
    const TCGArgConstraint *arg_ct;

    arg = *args++;

    nb_oargs = arg >> 16;
    nb_iargs = arg & 0xffff;
    nb_params = nb_iargs - 1;

    flags = args[nb_oargs + nb_iargs];

    nb_regs = tcg_target_get_call_iarg_regs_count(flags);
    if (nb_regs > nb_params)
        nb_regs = nb_params;

    /* assign stack slots first */
    /* XXX: preallocate call stack */
    call_stack_size = (nb_params - nb_regs) * sizeof(tcg_target_long);
    call_stack_size = (call_stack_size + TCG_TARGET_STACK_ALIGN - 1) & 
        ~(TCG_TARGET_STACK_ALIGN - 1);
    allocate_args = (call_stack_size > TCG_STATIC_CALL_ARGS_SIZE);
    if (allocate_args) {
        tcg_out_addi(s, TCG_REG_CALL_STACK, -STACK_DIR(call_stack_size));
    }
    /* XXX: on some architectures it does not start at zero */
    stack_offset = 0;
    for(i = nb_regs; i < nb_params; i++) {
        arg = args[nb_oargs + i];
        ts = &s->temps[arg];
        if (ts->val_type == TEMP_VAL_REG) {
            tcg_out_st(s, ts->type, ts->reg, TCG_REG_CALL_STACK, stack_offset);
        } else if (ts->val_type == TEMP_VAL_MEM) {
            reg = tcg_reg_alloc(s, tcg_target_available_regs[ts->type], 
                                s->reserved_regs);
            /* XXX: not correct if reading values from the stack */
            tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
            tcg_out_st(s, ts->type, reg, TCG_REG_CALL_STACK, stack_offset);
        } else if (ts->val_type == TEMP_VAL_CONST) {
            reg = tcg_reg_alloc(s, tcg_target_available_regs[ts->type], 
                                s->reserved_regs);
            /* XXX: sign extend may be needed on some targets */
            tcg_out_movi(s, ts->type, reg, ts->val);
            tcg_out_st(s, ts->type, reg, TCG_REG_CALL_STACK, stack_offset);
        } else {
            tcg_abort();
        }
        /* XXX: not necessarily in the same order */
        stack_offset += STACK_DIR(sizeof(tcg_target_long));
    }
    
    /* assign input registers */
    tcg_regset_set(allocated_regs, s->reserved_regs);
    for(i = 0; i < nb_regs; i++) {
        arg = args[nb_oargs + i];
        ts = &s->temps[arg];
        reg = tcg_target_call_iarg_regs[i];
        tcg_reg_free(s, reg);
        if (ts->val_type == TEMP_VAL_REG) {
            if (ts->reg != reg) {
                tcg_out_mov(s, reg, ts->reg);
            }
        } else if (ts->val_type == TEMP_VAL_MEM) {
            tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
        } else if (ts->val_type == TEMP_VAL_CONST) {
            /* XXX: sign extend ? */
            tcg_out_movi(s, ts->type, reg, ts->val);
        } else {
            tcg_abort();
        }
        tcg_regset_set_reg(allocated_regs, reg);
    }
    
    /* assign function address */
    func_arg = args[nb_oargs + nb_iargs - 1];
    arg_ct = &def->args_ct[0];
    ts = &s->temps[func_arg];
    func_addr = ts->val;
    const_func_arg = 0;
    if (ts->val_type == TEMP_VAL_MEM) {
        reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
        tcg_out_ld(s, ts->type, reg, ts->mem_reg, ts->mem_offset);
        func_arg = reg;
    } else if (ts->val_type == TEMP_VAL_REG) {
        reg = ts->reg;
        if (!tcg_regset_test_reg(arg_ct->u.regs, reg)) {
            reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
            tcg_out_mov(s, reg, ts->reg);
        }
        func_arg = reg;
    } else if (ts->val_type == TEMP_VAL_CONST) {
        if (tcg_target_const_match(func_addr, arg_ct)) {
            const_func_arg = 1;
            func_arg = func_addr;
        } else {
            reg = tcg_reg_alloc(s, arg_ct->u.regs, allocated_regs);
            tcg_out_movi(s, ts->type, reg, func_addr);
            func_arg = reg;
        }
    } else {
        tcg_abort();
    }
    
    /* mark dead temporaries and free the associated registers */
    for(i = 0; i < nb_params; i++) {
        arg = args[nb_oargs + i];
        if (IS_DEAD_IARG(i)) {
            ts = &s->temps[arg];
            if (ts->val_type != TEMP_VAL_CONST && !ts->fixed_reg) {
                if (ts->val_type == TEMP_VAL_REG)
                    s->reg_to_temp[ts->reg] = -1;
                ts->val_type = TEMP_VAL_DEAD;
            }
        }
    }
    
    /* clobber call registers */
    for(reg = 0; reg < TCG_TARGET_NB_REGS; reg++) {
        if (tcg_regset_test_reg(tcg_target_call_clobber_regs, reg)) {
            tcg_reg_free(s, reg);
        }
    }
    
    /* store globals and free associated registers (we assume the call
       can modify any global. */
    for(i = 0; i < s->nb_globals; i++) {
        ts = &s->temps[i];
        if (!ts->fixed_reg) {
            if (ts->val_type == TEMP_VAL_REG) {
                tcg_reg_free(s, ts->reg);
            }
        }
    }

    tcg_out_op(s, opc, &func_arg, &const_func_arg);
    
    if (allocate_args) {
        tcg_out_addi(s, TCG_REG_CALL_STACK, STACK_DIR(call_stack_size));
    }

    /* assign output registers and emit moves if needed */
    for(i = 0; i < nb_oargs; i++) {
        arg = args[i];
        ts = &s->temps[arg];
        reg = tcg_target_call_oarg_regs[i];
        tcg_reg_free(s, reg);
        if (ts->fixed_reg) {
            if (ts->reg != reg) {
                tcg_out_mov(s, ts->reg, reg);
            }
        } else {
            if (ts->val_type == TEMP_VAL_REG)
                s->reg_to_temp[ts->reg] = -1;
            ts->val_type = TEMP_VAL_REG;
            ts->reg = reg;
            ts->mem_coherent = 0; 
            s->reg_to_temp[reg] = arg;
        }
    }
    
    return nb_iargs + nb_oargs + def->nb_cargs + 1;
}

#ifdef CONFIG_PROFILER

static int64_t dyngen_table_op_count[NB_OPS];

void dump_op_count(void)
{
    int i;
    FILE *f;
    f = fopen("/tmp/op1.log", "w");
    for(i = 0; i < INDEX_op_end; i++) {
        fprintf(f, "%s %" PRId64 "\n", tcg_op_defs[i].name, dyngen_table_op_count[i]);
    }
    fclose(f);
    f = fopen("/tmp/op2.log", "w");
    for(i = INDEX_op_end; i < NB_OPS; i++) {
        fprintf(f, "%s %" PRId64 "\n", tcg_op_defs[i].name, dyngen_table_op_count[i]);
    }
    fclose(f);
}
#endif


static inline int tcg_gen_code_common(TCGContext *s, uint8_t *gen_code_buf,
                                      long search_pc)
{
    int opc, op_index, macro_op_index;
    const TCGOpDef *def;
    unsigned int dead_iargs;
    const TCGArg *args;

#ifdef DEBUG_DISAS
    if (unlikely(loglevel & CPU_LOG_TB_OP)) {
        fprintf(logfile, "OP:\n");
        tcg_dump_ops(s, logfile);
        fprintf(logfile, "\n");
    }
#endif

    tcg_liveness_analysis(s);

#ifdef DEBUG_DISAS
    if (unlikely(loglevel & CPU_LOG_TB_OP_OPT)) {
        fprintf(logfile, "OP after la:\n");
        tcg_dump_ops(s, logfile);
        fprintf(logfile, "\n");
    }
#endif

    tcg_reg_alloc_start(s);

    s->code_buf = gen_code_buf;
    s->code_ptr = gen_code_buf;

    macro_op_index = -1;
    args = gen_opparam_buf;
    op_index = 0;

    for(;;) {
        opc = gen_opc_buf[op_index];
#ifdef CONFIG_PROFILER
        dyngen_table_op_count[opc]++;
#endif
        def = &tcg_op_defs[opc];
#if 0
        printf("%s: %d %d %d\n", def->name,
               def->nb_oargs, def->nb_iargs, def->nb_cargs);
        //        dump_regs(s);
#endif
        switch(opc) {
        case INDEX_op_mov_i32:
#if TCG_TARGET_REG_BITS == 64
        case INDEX_op_mov_i64:
#endif
            dead_iargs = s->op_dead_iargs[op_index];
            tcg_reg_alloc_mov(s, def, args, dead_iargs);
            break;
        case INDEX_op_nop:
        case INDEX_op_nop1:
        case INDEX_op_nop2:
        case INDEX_op_nop3:
            break;
        case INDEX_op_nopn:
            args += args[0];
            goto next;
        case INDEX_op_discard:
            {
                TCGTemp *ts;
                ts = &s->temps[args[0]];
                /* mark the temporary as dead */
                if (ts->val_type != TEMP_VAL_CONST && !ts->fixed_reg) {
                    if (ts->val_type == TEMP_VAL_REG)
                        s->reg_to_temp[ts->reg] = -1;
                    ts->val_type = TEMP_VAL_DEAD;
                }
            }
            break;
        case INDEX_op_macro_goto:
            macro_op_index = op_index; /* only used for exceptions */
            op_index = args[0] - 1;
            args = gen_opparam_buf + args[1];
            goto next;
        case INDEX_op_macro_end:
            macro_op_index = -1; /* only used for exceptions */
            op_index = args[0] - 1;
            args = gen_opparam_buf + args[1];
            goto next;
        case INDEX_op_macro_start:
            /* must never happen here */
            tcg_abort();
        case INDEX_op_set_label:
            tcg_reg_alloc_bb_end(s);
            tcg_out_label(s, args[0], (long)s->code_ptr);
            break;
        case INDEX_op_call:
            dead_iargs = s->op_dead_iargs[op_index];
            args += tcg_reg_alloc_call(s, def, opc, args, dead_iargs);
            goto next;
        case INDEX_op_end:
            goto the_end;

#ifdef CONFIG_DYNGEN_OP
        case 0 ... INDEX_op_end - 1:
            /* legacy dyngen ops */
#ifdef CONFIG_PROFILER
            {
                extern int64_t dyngen_old_op_count;
                dyngen_old_op_count++;
            }
#endif
            tcg_reg_alloc_bb_end(s);
            if (search_pc >= 0) {
                s->code_ptr += def->copy_size;
                args += def->nb_args;
            } else {
                args = dyngen_op(s, opc, args);
            }
            goto next;
#endif
        default:
            /* Note: in order to speed up the code, it would be much
               faster to have specialized register allocator functions for
               some common argument patterns */
            dead_iargs = s->op_dead_iargs[op_index];
            tcg_reg_alloc_op(s, def, opc, args, dead_iargs);
            break;
        }
        args += def->nb_args;
    next: ;
        if (search_pc >= 0 && search_pc < s->code_ptr - gen_code_buf) {
            if (macro_op_index >= 0)
                return macro_op_index;
            else
                return op_index;
        }
        op_index++;
#ifndef NDEBUG
        check_regs(s);
#endif
    }
 the_end:
    return -1;
}

int dyngen_code(TCGContext *s, uint8_t *gen_code_buf)
{
#ifdef CONFIG_PROFILER
    {
        extern int64_t dyngen_op_count;
        extern int dyngen_op_count_max;
        int n;
        n = (gen_opc_ptr - gen_opc_buf);
        dyngen_op_count += n;
        if (n > dyngen_op_count_max)
            dyngen_op_count_max = n;
    }
#endif

    tcg_gen_code_common(s, gen_code_buf, -1);

    /* flush instruction cache */
    flush_icache_range((unsigned long)gen_code_buf, 
                       (unsigned long)s->code_ptr);
    return s->code_ptr -  gen_code_buf;
}

/* Return the index of the micro operation such as the pc after is <
   offset bytes from the start of the TB.  The contents of gen_code_buf must
   not be changed, though writing the same values is ok.
   Return -1 if not found. */
int dyngen_code_search_pc(TCGContext *s, uint8_t *gen_code_buf, long offset)
{
    return tcg_gen_code_common(s, gen_code_buf, offset);
}
