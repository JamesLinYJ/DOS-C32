// SPDX-License-Identifier: GPL-2.0-only
/* Stack-corruption termination for the freestanding GCC runtime. */
#include "compiler.h"
#include "console.h"
#include "dos_ui.h"
#include "io.h"
#include "types.h"

/* entry.S seeds both before the first stack-protected C frame is entered. */
uint32_t __stack_chk_guard;
uint32_t __stack_chk_guard_source_mask;

void __stack_chk_fail(void) __noreturn;
void __stack_chk_fail_local(void) __noreturn;

void __stack_chk_fail(void)
{
	struct dos_ui_text text =
		dos_ui_text_get(DOS_UI_FATAL_STACK_CORRUPTION);

	console_write(text.data, text.length);
	cpu_disable_interrupts();
	for (;;)
		cpu_halt();
}

void __stack_chk_fail_local(void)
{
	__stack_chk_fail();
}
