/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed i386 exception entry ABI shared with kernel/x86_runtime.c. */
.set X86_KERNEL_CODE_SELECTOR, 0x08
.set X86_KERNEL_DATA_SELECTOR, 0x10
.set X86_TSS_SELECTOR, 0x18
.set X86_USER_CODE_SELECTOR, 0x23
.set X86_USER_DATA_SELECTOR, 0x2b

.section .text.x86_traps, "ax", @progbits
.code32
.extern x86_trap_dispatch
.extern x86_runtime_set_kernel_stack
.extern x86_vm86_saved_kernel_stack

.macro X86_EXCEPTION_NO_ERROR vector
.global x86_exception_\vector
x86_exception_\vector:
    pushl $0
    pushl $\vector
    jmp x86_exception_common
.endm

.macro X86_EXCEPTION_WITH_ERROR vector
.global x86_exception_\vector
x86_exception_\vector:
    pushl $\vector
    jmp x86_exception_common
.endm

X86_EXCEPTION_NO_ERROR 0
X86_EXCEPTION_NO_ERROR 1
X86_EXCEPTION_NO_ERROR 2
X86_EXCEPTION_NO_ERROR 3
X86_EXCEPTION_NO_ERROR 4
X86_EXCEPTION_NO_ERROR 5
X86_EXCEPTION_NO_ERROR 6
X86_EXCEPTION_NO_ERROR 7
X86_EXCEPTION_WITH_ERROR 8
X86_EXCEPTION_NO_ERROR 9
X86_EXCEPTION_WITH_ERROR 10
X86_EXCEPTION_WITH_ERROR 11
X86_EXCEPTION_WITH_ERROR 12
X86_EXCEPTION_WITH_ERROR 13
X86_EXCEPTION_WITH_ERROR 14
X86_EXCEPTION_NO_ERROR 15
X86_EXCEPTION_NO_ERROR 16
X86_EXCEPTION_WITH_ERROR 17
X86_EXCEPTION_NO_ERROR 18
X86_EXCEPTION_NO_ERROR 19
X86_EXCEPTION_NO_ERROR 20
X86_EXCEPTION_NO_ERROR 21
X86_EXCEPTION_NO_ERROR 22
X86_EXCEPTION_NO_ERROR 23
X86_EXCEPTION_NO_ERROR 24
X86_EXCEPTION_NO_ERROR 25
X86_EXCEPTION_NO_ERROR 26
X86_EXCEPTION_NO_ERROR 27
X86_EXCEPTION_NO_ERROR 28
X86_EXCEPTION_NO_ERROR 29
X86_EXCEPTION_NO_ERROR 30
X86_EXCEPTION_NO_ERROR 31

/* The remapped 8259 IRQs never push CPU error codes. */
X86_EXCEPTION_NO_ERROR 32
X86_EXCEPTION_NO_ERROR 33
X86_EXCEPTION_NO_ERROR 34
X86_EXCEPTION_NO_ERROR 35
X86_EXCEPTION_NO_ERROR 36
X86_EXCEPTION_NO_ERROR 37
X86_EXCEPTION_NO_ERROR 38
X86_EXCEPTION_NO_ERROR 39
X86_EXCEPTION_NO_ERROR 40
X86_EXCEPTION_NO_ERROR 41
X86_EXCEPTION_NO_ERROR 42
X86_EXCEPTION_NO_ERROR 43
X86_EXCEPTION_NO_ERROR 44
X86_EXCEPTION_NO_ERROR 45
X86_EXCEPTION_NO_ERROR 46
X86_EXCEPTION_NO_ERROR 47

.global x86_user_syscall_entry
x86_user_syscall_entry:
    pushl $0
    pushl $0x30
    jmp x86_exception_common

x86_exception_common:
    cld
    pushal

    movw %ds, %ax
    movzwl %ax, %eax
    pushl %eax
    movw %es, %ax
    movzwl %ax, %eax
    pushl %eax
    movw %fs, %ax
    movzwl %ax, %eax
    pushl %eax
    movw %gs, %ax
    movzwl %ax, %eax
    pushl %eax

    movw $X86_KERNEL_DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl %esp
    call x86_trap_dispatch
    addl $4, %esp

    popl %eax
    movw %ax, %gs
    popl %eax
    movw %ax, %fs
    popl %eax
    movw %ax, %es
    popl %eax
    movw %ax, %ds
    popal
    addl $8, %esp
    iret

.global x86_load_runtime_tables
x86_load_runtime_tables:
    movl 4(%esp), %eax
    lgdt (%eax)
    ljmpl $X86_KERNEL_CODE_SELECTOR, $1f
