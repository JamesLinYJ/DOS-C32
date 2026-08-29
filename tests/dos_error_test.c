// SPDX-License-Identifier: GPL-2.0-only
/* Byte-level tests for DOS error-table semantics. */
#include "dos_error.h"
#include "test_entry.h"

struct fallback_case {
	uint8_t function;
	enum dos_error fallback;
};

struct metadata_case {
	enum dos_error error;
	uint8_t error_class;
	uint8_t action;
	uint8_t locus;
};

static const struct fallback_case fallback_cases[] = {
	{ 0x38u, DOS_ERROR_FILE_NOT_FOUND },
	{ 0x39u, DOS_ERROR_ACCESS_DENIED },
	{ 0x3au, DOS_ERROR_ACCESS_DENIED },
	{ 0x3bu, DOS_ERROR_PATH_NOT_FOUND },
	{ 0x3cu, DOS_ERROR_ACCESS_DENIED },
	{ 0x3du, DOS_ERROR_ACCESS_DENIED },
	{ 0x3eu, DOS_ERROR_INVALID_HANDLE },
	{ 0x3fu, DOS_ERROR_ACCESS_DENIED },
	{ 0x40u, DOS_ERROR_ACCESS_DENIED },
	{ 0x41u, DOS_ERROR_ACCESS_DENIED },
	{ 0x42u, DOS_ERROR_INVALID_FUNCTION },
	{ 0x43u, DOS_ERROR_ACCESS_DENIED },
	{ 0x44u, DOS_ERROR_ACCESS_DENIED },
	{ 0x45u, DOS_ERROR_TOO_MANY_OPEN_FILES },
	{ 0x46u, DOS_ERROR_TOO_MANY_OPEN_FILES },
	{ 0x47u, DOS_ERROR_INVALID_DRIVE },
	{ 0x48u, DOS_ERROR_NOT_ENOUGH_MEMORY },
	{ 0x49u, DOS_ERROR_INVALID_BLOCK },
	{ 0x4au, DOS_ERROR_NOT_ENOUGH_MEMORY },
	{ 0x4bu, DOS_ERROR_ACCESS_DENIED },
	{ 0x4eu, DOS_ERROR_NO_MORE_FILES },
	{ 0x4fu, DOS_ERROR_NO_MORE_FILES },
	{ 0x56u, DOS_ERROR_ACCESS_DENIED },
	{ 0x57u, DOS_ERROR_INVALID_FUNCTION },
	{ 0x58u, DOS_ERROR_INVALID_FUNCTION },
	{ 0x5au, DOS_ERROR_ACCESS_DENIED },
	{ 0x5bu, DOS_ERROR_ACCESS_DENIED },
	{ 0x5cu, DOS_ERROR_LOCK_VIOLATION },
	{ 0x65u, DOS_ERROR_FILE_NOT_FOUND },
	{ 0x66u, DOS_ERROR_FILE_NOT_FOUND },
	{ 0x67u, DOS_ERROR_INVALID_FUNCTION },
	{ 0x68u, DOS_ERROR_INVALID_HANDLE },
	{ 0x69u, DOS_ERROR_ACCESS_DENIED },
	{ 0x6au, DOS_ERROR_INVALID_HANDLE },
	{ 0x6cu, DOS_ERROR_ACCESS_DENIED },
};

