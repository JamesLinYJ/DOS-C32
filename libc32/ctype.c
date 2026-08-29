// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 invariant ASCII character classification
 *
 * Compatibility contract: command syntax uses ASCII; DOS country/code-page tables
 *                 remain authoritative for INT 21h international casing
 * Safety changes: total predicates for every int value; no table indexing
 *                 with a negative signed char
 */
#include "ctype.h"

bool ascii_isascii(int character)
{
	return character >= 0 && character <= 0x7f;
}

bool ascii_isupper(int character)
{
	return character >= 'A' && character <= 'Z';
}

bool ascii_islower(int character)
{
	return character >= 'a' && character <= 'z';
}

bool ascii_isalpha(int character)
{
	return ascii_isupper(character) || ascii_islower(character);
}

bool ascii_isdigit(int character)
{
	return character >= '0' && character <= '9';
}

bool ascii_isxdigit(int character)
{
	return ascii_isdigit(character) ||
	       (character >= 'A' && character <= 'F') ||
	       (character >= 'a' && character <= 'f');
}

bool ascii_isalnum(int character)
{
	return ascii_isalpha(character) || ascii_isdigit(character);
}

bool ascii_isblank(int character)
{
	return character == ' ' || character == '\t';
}

bool ascii_iscntrl(int character)
{
	return (character >= 0 && character < ' ') || character == 0x7f;
}

bool ascii_isprint(int character)
{
	return character >= ' ' && character <= '~';
}

bool ascii_isgraph(int character)
{
	return character > ' ' && character <= '~';
}

bool ascii_ispunct(int character)
{
	return ascii_isgraph(character) && !ascii_isalnum(character);
}
