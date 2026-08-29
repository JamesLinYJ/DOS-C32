// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native console interrupt wait
 *
 * STI, HLT and CLI ordering follows the architectural interrupt contract.
 * Contract: process context enters with IF clear after deciding that the
 *           focused console FIFO is empty; return also has IF clear
 * Safety changes: STI's interrupt shadow closes the check/sleep race; this
 *                 hook is never installed on an IRQ receive path
 */
#include "keyboard.h"

void keyboard_console_x86_wait(void *context)
{
	(void)context;
#if defined(DOSC32_HOST_TEST)
	__asm__ volatile("" ::: "memory");
#elif defined(__i386__) || defined(__x86_64__)
	__asm__ volatile("sti\n\thlt\n\tcli" ::: "memory", "cc");
#else
#error keyboard_console_x86_wait requires an x86 supervisor
#endif
}
