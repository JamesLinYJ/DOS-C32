/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_CTYPE_H
#define DOSC32_CTYPE_H

#include "types.h"

/*
 * These predicates describe the invariant 7-bit ASCII command language.
 * Country/code-page-aware DOS upper-casing is a separate DOS API service.
 */
bool ascii_isascii(int character);
bool ascii_isalnum(int character);
bool ascii_isalpha(int character);
bool ascii_isblank(int character);
bool ascii_iscntrl(int character);
bool ascii_isdigit(int character);
bool ascii_isgraph(int character);
bool ascii_islower(int character);
bool ascii_isprint(int character);
bool ascii_ispunct(int character);
bool ascii_isspace(char character);
bool ascii_isupper(int character);
bool ascii_isxdigit(int character);
char ascii_tolower(char character);
char ascii_toupper(char character);

#endif
