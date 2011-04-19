/*
 * Debugger ARM specific functions
 *
 * Copyright 2000-2003 Marcus Meissner
 *                2004 Eric Pouech
 *                2010 André Hentschel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "debugger.h"

#if defined(__arm__)

static unsigned be_arm_get_addr(HANDLE hThread, const CONTEXT* ctx,
                                enum be_cpu_addr bca, ADDRESS64* addr)
{
    switch (bca)
    {
    case be_cpu_addr_pc:
        return be_cpu_build_addr(hThread, ctx, addr, 0, ctx->Pc);
    case be_cpu_addr_stack:
        return be_cpu_build_addr(hThread, ctx, addr, 0, ctx->Sp);
    case be_cpu_addr_frame:
        return be_cpu_build_addr(hThread, ctx, addr, 0, ctx->Fp);
    }
    return FALSE;
}

static unsigned be_arm_get_register_info(int regno, enum be_cpu_addr* kind)
{
    switch (regno)
    {
    case CV_ARM_PC:  *kind = be_cpu_addr_pc; return TRUE;
    case CV_ARM_R0 + 11: *kind = be_cpu_addr_frame; return TRUE;
    case CV_ARM_SP:  *kind = be_cpu_addr_stack; return TRUE;
    }
    return FALSE;
}

static void be_arm_single_step(CONTEXT* ctx, unsigned enable)
{
    dbg_printf("be_arm_single_step: not done\n");
}

static void be_arm_print_context(HANDLE hThread, const CONTEXT* ctx, int all_regs)
{
    dbg_printf("Register dump:\n");
    dbg_printf(" Pc:%04x Sp:%04x Lr:%04x Cpsr:%04x\n",
               ctx->Pc, ctx->Sp, ctx->Lr, ctx->Cpsr);
    dbg_printf(" r0:%04x r1:%04x r2:%04x r3:%04x\n",
               ctx->R0, ctx->R1, ctx->R2, ctx->R3);
    dbg_printf(" r4:%04x r5:%04x  r6:%04x  r7:%04x r8:%04x\n",
               ctx->R4, ctx->R5, ctx->R6, ctx->R7, ctx->R8 );
    dbg_printf(" r9:%04x r10:%04x Fp:%04x Ip:%04x\n",
               ctx->R9, ctx->R10, ctx->Fp, ctx->Ip );

    if (all_regs) dbg_printf( "Floating point ARM dump not implemented\n" );
}

static void be_arm_print_segment_info(HANDLE hThread, const CONTEXT* ctx)
{
}

static struct dbg_internal_var be_arm_ctx[] =
{
    {0,                 NULL,           0,                                      dbg_itype_none}
};

static unsigned be_arm_is_step_over_insn(const void* insn)
{
    dbg_printf("be_arm_is_step_over_insn: not done\n");
    return FALSE;
}

static unsigned be_arm_is_function_return(const void* insn)
{
    dbg_printf("be_arm_is_function_return: not done\n");
    return FALSE;
}

static unsigned be_arm_is_break_insn(const void* insn)
{
    dbg_printf("be_arm_is_break_insn: not done\n");
    return FALSE;
}

static unsigned be_arm_is_func_call(const void* insn, ADDRESS64* callee)
{
    return FALSE;
}

static unsigned be_arm_is_jump(const void* insn, ADDRESS64* jumpee)
{
    return FALSE;
}

static void be_arm_disasm_one_insn(ADDRESS64* addr, int display)
{
    dbg_printf("Disasm NIY\n");
}

static unsigned be_arm_insert_Xpoint(HANDLE hProcess, const struct be_process_io* pio,
                                     CONTEXT* ctx, enum be_xpoint_type type,
                                     void* addr, unsigned long* val, unsigned size)
{
    SIZE_T              sz;

    switch (type)
    {
    case be_xpoint_break:
        if (!size) return 0;
        if (!pio->read(hProcess, addr, val, 4, &sz) || sz != 4) return 0;
    default:
        dbg_printf("Unknown/unsupported bp type %c\n", type);
        return 0;
    }
    return 1;
}

static unsigned be_arm_remove_Xpoint(HANDLE hProcess, const struct be_process_io* pio,
                                     CONTEXT* ctx, enum be_xpoint_type type,
                                     void* addr, unsigned long val, unsigned size)
{
    SIZE_T              sz;

    switch (type)
    {
    case be_xpoint_break:
        if (!size) return 0;
        if (!pio->write(hProcess, addr, &val, 4, &sz) || sz == 4) return 0;
        break;
    default:
        dbg_printf("Unknown/unsupported bp type %c\n", type);
        return 0;
    }
    return 1;
}

static unsigned be_arm_is_watchpoint_set(const CONTEXT* ctx, unsigned idx)
{
    dbg_printf("be_arm_is_watchpoint_set: not done\n");
    return FALSE;
}

static void be_arm_clear_watchpoint(CONTEXT* ctx, unsigned idx)
{
    dbg_printf("be_arm_clear_watchpoint: not done\n");
}

static int be_arm_adjust_pc_for_break(CONTEXT* ctx, BOOL way)
{
    if (way)
    {
        ctx->Pc--;
        return -1;
    }
    ctx->Pc++;
    return 1;
}

static int be_arm_fetch_integer(const struct dbg_lvalue* lvalue, unsigned size,
                                unsigned ext_sign, LONGLONG* ret)
{
    dbg_printf("be_arm_fetch_integer: not done\n");
    return FALSE;
}

static int be_arm_fetch_float(const struct dbg_lvalue* lvalue, unsigned size,
                              long double* ret)
{
    dbg_printf("be_arm_fetch_float: not done\n");
    return FALSE;
}

struct backend_cpu be_arm =
{
    IMAGE_FILE_MACHINE_ARM,
    4,
    be_cpu_linearize,
    be_cpu_build_addr,
    be_arm_get_addr,
    be_arm_get_register_info,
    be_arm_single_step,
    be_arm_print_context,
    be_arm_print_segment_info,
    be_arm_ctx,
    be_arm_is_step_over_insn,
    be_arm_is_function_return,
    be_arm_is_break_insn,
    be_arm_is_func_call,
    be_arm_is_jump,
    be_arm_disasm_one_insn,
    be_arm_insert_Xpoint,
    be_arm_remove_Xpoint,
    be_arm_is_watchpoint_set,
    be_arm_clear_watchpoint,
    be_arm_adjust_pc_for_break,
    be_arm_fetch_integer,
    be_arm_fetch_float,
};
#endif