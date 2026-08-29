// SPDX-License-Identifier: GPL-2.0-only
/*
 * High-memory i386 descriptor tables.
 *
 * The descriptor construction follows the Intel i386 architectural format.
 * One typed TSS owner and a single assembly-entry ABI keep policy in C.
 * DOS/VM86-visible behavior remains owned by the machine layer.
 */
#include "x86_runtime.h"

#include "x86_legacy_irq.h"
#include "x86_paging.h"
#include "x86_user.h"
#include "x86_vm86.h"

#define X86_GDT_ENTRY_COUNT 6u
#define X86_IDT_ENTRY_COUNT 256u
#define X86_KERNEL_CODE_SELECTOR 0x08u
#define X86_KERNEL_DATA_SELECTOR 0x10u
#define X86_TSS_SELECTOR 0x18u
#define X86_USER_CODE_SELECTOR 0x23u
#define X86_USER_DATA_SELECTOR 0x2bu

#define X86_DESCRIPTOR_PRESENT 0x80u
#define X86_DESCRIPTOR_TSS_AVAILABLE_32 0x09u
#define X86_INTERRUPT_GATE_32 0x0eu

struct x86_descriptor {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t base_middle;
	uint8_t access;
	uint8_t limit_high_and_flags;
	uint8_t base_high;
} __packed;

struct x86_table_pointer {
	uint16_t limit;
	uint32_t base;
} __packed;

struct x86_task_state_segment {
	uint32_t previous_task;
	uint32_t esp0;
	uint32_t ss0;
	uint32_t esp1;
	uint32_t ss1;
	uint32_t esp2;
	uint32_t ss2;
	uint32_t cr3;
	uint32_t instruction_pointer;
	uint32_t flags;
	uint32_t eax;
	uint32_t ecx;
	uint32_t edx;
	uint32_t ebx;
	uint32_t esp;
	uint32_t ebp;
	uint32_t esi;
	uint32_t edi;
	uint32_t es;
	uint32_t cs;
	uint32_t ss;
	uint32_t ds;
	uint32_t fs;
	uint32_t gs;
	uint32_t ldt;
	uint16_t trap;
	uint16_t io_map_base;
} __packed;

struct x86_interrupt_gate {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t reserved;
	uint8_t access;
	uint16_t offset_high;
} __packed;

extern const uint32_t x86_exception_stub_table[X86_EXCEPTION_COUNT];
extern const uint32_t x86_irq_stub_table[X86_LEGACY_IRQ_COUNT];
extern void x86_user_syscall_entry(void);
extern uint8_t __kernel_stack_top[];
extern uint8_t __kernel_stack_floor[];

void x86_load_runtime_tables(const struct x86_table_pointer *gdt_pointer,
			     const struct x86_table_pointer *idt_pointer);

static struct x86_descriptor runtime_gdt[X86_GDT_ENTRY_COUNT]
	__attribute__((aligned(8)));
static struct x86_task_state_segment runtime_tss __attribute__((aligned(16)));
static struct x86_interrupt_gate runtime_idt[X86_IDT_ENTRY_COUNT]
	__attribute__((aligned(8)));
#if CONFIG_BOOT_SELFTESTS
static bool breakpoint_expected;
static bool breakpoint_observed;
#endif

static_assert_expression(sizeof(struct x86_descriptor) == 8u,
			 "i386 GDT descriptor must be eight bytes");
static_assert_expression(sizeof(struct x86_interrupt_gate) == 8u,
			 "i386 IDT gate must be eight bytes");
static_assert_expression(sizeof(struct x86_table_pointer) == 6u,
			 "i386 table pointer must be six bytes");
static_assert_expression(sizeof(struct x86_task_state_segment) == 104u,
			 "i386 TSS must end at the I/O-map base word");

static struct x86_descriptor segment_descriptor(uint8_t access,
					 uint8_t flags)
{
	return (struct x86_descriptor){
		.limit_low = 0xffffu,
		.base_low = 0u,
		.base_middle = 0u,
		.access = access,
		.limit_high_and_flags = (uint8_t)(0x0fu | flags),
		.base_high = 0u,
	};
}

static struct x86_descriptor tss_descriptor(uintptr_t base, uint32_t limit)
{
	return (struct x86_descriptor){
		.limit_low = (uint16_t)limit,
		.base_low = (uint16_t)base,
		.base_middle = (uint8_t)(base >> 16u),
		.access = X86_DESCRIPTOR_PRESENT |
			  X86_DESCRIPTOR_TSS_AVAILABLE_32,
		.limit_high_and_flags = (uint8_t)((limit >> 16u) & 0x0fu),
		.base_high = (uint8_t)(base >> 24u),
	};
}

static struct x86_interrupt_gate interrupt_gate_with_dpl(uintptr_t entry,
						  uint8_t dpl)
{
	return (struct x86_interrupt_gate){
		.offset_low = (uint16_t)entry,
		.selector = X86_KERNEL_CODE_SELECTOR,
		.reserved = 0u,
		.access = X86_DESCRIPTOR_PRESENT | X86_INTERRUPT_GATE_32 |
			  (uint8_t)(dpl << 5u),
		.offset_high = (uint16_t)(entry >> 16u),
	};
}

static struct x86_interrupt_gate interrupt_gate(uintptr_t entry)
{
	return interrupt_gate_with_dpl(entry, 0u);
}

