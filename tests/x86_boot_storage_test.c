// SPDX-License-Identifier: GPL-2.0-only
#include "x86_boot_storage.h"

#define TEST_PARAMETERS_KEY_OFFSET 30u
#define TEST_PARAMETERS_PATH_LENGTH_OFFSET 32u
#define TEST_PARAMETERS_HOST_BUS_OFFSET 36u
#define TEST_PARAMETERS_INTERFACE_OFFSET 40u
#define TEST_PARAMETERS_INTERFACE_PATH_OFFSET 48u
#define TEST_PARAMETERS_DEVICE_PATH_OFFSET 56u

static void clear_bytes(uint8_t *bytes, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = 0u;
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8u);
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
	size_t index;

	for (index = 0u; index < 8u; ++index)
		bytes[index] = (uint8_t)(value >> (index * 8u));
}

static void write_ascii(uint8_t *bytes, const char *text, size_t count)
{
	size_t index;

	for (index = 0u; index < count; ++index)
		bytes[index] = (uint8_t)text[index];
}

static void set_checksum(uint8_t *bytes, size_t count)
{
	uint8_t sum = 0u;
	size_t index;

	bytes[count - 1u] = 0u;
	for (index = 0u; index < count; ++index)
		sum = (uint8_t)(sum + bytes[index]);
	bytes[count - 1u] = (uint8_t)(0u - sum);
}

static void prepare_dpte(uint8_t *dpte, uint16_t command_port,
			 uint16_t control_port, uint8_t device)
{
	clear_bytes(dpte, X86_BOOT_EDD_DPTE_BYTES);
	write_le16(dpte, command_port);
	write_le16(dpte + 2u, control_port);
	dpte[4] = (uint8_t)(0xe0u | (device << 4u));
	dpte[5] = 0xcbu;
	dpte[6] = 0x0eu;
	dpte[7] = 0x01u;
	dpte[10] = 0x18u;
	dpte[14] = 0x11u;
	set_checksum(dpte, X86_BOOT_EDD_DPTE_BYTES);
}

static void prepare_boot_info(struct x86_boot_info *boot_info,
			      uint8_t boot_drive)
{
	struct x86_boot_storage_handoff *storage;
	uint8_t *parameters;

	clear_bytes((uint8_t *)boot_info, sizeof(*boot_info));
	boot_info->signature = X86_BOOT_INFO_SIGNATURE;
	boot_info->version = X86_BOOT_INFO_VERSION;
	boot_info->header_bytes = X86_BOOT_INFO_HEADER_BYTES;
	boot_info->range_bytes = X86_BOOT_MEMORY_RANGE_BYTES;
	storage = &boot_info->storage;
	storage->signature = X86_BOOT_STORAGE_SIGNATURE;
	storage->version = X86_BOOT_STORAGE_VERSION;
	storage->bytes = X86_BOOT_STORAGE_BYTES;
	storage->flags = X86_BOOT_STORAGE_FLAG_MASK;
	storage->interface_support = 1u;
	storage->boot_drive = boot_drive;
	storage->edd_version = 0x30u;
	parameters = storage->parameters;
	/* SeaBIOS returns the 30-byte base length while independently publishing
	 * the checksummed 44-byte EDD 3.0 path in the caller's 74-byte buffer. */
	write_le16(parameters, 30u);
	write_le64(parameters + 16u, 32768u);
	write_le16(parameters + 24u, 512u);
	write_le16(parameters + 26u, 0xf200u);
	write_le16(parameters + 28u, 0xda80u);
	write_le16(parameters + TEST_PARAMETERS_KEY_OFFSET, 0xbeddu);
	parameters[TEST_PARAMETERS_PATH_LENGTH_OFFSET] = 44u;
	write_ascii(parameters + TEST_PARAMETERS_HOST_BUS_OFFSET, "PCI ", 4u);
	write_ascii(parameters + TEST_PARAMETERS_INTERFACE_OFFSET,
		    "ATA     ", 8u);
	parameters[TEST_PARAMETERS_INTERFACE_PATH_OFFSET] = 0u;
	parameters[TEST_PARAMETERS_INTERFACE_PATH_OFFSET + 1u] = 1u;
	parameters[TEST_PARAMETERS_INTERFACE_PATH_OFFSET + 2u] = 1u;
	parameters[TEST_PARAMETERS_INTERFACE_PATH_OFFSET + 3u] = 0u;
	parameters[TEST_PARAMETERS_DEVICE_PATH_OFFSET] = 0u;
	set_checksum(parameters + TEST_PARAMETERS_KEY_OFFSET, 44u);
	prepare_dpte(storage->dpte, 0x01f0u, 0x03f6u, 0u);
	write_le64(storage->volume_start_lba, 0u);
	write_le64(storage->volume_sector_count, 32768u);
	write_le16((uint8_t *)&storage->volume_sector_bytes, 512u);
}

