/* SPDX-License-Identifier: GPL-2.0-only */
/* Byte-level regression tests for MS-DOS guest ABI layouts. */
#include "dos_abi.h"
#include "test_entry.h"

static struct dos_far_pointer16 far_pointer;
static struct dos_mcb40 mcb;
static struct dos_fcb40 fcb;
static struct dos_find_buffer40 find_buffer;
static struct dos_psp40 psp;
static struct dos_device_extended_rw_request40 rw_request;
static struct dos_exec_load_result40 exec_result;
static struct dos_mz_header40 mz_header;

static bool bytes_equal(const uint8_t *actual, const uint8_t *expected,
			size_t length)
{
	size_t index;

	for (index = 0; index < length; ++index) {
		if (actual[index] != expected[index])
			return false;
	}
	return true;
}

static bool test_far_pointer(void)
{
	static const uint8_t expected[] = { 0x34, 0x12, 0xcd, 0xab };

	far_pointer.offset = 0x1234;
	far_pointer.segment = 0xabcd;
	return bytes_equal((const uint8_t *)(const void *)&far_pointer,
			   expected, sizeof(expected));
}

static bool test_mcb(void)
{
	static const uint8_t expected[] = {
		0x5a, 0x34, 0x12, 0x78, 0x56, 0, 0, 0,
		'M', 'Y', 'P', 'R', 'O', 'G', ' ', ' '
	};
	static const uint8_t name[] = { 'M', 'Y', 'P', 'R', 'O', 'G', ' ', ' ' };
	size_t index;

	mcb.signature = DOS_MCB_SIGNATURE_END;
	mcb.owner_psp = 0x1234;
	mcb.size_paragraphs = 0x5678;
	for (index = 0; index < sizeof(name); ++index)
		mcb.owner_name[index] = name[index];
	return bytes_equal((const uint8_t *)(const void *)&mcb, expected,
			   sizeof(expected));
}

static bool test_fcb(void)
{
	const uint8_t *bytes = (const uint8_t *)(const void *)&fcb;

	fcb.drive = 3;
	fcb.current_extent = 0x1234;
	fcb.record_size = 0x0080;
	fcb.file_size_or_search_state = 0x89abcdef;
	fcb.modified_date = 0x4a21;
	fcb.next_record = 0x7f;
	fcb.random_record[0] = 0x11;
	fcb.random_record[1] = 0x22;
	fcb.random_record[2] = 0x33;
	fcb.random_record[3] = 0x44;

	return bytes[0] == 3 && bytes[12] == 0x34 && bytes[13] == 0x12 &&
	       bytes[14] == 0x80 && bytes[15] == 0 && bytes[16] == 0xef &&
	       bytes[17] == 0xcd && bytes[18] == 0xab && bytes[19] == 0x89 &&
	       bytes[32] == 0x7f && bytes[33] == 0x11 && bytes[36] == 0x44;
}

static bool test_find_dta(void)
{
	const uint8_t *bytes = (const uint8_t *)(const void *)&find_buffer;

	find_buffer.search_drive = 2;
	find_buffer.last_entry = 0x1234;
	find_buffer.directory_start = 0x5678;
	find_buffer.found_attributes = 0x20;
	find_buffer.modified_time = 0x1122;
	find_buffer.modified_date = 0x3344;
	find_buffer.file_size = 0x89abcdef;
	find_buffer.packed_name[0] = 'F';
	find_buffer.packed_name[12] = 0;

	return bytes[0] == 2 && bytes[13] == 0x34 && bytes[14] == 0x12 &&
	       bytes[15] == 0x78 && bytes[16] == 0x56 && bytes[21] == 0x20 &&
	       bytes[22] == 0x22 && bytes[23] == 0x11 && bytes[26] == 0xef &&
	       bytes[29] == 0x89 && bytes[30] == 'F' && bytes[42] == 0;
}

static bool test_psp_overlays(void)
{
	uint8_t *bytes = (uint8_t *)(void *)&psp;

	psp.prefix.exit_instruction = 0x20cd;
	psp.prefix.parent_psp = 0x3456;
	psp.prefix.jft_pointer.offset = 0x0018;
	psp.prefix.jft_pointer.segment = 0x3456;
	psp.compatibility.first.first_fcb.drive = 1;
	psp.compatibility.second.second_fcb.drive = 2;
	psp.compatibility.command.command_tail.length = 3;
	psp.compatibility.command.command_tail.data[0] = 'A';
	psp.compatibility.command.command_tail.data[1] = 'B';
	psp.compatibility.command.command_tail.data[2] = '\r';

	return bytes[0] == 0xcd && bytes[1] == 0x20 &&
	       bytes[0x16] == 0x56 && bytes[0x17] == 0x34 &&
	       bytes[0x34] == 0x18 && bytes[0x35] == 0 &&
	       bytes[0x36] == 0x56 && bytes[0x37] == 0x34 &&
	       bytes[DOS_PSP_FIRST_FCB_OFFSET] == 1 &&
	       bytes[DOS_PSP_SECOND_FCB_OFFSET] == 2 &&
	       bytes[DOS_PSP_COMMAND_TAIL_OFFSET] == 3 &&
	       bytes[DOS_PSP_COMMAND_TAIL_OFFSET + 1] == 'A' &&
	       bytes[DOS_PSP_COMMAND_TAIL_OFFSET + 3] == '\r';
}

