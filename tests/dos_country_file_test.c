// SPDX-License-Identifier: GPL-2.0-only
#include "dos_country_file.h"
#include "test_entry.h"

#define TEST_BUFFER_BYTES 4096u

static uint8_t encoded[TEST_BUFFER_BYTES];
static struct dos_country_catalog catalog;

static int fail_if(bool condition)
{
	return condition ? 1 : 0;
}

static uint8_t package_upcase(const struct dos_nls_package *package,
			      uint8_t byte)
{
	if (byte >= 'a' && byte <= 'z')
		return (uint8_t)(byte - ('a' - 'A'));
	if (byte >= 0x80u)
		return package->upcase_high[byte - 0x80u];
	return byte;
}

static uint8_t package_collate(const struct dos_nls_package *package,
			       uint8_t byte)
{
	if (byte >= 'a' && byte <= 'z')
		return (uint8_t)(byte - ('a' - 'A'));
	if (byte >= 0x80u)
		return package->collate_high[byte - 0x80u];
	return byte;
}

static bool expected_dbcs_lead(uint16_t code_page, uint8_t byte)
{
	if (code_page == 932u)
		return (byte >= 0x81u && byte <= 0x9fu) ||
		       (byte >= 0xe0u && byte <= 0xfcu);
	if (code_page == 934u)
		return byte >= 0x81u && byte <= 0xbfu;
	if (code_page == 936u || code_page == 938u)
		return byte >= 0x81u && byte <= 0xfcu;
	return false;
}

static bool expected_dbcs_single(uint16_t code_page, uint8_t byte)
{
	if (byte == 0x80u)
		return true;
	if (code_page == 932u)
		return byte >= 0xa0u && byte <= 0xdfu;
	if (code_page == 934u)
		return byte >= 0xc0u && byte <= 0xfcu;
	return false;
}

static uint8_t expected_dbcs_class(uint16_t code_page, uint8_t byte)
{
	if (expected_dbcs_single(code_page, byte))
		return 0u;
	if (expected_dbcs_lead(code_page, byte))
		return 1u;
	return 2u;
}

static bool declared_dbcs_lead(const struct dos_nls_package *package,
			       uint8_t byte)
{
	size_t index;

	for (index = 0u; index + 2u < package->dbcs.length; index += 2u)
		if (byte >= package->dbcs.ranges[index] &&
		    byte <= package->dbcs.ranges[index + 1u])
			return true;
	return false;
}

static bool expected_dbcs_collate(uint16_t code_page, uint8_t byte,
				  uint8_t *key)
{
	uint16_t next = 0x80u;
	uint8_t class;

	for (class = 0u; class < 3u; ++class) {
		uint16_t candidate;

		for (candidate = 0x80u; candidate <= 0xffu; ++candidate) {
			if (expected_dbcs_class(code_page, (uint8_t)candidate) !=
			    class)
				continue;
			if (candidate == byte) {
				if (next > 0xffu)
					return false;
				*key = (uint8_t)next;
				return true;
			}
			++next;
		}
	}
	return false;
}

