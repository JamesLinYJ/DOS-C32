// SPDX-License-Identifier: GPL-2.0-only
/* Hosted build tool; country tables and validation remain shared with the kernel. */
#include <stdio.h>

#include "dos_country_file.h"

#define COUNTRY_TOOL_CAPACITY 65536u

static uint8_t output[COUNTRY_TOOL_CAPACITY];

int main(int argc, char **argv)
{
	FILE *file;
	size_t written;
	enum dos_country_status status;

	if (argc != 2) {
		fprintf(stderr, "usage: %s OUTPUT\n", argv[0]);
		return 2;
	}
	status = dos_country_encode(output, sizeof(output), &written);
	if (status != DOS_COUNTRY_OK) {
		fprintf(stderr, "COUNTRY.SYS encoding failed: %d\n", (int)status);
		return 1;
	}
	file = fopen(argv[1], "wb");
	if (file == NULL) {
		perror(argv[1]);
		return 1;
	}
	if (fwrite(output, 1u, written, file) != written || fclose(file) != 0) {
		fprintf(stderr, "COUNTRY.SYS write failed: %s\n", argv[1]);
		return 1;
	}
	return 0;
}