static bool test_device_request(void)
{
	const uint8_t *bytes = (const uint8_t *)(const void *)&rw_request;

	rw_request.legacy.header.length = 30;
	rw_request.legacy.header.function = DOS_DEVICE_REQUEST_READ;
	rw_request.legacy.header.status = DOS_DEVICE_STATUS_ERROR |
					  DOS_DEVICE_STATUS_DONE | 5;
	rw_request.legacy.media_descriptor = 0xf8;
	rw_request.legacy.transfer_buffer.offset = 0x2468;
	rw_request.legacy.transfer_buffer.segment = 0x1357;
	rw_request.legacy.sector_count = 0x0102;
	rw_request.legacy.start_sector = 0xffff;
	rw_request.volume_id = 0x11223344;
	rw_request.start_sector32 = 0x89abcdef;

	return bytes[0] == 30 && bytes[2] == DOS_DEVICE_REQUEST_READ &&
	       bytes[3] == 5 && bytes[4] == 0x81 && bytes[13] == 0xf8 &&
	       bytes[14] == 0x68 && bytes[15] == 0x24 && bytes[16] == 0x57 &&
	       bytes[17] == 0x13 && bytes[18] == 2 && bytes[19] == 1 &&
	       bytes[20] == 0xff && bytes[21] == 0xff && bytes[22] == 0x44 &&
	       bytes[25] == 0x11 && bytes[26] == 0xef && bytes[29] == 0x89;
}

static bool test_exec_and_mz(void)
{
	const uint8_t *exec_bytes =
		(const uint8_t *)(const void *)&exec_result;
	const uint8_t *mz_bytes = (const uint8_t *)(const void *)&mz_header;

	exec_result.parameters.environment_segment = 0x1234;
	exec_result.parameters.command_line.offset = 0x0080;
	exec_result.parameters.command_line.segment = 0x2345;
	exec_result.initial_sp = 0x1111;
	exec_result.initial_ss = 0x2222;
	exec_result.initial_ip = 0x3333;
	exec_result.initial_cs = 0x4444;

	mz_header.signature = DOS_MZ_SIGNATURE;
	mz_header.pages = 0x1234;
	mz_header.relocation_table_offset = 0x001c;
	mz_header.overlay_number = 2;
	mz_header.symbol_table_offset = 0x89abcdef;

	return exec_bytes[0] == 0x34 && exec_bytes[1] == 0x12 &&
	       exec_bytes[2] == 0x80 && exec_bytes[3] == 0 &&
	       exec_bytes[4] == 0x45 && exec_bytes[5] == 0x23 &&
	       exec_bytes[14] == 0x11 && exec_bytes[16] == 0x22 &&
	       exec_bytes[18] == 0x33 && exec_bytes[20] == 0x44 &&
	       mz_bytes[0] == 'M' && mz_bytes[1] == 'Z' &&
	       mz_bytes[4] == 0x34 && mz_bytes[5] == 0x12 &&
	       mz_bytes[24] == 0x1c && mz_bytes[25] == 0 &&
	       mz_bytes[28] == 0xef && mz_bytes[31] == 0x89 &&
	       DOS_EXEC_OVERLAY == 3u &&
	       DOS_EXEC_FUNCTION_OVERLAY_BIT == 2u &&
	       dos_exec_subfunction_is_valid(0u) &&
	       dos_exec_subfunction_is_valid(1u) &&
	       !dos_exec_subfunction_is_valid(2u) &&
	       dos_exec_subfunction_is_valid(3u);
}

static int run_tests(void)
{
	if (!test_far_pointer())
		return 1;
	if (!test_mcb())
		return 2;
	if (!test_fcb())
		return 3;
	if (!test_find_dta())
		return 4;
	if (!test_psp_overlays())
		return 5;
	if (!test_device_request())
		return 6;
	if (!test_exec_and_mz())
		return 7;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)