static int locator_is_unchanged(
	const struct x86_boot_device_locator *locator)
{
	return locator->sector_count == 0x1122334455667788ull &&
	       locator->flags == 0xa5a55a5au;
}

static void prepare_sentinel(struct x86_boot_device_locator *locator)
{
	clear_bytes((uint8_t *)locator, sizeof(*locator));
	locator->sector_count = 0x1122334455667788ull;
	locator->flags = 0xa5a55a5au;
}

static int test_seabios_primary_master(void)
{
	struct x86_boot_info boot_info;
	struct x86_boot_device_locator locator;
	const struct x86_ata_device_identity identity = {
		.sector_count = 32768u,
		.command_port = 0x01f0u,
		.control_port = 0x03f6u,
		.logical_sector_bytes = 512u,
		.device = 0u,
	};

	prepare_boot_info(&boot_info, 0x80u);
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
	    X86_BOOT_STORAGE_OK)
		return 1;
	return locator.sector_count == 32768u &&
	       locator.boot_volume_first_lba == 0u &&
	       locator.boot_volume_sector_count == 32768u &&
	       locator.logical_sector_bytes == 512u &&
	       locator.command_port == 0x01f0u &&
	       locator.control_port == 0x03f6u && locator.ata_device == 0u &&
	       locator.host_bus == X86_BOOT_HOST_BUS_PCI &&
	       locator.pci_bus == 0u && locator.pci_slot == 1u &&
	       locator.pci_function == 1u && locator.ata_channel == 0u &&
	       x86_boot_device_locator_matches_ata(&locator, &identity)
		       ? 0
		       : 1;
}

static int test_partitioned_boot_volume_extent(void)
{
	struct x86_boot_info boot_info;
	struct x86_boot_device_locator locator;

	prepare_boot_info(&boot_info, 0x80u);
	write_le64(boot_info.storage.volume_start_lba, 2048u);
	write_le64(boot_info.storage.volume_sector_count, 16384u);
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_OK ||
	    locator.boot_volume_first_lba != 2048u ||
	    locator.boot_volume_sector_count != 16384u)
		return 1;

	prepare_sentinel(&locator);
	write_le64(boot_info.storage.volume_start_lba, 32000u);
	write_le64(boot_info.storage.volume_sector_count, 1000u);
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_CORRUPT ||
	    !locator_is_unchanged(&locator))
		return 2;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	write_le16((uint8_t *)&boot_info.storage.volume_sector_bytes, 4096u);
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_CORRUPT ||
	    !locator_is_unchanged(&locator))
		return 3;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	write_le64(boot_info.storage.volume_sector_count, 0u);
	return x86_boot_storage_decode(&boot_info, 0x80u, &locator) ==
		       X86_BOOT_STORAGE_CORRUPT &&
	       locator_is_unchanged(&locator)
	       ? 0
	       : 4;
}

static int test_bios_drive_number_is_not_topology(void)
{
	struct x86_boot_info boot_info;
	struct x86_boot_device_locator locator;
	const struct x86_ata_device_identity identity = {
		.sector_count = 32768u,
		.command_port = 0x01f0u,
		.control_port = 0x03f6u,
		.logical_sector_bytes = 512u,
		.device = 0u,
	};

	prepare_boot_info(&boot_info, 0x81u);
	return x86_boot_storage_decode(&boot_info, 0x81u, &locator) ==
			       X86_BOOT_STORAGE_OK &&
	       locator.bios_drive == 0x81u &&
	       x86_boot_device_locator_matches_ata(&locator, &identity)
		       ? 0
		       : 1;
}

