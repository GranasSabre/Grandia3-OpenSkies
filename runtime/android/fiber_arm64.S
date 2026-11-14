// Android build: fiber context switch for ARM64 (Win32 SwitchToFiber replacement, win32_shim.cpp).
// Saves the callee-saved registers on the current stack, stores sp, loads the other fiber's sp and
// restores its registers. The kernel's savestates resume threads at the MIPS level, so nothing else
// depends on the layout of these stacks.

    .text
    .align 4

// void g3_ctx_switch(void** save_sp, void* load_sp)
    .global g3_ctx_switch
    .type g3_ctx_switch, %function
g3_ctx_switch:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret
    .size g3_ctx_switch, .-g3_ctx_switch

// first entry of a new fiber: g3_fiber_init stored the fiber in x19 and this address in x30
    .global g3_fiber_trampoline
    .type g3_fiber_trampoline, %function
g3_fiber_trampoline:
    mov x0, x19
    bl g3_fiber_main
    brk #0
    .size g3_fiber_trampoline, .-g3_fiber_trampoline

    .section .note.GNU-stack, "", %progbits
