/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSC32_CONFIG_ATKBD_H
#define DOSC32_CONFIG_ATKBD_H

/* PS/2 protocol-shape ceilings, not detected keyboard or topology facts. */
#define CONFIG_ATKBD_COMMAND_SCRIPT_MAX 10u
#define CONFIG_ATKBD_COMMAND_WRITE_LIMIT_MAX 32u
#define CONFIG_ATKBD_COMMAND_NAK_LIMIT_MAX 8u
#define CONFIG_ATKBD_SPECIAL_SEQUENCE_MAX 8u
#define CONFIG_ATKBD_PRESSED_WORD_COUNT 4u

#endif