static const struct metadata_case metadata_cases[] = {
	{ DOS_ERROR_FILE_NOT_FOUND, DOS_ERROR_CLASS_NOT_FOUND,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK },
	{ DOS_ERROR_TOO_MANY_OPEN_FILES, DOS_ERROR_CLASS_OUT_OF_RESOURCE,
	  DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_UNKNOWN },
	{ DOS_ERROR_INVALID_HANDLE, DOS_ERROR_CLASS_APPLICATION,
	  DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_UNKNOWN },
	{ DOS_ERROR_ARENA_TRASHED, DOS_ERROR_CLASS_APPLICATION,
	  DOS_ERROR_ACTION_PANIC, DOS_ERROR_LOCUS_MEMORY },
	{ DOS_ERROR_NOT_ENOUGH_MEMORY, DOS_ERROR_CLASS_OUT_OF_RESOURCE,
	  DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_MEMORY },
	{ DOS_ERROR_BAD_FORMAT, DOS_ERROR_CLASS_BAD_FORMAT,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_UNKNOWN },
	{ DOS_ERROR_INVALID_DRIVE, DOS_ERROR_CLASS_NOT_FOUND,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK },
	{ DOS_ERROR_CURRENT_DIRECTORY, DOS_ERROR_CLASS_AUTHORIZATION,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK },
	{ DOS_ERROR_NOT_SAME_DEVICE, DOS_ERROR_CLASS_UNKNOWN,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK },
	{ DOS_ERROR_FILE_EXISTS, DOS_ERROR_CLASS_ALREADY_EXISTS,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_DISK },
	{ DOS_ERROR_SHARING_VIOLATION, DOS_ERROR_CLASS_LOCKED,
	  DOS_ERROR_ACTION_DELAY_RETRY, DOS_ERROR_LOCUS_DISK },
	{ DOS_ERROR_INVALID_PASSWORD, DOS_ERROR_CLASS_AUTHORIZATION,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_UNKNOWN },
	{ DOS_ERROR_CANNOT_MAKE, DOS_ERROR_CLASS_OUT_OF_RESOURCE,
	  DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_DISK },
	{ DOS_ERROR_NOT_SUPPORTED, DOS_ERROR_CLASS_BAD_FORMAT,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_NETWORK },
	{ DOS_ERROR_ALREADY_ASSIGNED, DOS_ERROR_CLASS_ALREADY_EXISTS,
	  DOS_ERROR_ACTION_ASK_USER, DOS_ERROR_LOCUS_NETWORK },
	{ DOS_ERROR_FAIL_I24, DOS_ERROR_CLASS_UNKNOWN, DOS_ERROR_ACTION_ABORT,
	  DOS_ERROR_LOCUS_UNKNOWN },
	{ DOS_ERROR_SHARING_BUFFER_EXCEEDED,
	  DOS_ERROR_CLASS_OUT_OF_RESOURCE, DOS_ERROR_ACTION_ABORT,
	  DOS_ERROR_LOCUS_MEMORY },
	{ DOS_ERROR_HANDLE_EOF, DOS_ERROR_CLASS_OUT_OF_RESOURCE,
	  DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_UNKNOWN },
	{ DOS_ERROR_SYS_COMPONENT_NOT_LOADED, DOS_ERROR_CLASS_UNKNOWN,
	  DOS_ERROR_ACTION_ABORT, DOS_ERROR_LOCUS_DISK },
};

static int run_tests(void)
{
	struct dos_extended_error extended;
	size_t index;

	for (index = 0u; index < sizeof(fallback_cases) /
					   sizeof(fallback_cases[0]); ++index) {
		extended = (struct dos_extended_error){
			.code = 0u,
			.error_class = 0xa1u,
			.action = 0xa2u,
			.locus = 0xa3u,
		};
		if (dos_error_map_int21(fallback_cases[index].function,
					DOS_ERROR_GENERAL_FAILURE, &extended) !=
		    fallback_cases[index].fallback ||
		    extended.code != DOS_ERROR_GENERAL_FAILURE)
			return 1;
	}

	for (index = 0u; index < sizeof(metadata_cases) /
					   sizeof(metadata_cases[0]); ++index) {
		extended = (struct dos_extended_error){ 0u, 0xa1u, 0xa2u, 0xa3u };
		dos_error_record_extended(&extended, metadata_cases[index].error);
		if (extended.code != (uint16_t)metadata_cases[index].error ||
		    extended.error_class != metadata_cases[index].error_class ||
		    extended.action != metadata_cases[index].action ||
		    extended.locus != metadata_cases[index].locus)
			return 2;
	}

	/* An allowed error is not replaced, but remains the extended error too. */
	extended = (struct dos_extended_error){ 0u, 0u, 0u, 0u };
	if (dos_error_map_int21(0x3du, DOS_ERROR_PATH_NOT_FOUND, &extended) !=
		    DOS_ERROR_PATH_NOT_FOUND ||
	    extended.code != DOS_ERROR_PATH_NOT_FOUND)
		return 3;

	/* Calls absent from I21_MAP_E_TAB leave the legacy code unchanged. */
	if (dos_error_map_int21(0x10u, DOS_ERROR_INVALID_HANDLE, &extended) !=
	    DOS_ERROR_INVALID_HANDLE)
		return 4;

	/* MS_TABLE marks these loci as call-specific; CAL_LK preserves them. */
	extended = (struct dos_extended_error){
		0u, 0u, 0u, DOS_ERROR_LOCUS_SERIAL_DEVICE
	};
	dos_error_record_extended(&extended, DOS_ERROR_ACCESS_DENIED);
	if (extended.error_class != DOS_ERROR_CLASS_AUTHORIZATION ||
	    extended.action != DOS_ERROR_ACTION_ASK_USER ||
	    extended.locus != DOS_ERROR_LOCUS_SERIAL_DEVICE)
		return 5;

	/* The catch-all table row preserves metadata for an unknown real error. */
	extended = (struct dos_extended_error){ 0u, 0x91u, 0x92u, 0x93u };
	dos_error_record_extended(&extended, (enum dos_error)123u);
	if (extended.code != 123u || extended.error_class != 0x91u ||
	    extended.action != 0x92u || extended.locus != 0x93u)
		return 6;

	/* A NULL extended-error destination cannot change legacy mapping. */
	if (dos_error_map_int21(0x48u, DOS_ERROR_GENERAL_FAILURE, NULL) !=
	    DOS_ERROR_NOT_ENOUGH_MEMORY)
		return 7;
	dos_error_record_extended(NULL, DOS_ERROR_INVALID_FUNCTION);
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)