void x86_runtime_initialize(void)
{
	struct x86_table_pointer gdt_pointer;
	struct x86_table_pointer idt_pointer;
	uint32_t vector;

	if (!x86_paging_is_enabled())
		for (;;)
			__asm__ volatile("cli; hlt");

	runtime_tss = (struct x86_task_state_segment){0};
	runtime_tss.esp0 = (uint32_t)(uintptr_t)&__kernel_stack_top[0];
	runtime_tss.ss0 = X86_KERNEL_DATA_SELECTOR;
	/* A base at sizeof(TSS) means no byte of an I/O bitmap is present. */
	runtime_tss.io_map_base = (uint16_t)sizeof(runtime_tss);

	runtime_gdt[0] = (struct x86_descriptor){0};
	runtime_gdt[1] = segment_descriptor(0x9au, 0xc0u);
	runtime_gdt[2] = segment_descriptor(0x92u, 0xc0u);
	runtime_gdt[3] = tss_descriptor(
		(uintptr_t)&runtime_tss,
		(uint32_t)sizeof(runtime_tss) - 1u);
	runtime_gdt[4] = segment_descriptor(0xfau, 0xc0u);
	runtime_gdt[5] = segment_descriptor(0xf2u, 0xc0u);

	for (vector = 0u; vector < X86_EXCEPTION_COUNT; ++vector)
		runtime_idt[vector] = interrupt_gate(
			(uintptr_t)x86_exception_stub_table[vector]);
	for (vector = 0u; vector < X86_LEGACY_IRQ_COUNT; ++vector)
		runtime_idt[X86_LEGACY_IRQ_VECTOR_BASE + vector] = interrupt_gate(
			(uintptr_t)x86_irq_stub_table[vector]);
	runtime_idt[0x30u] = interrupt_gate_with_dpl(
		(uintptr_t)&x86_user_syscall_entry, 3u);

	gdt_pointer = (struct x86_table_pointer){
		.limit = (uint16_t)sizeof(runtime_gdt) - 1u,
		.base = (uint32_t)(uintptr_t)&runtime_gdt[0],
	};
	idt_pointer = (struct x86_table_pointer){
		.limit = (uint16_t)sizeof(runtime_idt) - 1u,
		.base = (uint32_t)(uintptr_t)&runtime_idt[0],
	};
	x86_load_runtime_tables(&gdt_pointer, &idt_pointer);
}

bool x86_runtime_set_kernel_stack(uint32_t stack_pointer)
{
	uint32_t floor = (uint32_t)(uintptr_t)&__kernel_stack_floor[0];
	uint32_t top = (uint32_t)(uintptr_t)&__kernel_stack_top[0];

	if ((stack_pointer & 3u) != 0u || stack_pointer < floor ||
	    stack_pointer > top)
		return false;
	runtime_tss.esp0 = stack_pointer;
	return true;
}

#if CONFIG_BOOT_SELFTESTS
bool x86_runtime_breakpoint_self_test(void)
{
	breakpoint_observed = false;
	breakpoint_expected = true;
	__asm__ volatile("int3" : : : "memory");
	breakpoint_expected = false;
	return breakpoint_observed;
}
#endif

void x86_trap_dispatch(struct x86_trap_frame *frame)
{
	enum x86_legacy_irq_status irq_status;
	enum x86_vm86_interrupt_delivery_status delivery_status;

	if (frame != NULL && frame->error_code == 0u &&
	    frame->vector >= X86_LEGACY_IRQ_VECTOR_BASE &&
	    frame->vector < X86_LEGACY_IRQ_VECTOR_BASE + X86_LEGACY_IRQ_COUNT) {
		/* The selected controller classifies the delivery, registered native
		 * actions run, and the controller applies its exact EOI rule.  Guest
		 * state is reached only by an action's explicit typed producer. */
		irq_status = x86_legacy_irq_dispatch_vector(frame->vector);
		if (irq_status != X86_LEGACY_IRQ_OK &&
		    irq_status != X86_LEGACY_IRQ_SPURIOUS &&
		    irq_status != X86_LEGACY_IRQ_UNHANDLED)
			goto fail_closed;
		delivery_status =
			x86_vm86_deliver_pending_interrupt(frame);
		switch (delivery_status) {
		case X86_VM86_INTERRUPT_INACTIVE:
		case X86_VM86_INTERRUPT_DEFERRED:
		case X86_VM86_INTERRUPT_NONE:
		case X86_VM86_INTERRUPT_DELIVERED:
		case X86_VM86_INTERRUPT_SESSION_FAULT:
			return;
		case X86_VM86_INTERRUPT_SYSTEM_FAULT:
		default:
			goto fail_closed;
		}
	}
	if (x86_vm86_handle_trap(frame))
		return;
	if (x86_user_handle_trap(frame))
		return;
#if CONFIG_BOOT_SELFTESTS
	if (frame != NULL &&
	    frame->vector == (uint32_t)X86_EXCEPTION_BREAKPOINT &&
	    frame->error_code == 0u && breakpoint_expected) {
		breakpoint_observed = true;
		return;
	}
#endif

fail_closed:
	/* Frames unclaimed by an execution-domain owner fail closed. */
	for (;;)
		__asm__ volatile("cli; hlt");
}