1:
    movw $X86_KERNEL_DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    movw $X86_TSS_SELECTOR, %ax
    ltr %ax
    movl 8(%esp), %eax
    lidt (%eax)
    ret

/* bool x86_vm86_enter(struct dos_cpu_state *state) */
.global x86_vm86_enter
x86_vm86_enter:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, x86_vm86_saved_kernel_stack
    pushl %esp
    call x86_runtime_set_kernel_stack
    addl $4, %esp
    testl %eax, %eax
    jz x86_vm86_enter_failed

    movl 20(%esp), %ebp

    /* Hardware VM86 IRET frame, in reverse pop order. */
    movzwl 50(%ebp), %eax
    pushl %eax
    movzwl 48(%ebp), %eax
    pushl %eax
    movzwl 44(%ebp), %eax
    pushl %eax
    movzwl 46(%ebp), %eax
    pushl %eax
    movzwl 42(%ebp), %eax
    pushl %eax
    pushl 28(%ebp)
    movl 36(%ebp), %eax
    /* Force physical IF independently of the saved guest IF.  The C owner
     * keeps logical IF separately; never grant IOPL or retain NT. */
    andl $0xffff8fff, %eax
    orl $0x00020202, %eax
    pushl %eax
    movzwl 40(%ebp), %eax
    pushl %eax
    pushl 32(%ebp)

    movl 4(%ebp), %ebx
    movl 8(%ebp), %ecx
    movl 12(%ebp), %edx
    movl 16(%ebp), %esi
    movl 20(%ebp), %edi
    movl 0(%ebp), %eax
    movl 24(%ebp), %ebp
    iret

.global x86_vm86_kernel_return
x86_vm86_kernel_return:
    movl x86_vm86_saved_kernel_stack, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    movl $1, %eax
    ret

x86_vm86_enter_failed:
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    xorl %eax, %eax
    ret

/* bool x86_user_enter(uint32_t entry_point, uint32_t stack_top) */
.global x86_user_enter
x86_user_enter:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, x86_user_saved_kernel_stack
    pushl %esp
    call x86_runtime_set_kernel_stack
    addl $4, %esp
    testl %eax, %eax
    jz x86_user_enter_failed

    movl 20(%esp), %ecx
    movl 24(%esp), %edx
    movw $X86_USER_DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    pushl $X86_USER_DATA_SELECTOR
    pushl %edx
    /* Ring-3 native programs run with physical interrupts enabled.  The TSS
     * still denies all direct I/O, and interrupt gates enter Ring 0 with IF
     * clear; blocking kernel services explicitly wait and return with the
     * caller's interrupt state intact. */
    pushl $0x00000202
    pushl $X86_USER_CODE_SELECTOR
    pushl %ecx
    iret

.global x86_user_kernel_return
x86_user_kernel_return:
    movw $X86_KERNEL_DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movl x86_user_saved_kernel_stack, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    movl $1, %eax
    ret

x86_user_enter_failed:
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    xorl %eax, %eax
    ret

.section .bss.x86_vm86, "aw", @nobits
.align 4
.global x86_vm86_saved_kernel_stack
x86_vm86_saved_kernel_stack:
    .long 0

.global x86_user_saved_kernel_stack
x86_user_saved_kernel_stack:
    .long 0

.section .rodata.x86_traps, "a", @progbits
.align 4
.global x86_exception_stub_table
x86_exception_stub_table:
    .long x86_exception_0, x86_exception_1, x86_exception_2
    .long x86_exception_3, x86_exception_4, x86_exception_5
    .long x86_exception_6, x86_exception_7, x86_exception_8
    .long x86_exception_9, x86_exception_10, x86_exception_11
    .long x86_exception_12, x86_exception_13, x86_exception_14
    .long x86_exception_15, x86_exception_16, x86_exception_17
    .long x86_exception_18, x86_exception_19, x86_exception_20
    .long x86_exception_21, x86_exception_22, x86_exception_23
    .long x86_exception_24, x86_exception_25, x86_exception_26
    .long x86_exception_27, x86_exception_28, x86_exception_29
    .long x86_exception_30, x86_exception_31

.align 4
.global x86_irq_stub_table
x86_irq_stub_table:
    .long x86_exception_32, x86_exception_33, x86_exception_34
    .long x86_exception_35, x86_exception_36, x86_exception_37
    .long x86_exception_38, x86_exception_39, x86_exception_40
    .long x86_exception_41, x86_exception_42, x86_exception_43
    .long x86_exception_44, x86_exception_45, x86_exception_46
    .long x86_exception_47

.section .note.GNU-stack, "", @progbits
