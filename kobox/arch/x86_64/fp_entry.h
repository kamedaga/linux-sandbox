/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_FP_ENTRY_H
#define KOBOX_FP_ENTRY_H

/* Integer-only leaf primitives may run with the caller's FP bank live.
 * Prevent implicit vector code even in compiler prologues/epilogues; actual
 * generated leaf code is also audited by the notification contract test. */
#define KOBOX_GPR_LEAF __attribute__((target("no-sse,no-sse2,no-avx,no-mmx")))

/* A preserving entry must save before any C prologue can clobber vectors.
 * This is always active, including POSIX without KOBOX_BOOT_RUNTIME. The
 * stack-local bank follows the task through nested callbacks and migration.
 * SysV entry RSP is 8 mod 16; 520 bytes aligns both FXSAVE and the C call. */
static const unsigned char kobox_fp_entry_initial[512]
    __attribute__((aligned(16), used)) = {
    [0] = 0x7f, [1] = 0x03,
    [24] = 0x80, [25] = 0x1f,
};

#define KOBOX_FP_ENTRY_BODY(callee) \
    __asm__ volatile( \
        "sub $520, %rsp\n" \
        "fxsaveq (%rsp)\n" \
        "fxrstorq kobox_fp_entry_initial(%rip)\n" \
        "call " #callee "\n" \
        "fxrstorq (%rsp)\n" \
        "add $520, %rsp\n" \
        "ret\n")

#endif
