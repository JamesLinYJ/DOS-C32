/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_ATA_TEST_IO_H
#define DOSC32_ATA_TEST_IO_H

#include "types.h"

uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t value);
uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t value);

#endif
