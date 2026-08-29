/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable host-test entry point for macOS and freestanding x86 builds. */
#ifndef DOSC32_TEST_ENTRY_H
#define DOSC32_TEST_ENTRY_H

#if defined(__APPLE__)

#define DOSC32_TEST_ENTRY(test_function)                                    \
	int main(void)                                                       \
	{                                                                    \
		return test_function();                                        \
	}

#elif defined(__i386__)

#define DOSC32_TEST_ENTRY(test_function)                                    \
	void _start(void) __attribute__((noreturn, force_align_arg_pointer)); \
	void _start(void)                                                     \
	{                                                                    \
		register unsigned int status __asm__("ebx") =                  \
			(unsigned int)test_function();                            \
		register unsigned int number __asm__("eax") = 1u;              \
		__asm__ volatile("int $0x80"                                    \
				 :                                                   \
				 : "r"(number), "r"(status)                         \
				 : "memory");                                        \
		__builtin_unreachable();                                        \
	}

#elif defined(__x86_64__)

#define DOSC32_TEST_ENTRY(test_function)                                    \
	void _start(void) __attribute__((noreturn, force_align_arg_pointer)); \
	void _start(void)                                                     \
	{                                                                    \
		register unsigned long status __asm__("rdi") =                  \
			(unsigned long)(unsigned int)test_function();              \
		register unsigned long number __asm__("rax") = 60u;             \
		__asm__ volatile("syscall"                                      \
				 :                                                   \
				 : "r"(number), "r"(status)                         \
				 : "rcx", "r11", "memory");                        \
		__builtin_unreachable();                                        \
	}

#else
#error Unsupported host-test architecture
#endif

#endif