static int test_cp437_rules(void)
{
	const struct dos_nls_package *package = dos_nls_find_package(437u);
	uint16_t byte;

	if (package == NULL || !package->complete ||
	    package->upcase_high[0x81u - 0x80u] != 0x9au ||
	    package->upcase_high[0x82u - 0x80u] != 0x90u ||
	    package->upcase_high[0x86u - 0x80u] != 0x8fu ||
	    package->upcase_high[0x87u - 0x80u] != 0x80u ||
	    package->upcase_high[0x91u - 0x80u] != 0x92u ||
	    package->upcase_high[0x94u - 0x80u] != 0x99u ||
	    package->upcase_high[0xa4u - 0x80u] != 0xa5u ||
	    package->upcase_high[0xe5u - 0x80u] != 0xe4u ||
	    package->upcase_high[0xedu - 0x80u] != 0xe8u)
		return 1;
	if (package->upcase_high[0x83u - 0x80u] != 'A' ||
	    package->upcase_high[0x88u - 0x80u] != 'E' ||
	    package->upcase_high[0x8bu - 0x80u] != 'I' ||
	    package->upcase_high[0x93u - 0x80u] != 'O' ||
	    package->upcase_high[0x96u - 0x80u] != 'U' ||
	    package->upcase_high[0x98u - 0x80u] != 'Y')
		return 2;
	if (package->collate_high[0x9bu - 0x80u] != 0x9bu ||
	    package->collate_high[0x9cu - 0x80u] != 0x9cu ||
	    package->collate_high[0x9fu - 0x80u] != 'F' ||
	    package->collate_high[0xa6u - 0x80u] != 'A' ||
	    package->collate_high[0xa7u - 0x80u] != 'O' ||
	    package->collate_high[0xe1u - 0x80u] != 'S' ||
	    package->collate_high[0xfcu - 0x80u] != 'N')
		return 3;
	for (byte = 0u; byte <= 0xffu; ++byte) {
		uint8_t upper = package_upcase(package, (uint8_t)byte);

		if (package_upcase(package, upper) != upper ||
		    package_collate(package, upper) !=
			package_collate(package, (uint8_t)byte))
			return 4;
	}
	return 0;
}

static int test_dbcs_rules(uint16_t code_page)
{
	const struct dos_nls_package *package = dos_nls_find_package(code_page);
	uint16_t byte;

	if (package == NULL || !package->complete)
		return 1;
	for (byte = 0x80u; byte <= 0xffu; ++byte) {
		uint8_t expected;

		if (declared_dbcs_lead(package, (uint8_t)byte) !=
			expected_dbcs_lead(code_page, (uint8_t)byte) ||
		    !expected_dbcs_collate(code_page, (uint8_t)byte,
					    &expected) ||
		    package->collate_high[byte - 0x80u] != expected ||
		    package->upcase_high[byte - 0x80u] != (uint8_t)byte)
			return 2;
	}
	return 0;
}

static int run_tests(void)
{
	uint64_t expected;
	size_t written;
	enum dos_country_status status;
	uint16_t index;
	int result;

	result = test_cp437_rules();
	if (result != 0)
		return 10 + result;
	result = test_dbcs_rules(932u);
	if (result != 0)
		return 20 + result;
	result = test_dbcs_rules(934u);
	if (result != 0)
		return 30 + result;
	result = test_dbcs_rules(936u);
	if (result != 0)
		return 40 + result;

	if (dos_country_encoded_size(&expected) != DOS_COUNTRY_OK ||
	    expected > TEST_BUFFER_BYTES ||
	    dos_country_encode(encoded, sizeof(encoded), &written) !=
		DOS_COUNTRY_OK || written != expected)
		return 1;
	status = dos_country_parse(encoded, written, &catalog);
	if (status != DOS_COUNTRY_OK || catalog.package_count != 4u ||
	    catalog.format_version != DOS_COUNTRY_FILE_VERSION)
		return 2;
	for (index = 0u; index < catalog.package_count; ++index) {
		const struct dos_nls_package *package =
			&catalog.packages[index].package;

		if (!dos_nls_validate_package(package) || !package->complete ||
		    (index != 0u && package->code_page <=
			catalog.packages[index - 1u].package.code_page))
			return 3;
	}
	if (catalog.packages[0].package.code_page != 437u ||
	    catalog.packages[1].package.code_page != 932u ||
	    catalog.packages[2].package.code_page != 934u ||
	    catalog.packages[3].package.code_page != 936u)
		return 4;

	encoded[written - 1u] ^= 1u;
	if (dos_country_parse(encoded, written, &catalog) !=
	    DOS_COUNTRY_CHECKSUM || catalog.package_count != 0u)
		return 5;
	encoded[written - 1u] ^= 1u;
	if (dos_country_parse(encoded, written - 1u, &catalog) !=
	    DOS_COUNTRY_FORMAT)
		return 6;
	if (dos_country_encode(encoded, (size_t)expected - 1u, &written) !=
	    DOS_COUNTRY_CAPACITY || written != 0u)
		return 7;
	if (fail_if(dos_country_parse(NULL, 0u, &catalog) !=
		    DOS_COUNTRY_INVALID_ARGUMENT))
		return 8;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)