static int test_secondary_slave_identity(void)
{
	struct x86_boot_info boot_info;
	struct x86_boot_device_locator locator;
	const struct x86_ata_device_identity secondary_slave = {
		.sector_count = 32768u,
		.command_port = 0x0170u,
		.control_port = 0x0376u,
		.logical_sector_bytes = 512u,
		.device = 1u,
	};
	const struct x86_ata_device_identity primary_master = {
		.sector_count = 32768u,
		.command_port = 0x01f0u,
		.control_port = 0x03f6u,
		.logical_sector_bytes = 512u,
		.device = 0u,
	};

	prepare_boot_info(&boot_info, 0x82u);
	boot_info.storage.parameters[TEST_PARAMETERS_INTERFACE_PATH_OFFSET + 3u] =
		1u;
	boot_info.storage.parameters[TEST_PARAMETERS_DEVICE_PATH_OFFSET] = 1u;
	set_checksum(boot_info.storage.parameters + TEST_PARAMETERS_KEY_OFFSET,
		     44u);
	prepare_dpte(boot_info.storage.dpte, 0x0170u, 0x0376u, 1u);
	if (x86_boot_storage_decode(&boot_info, 0x82u, &locator) !=
	    X86_BOOT_STORAGE_OK)
		return 1;
	return x86_boot_device_locator_matches_ata(&locator,
						  &secondary_slave) &&
	       !x86_boot_device_locator_matches_ata(&locator, &primary_master)
		       ? 0
		       : 1;
}

static int test_unavailable_and_atomic_failures(void)
{
	struct x86_boot_info boot_info;
	struct x86_boot_device_locator locator;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	boot_info.storage.flags = 0u;
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_NOT_AVAILABLE ||
	    !locator_is_unchanged(&locator))
		return 1;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	boot_info.storage.flags &= ~X86_BOOT_STORAGE_VOLUME_PRESENT;
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_CORRUPT ||
	    !locator_is_unchanged(&locator))
		return 1;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	boot_info.storage.flags = X86_BOOT_STORAGE_EXTENSIONS_PRESENT;
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_NOT_AVAILABLE ||
	    !locator_is_unchanged(&locator))
		return 1;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	if (x86_boot_storage_decode(&boot_info, 0x81u, &locator) !=
		    X86_BOOT_STORAGE_CORRUPT ||
	    !locator_is_unchanged(&locator))
		return 1;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	boot_info.storage.dpte[15] ^= 1u;
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_CORRUPT ||
	    !locator_is_unchanged(&locator))
		return 1;

	prepare_boot_info(&boot_info, 0x80u);
	prepare_sentinel(&locator);
	boot_info.storage.parameters[73] ^= 1u;
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_CORRUPT ||
	    !locator_is_unchanged(&locator))
		return 1;
	return 0;
}

static int test_unsupported_interface_and_mismatches(void)
{
	struct x86_boot_info boot_info;
	struct x86_boot_device_locator locator;
	struct x86_ata_device_identity identity = {
		.sector_count = 32768u,
		.command_port = 0x01f0u,
		.control_port = 0x03f6u,
		.logical_sector_bytes = 512u,
		.device = 0u,
	};

	prepare_boot_info(&boot_info, 0x80u);
	write_ascii(boot_info.storage.parameters +
			    TEST_PARAMETERS_INTERFACE_OFFSET,
		    "SCSI    ", 8u);
	set_checksum(boot_info.storage.parameters + TEST_PARAMETERS_KEY_OFFSET,
		     44u);
	prepare_sentinel(&locator);
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_UNSUPPORTED ||
	    !locator_is_unchanged(&locator))
		return 1;

	prepare_boot_info(&boot_info, 0x80u);
	boot_info.storage.parameters[TEST_PARAMETERS_DEVICE_PATH_OFFSET] = 1u;
	set_checksum(boot_info.storage.parameters + TEST_PARAMETERS_KEY_OFFSET,
		     44u);
	prepare_sentinel(&locator);
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
		    X86_BOOT_STORAGE_CORRUPT ||
	    !locator_is_unchanged(&locator))
		return 1;

	prepare_boot_info(&boot_info, 0x80u);
	if (x86_boot_storage_decode(&boot_info, 0x80u, &locator) !=
	    X86_BOOT_STORAGE_OK)
		return 1;
	identity.sector_count--;
	if (x86_boot_device_locator_matches_ata(&locator, &identity))
		return 1;
	identity.sector_count++;
	identity.reserved = 1u;
	return !x86_boot_device_locator_matches_ata(&locator, &identity) ? 0 : 1;
}

int main(void)
{
	return test_seabios_primary_master() ||
	       test_partitioned_boot_volume_extent() ||
	       test_bios_drive_number_is_not_topology() ||
	       test_secondary_slave_identity() ||
	       test_unavailable_and_atomic_failures() ||
	       test_unsupported_interface_and_mismatches();
}
