// SPDX-License-Identifier: GPL-2.0-only
/* MS-DOS DPB layout encoder for guest-visible drive metadata. */
#include "dos_dpb.h"

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static void write_far(uint8_t *bytes, uint16_t segment, uint16_t offset)
{
	write_le16(bytes, offset);
	write_le16(bytes + 2u, segment);
}

static bool cluster_shift(uint8_t sectors_per_cluster, uint8_t *shift)
{
	uint8_t value = sectors_per_cluster;
	uint8_t prepared = 0u;

	if (value == 0u || (value & (uint8_t)(value - 1u)) != 0u)
		return false;
	while (value > 1u) {
		value >>= 1u;
		++prepared;
	}
	*shift = prepared;
	return true;
}

enum dos_dpb_status dos_dpb_encode(
	const struct dos_dpb_parameters *parameters,
	uint8_t output[DOS_DPB_BYTES])
{
	uint8_t shift;
	size_t index;

	if (parameters == NULL || output == NULL ||
	    parameters->bytes_per_sector == 0u ||
	    parameters->fat_count == 0u ||
	    !cluster_shift(parameters->sectors_per_cluster, &shift))
		return DOS_DPB_INVALID_ARGUMENT;
	if (parameters->root_start > 0xffffu ||
	    parameters->data_start > 0xffffu)
		return DOS_DPB_UNREPRESENTABLE;
	for (index = 0u; index < DOS_DPB_BYTES; ++index)
		output[index] = 0u;
	output[0] = parameters->drive;
	output[1] = parameters->unit;
	write_le16(output + 2u, parameters->bytes_per_sector);
	output[4] = (uint8_t)(parameters->sectors_per_cluster - 1u);
	output[5] = shift;
	write_le16(output + 6u, parameters->reserved_sectors);
	output[8] = parameters->fat_count;
	write_le16(output + 9u, parameters->root_entries);
	write_le16(output + 11u, (uint16_t)parameters->data_start);
	write_le16(output + 13u, parameters->maximum_cluster);
	write_le16(output + 15u, parameters->sectors_per_fat);
	write_le16(output + 17u, (uint16_t)parameters->root_start);
	write_far(output + 19u, DOS_DPB_SEGMENT, DOS_DPB_DRIVER_OFFSET);
	output[23] = parameters->media;
	output[24] = 0u;
	output[25] = 0xffu;
	output[26] = 0xffu;
	output[27] = 0xffu;
	output[28] = 0xffu;
	write_le16(output + 29u, 0u);
	write_le16(output + 31u, 0xffffu);
	return DOS_DPB_OK;
}

void dos_dpb_encode_driver_header(uint8_t output[DOS_DPB_DRIVER_BYTES])
{
	size_t index;

	if (output == NULL)
		return;
	for (index = 0u; index < DOS_DPB_DRIVER_BYTES; ++index)
		output[index] = 0u;
	/* End of chain, block device, one unit. */
	output[0] = 0xffu;
	output[1] = 0xffu;
	output[2] = 0xffu;
	output[3] = 0xffu;
	/* This block driver implements the removable-media
	 * query used by INT 21h/AX=4408h. */
	write_le16(output + 4u, 0x0800u);
	write_le16(output + 6u, (uint16_t)(DOS_DPB_DRIVER_OFFSET + 18u));
	write_le16(output + 8u, (uint16_t)(DOS_DPB_DRIVER_OFFSET + 19u));
	output[10] = 1u;
	/* Strategy/interrupt direct-call fallbacks return far without mutation. */
	output[18] = 0xcbu;
	output[19] = 0xcbu;
}
