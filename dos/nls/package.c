// SPDX-License-Identifier: GPL-2.0-only
/*
 * Immutable DOS national-language package registry.
 *
 * The public ABI follows MS-DOS. Internal table-bearing NLS packages use
 * validated immutable descriptors and generation-checked prepare/commit
 * publication.
 *
 * DBCS lead-byte ranges are encoded as immutable, bounds-carrying C tables.
 */
#include "dos_nls.h"

#define DOS_NLS_DBCS_MAXIMUM_BYTES 8u
#define DOS_NLS_HIGH_TABLE_BYTES 128u

static const uint8_t sbcs_ranges[] = {0x00u, 0x00u};
static const uint8_t cp932_ranges[] = {
	0x81u, 0x9fu, 0xe0u, 0xfcu, 0x00u, 0x00u,
};
static const uint8_t cp934_ranges[] = {
	0x81u, 0xbfu, 0x00u, 0x00u,
};
static const uint8_t cp936_ranges[] = {
	0x81u, 0xfcu, 0x00u, 0x00u,
};

/*
 * CP437 uses two distinct byte-local rules.  Upcase first selects an exact
 * capital that CP437 can encode, then folds a Latin accent to its base capital
 * only when the exact capital is absent.  Collation uses accent-insensitive
 * Latin primary keys, keeps unrelated symbols distinct, and joins the two
 * representable Greek case pairs.
 *
 * DBCS collation is structural: valid single bytes sort first, lead bytes sort
 * second, and reserved bytes sort last.  Order within each class is stable.
 * A byte-local upcase table cannot transform a two-byte character, so DBCS
 * high bytes are preserved and the complete character remains the unit of
 * language-aware conversion.  CP936's structural order equals encoded-byte
 * order, so both meanings share one immutable materialization.
 */
#define NLS_MAP_ROW(transform, base) \
	transform((base) + 0u), transform((base) + 1u), \
	transform((base) + 2u), transform((base) + 3u), \
	transform((base) + 4u), transform((base) + 5u), \
	transform((base) + 6u), transform((base) + 7u), \
	transform((base) + 8u), transform((base) + 9u), \
	transform((base) + 10u), transform((base) + 11u), \
	transform((base) + 12u), transform((base) + 13u), \
	transform((base) + 14u), transform((base) + 15u)

#define NLS_MAP_HIGH_HALF(transform) \
	NLS_MAP_ROW(transform, 0x80u), NLS_MAP_ROW(transform, 0x90u), \
	NLS_MAP_ROW(transform, 0xa0u), NLS_MAP_ROW(transform, 0xb0u), \
	NLS_MAP_ROW(transform, 0xc0u), NLS_MAP_ROW(transform, 0xd0u), \
	NLS_MAP_ROW(transform, 0xe0u), NLS_MAP_ROW(transform, 0xf0u)

#define CP437_COLLATE_A(byte) \
	((byte) == 0x83u || (byte) == 0x84u || (byte) == 0x85u || \
	 (byte) == 0x86u || (byte) == 0x8eu || (byte) == 0x8fu || \
	 (byte) == 0x91u || (byte) == 0x92u || (byte) == 0xa0u || \
	 (byte) == 0xa6u)
#define CP437_COLLATE_C(byte) ((byte) == 0x80u || (byte) == 0x87u)
#define CP437_COLLATE_E(byte) \
	((byte) == 0x82u || (byte) == 0x88u || (byte) == 0x89u || \
	 (byte) == 0x8au || (byte) == 0x90u)
#define CP437_COLLATE_I(byte) \
	((byte) == 0x8bu || (byte) == 0x8cu || (byte) == 0x8du || \
	 (byte) == 0xa1u)
#define CP437_COLLATE_N(byte) \
	((byte) == 0xa4u || (byte) == 0xa5u || (byte) == 0xfcu)
#define CP437_COLLATE_O(byte) \
	((byte) == 0x93u || (byte) == 0x94u || (byte) == 0x95u || \
	 (byte) == 0x99u || (byte) == 0xa2u || (byte) == 0xa7u)
#define CP437_COLLATE_U(byte) \
	((byte) == 0x81u || (byte) == 0x96u || (byte) == 0x97u || \
	 (byte) == 0x9au || (byte) == 0xa3u)

#define CP437_COLLATE_KEY(byte) \
	((uint8_t)(CP437_COLLATE_A(byte) ? 'A' : \
	 CP437_COLLATE_C(byte) ? 'C' : CP437_COLLATE_E(byte) ? 'E' : \
	 (byte) == 0x9fu ? 'F' : CP437_COLLATE_I(byte) ? 'I' : \
	 CP437_COLLATE_N(byte) ? 'N' : CP437_COLLATE_O(byte) ? 'O' : \
	 (byte) == 0xe1u ? 'S' : CP437_COLLATE_U(byte) ? 'U' : \
	 (byte) == 0x98u ? 'Y' : (byte) == 0xe5u ? 0xe4u : \
	 (byte) == 0xedu ? 0xe8u : (byte)))

#define CP437_UPCASE_A_FOLD(byte) \
	((byte) == 0x83u || (byte) == 0x85u || (byte) == 0xa0u)
#define CP437_UPCASE_E_FOLD(byte) \
	((byte) == 0x88u || (byte) == 0x89u || (byte) == 0x8au)
#define CP437_UPCASE_I_FOLD(byte) \
	((byte) == 0x8bu || (byte) == 0x8cu || (byte) == 0x8du || \
	 (byte) == 0xa1u)
#define CP437_UPCASE_O_FOLD(byte) \
	((byte) == 0x93u || (byte) == 0x95u || (byte) == 0xa2u)
#define CP437_UPCASE_U_FOLD(byte) \
	((byte) == 0x96u || (byte) == 0x97u || (byte) == 0xa3u)

#define CP437_UPCASE(byte) \
	((uint8_t)((byte) == 0x81u ? 0x9au : \
	 (byte) == 0x82u ? 0x90u : (byte) == 0x84u ? 0x8eu : \
	 (byte) == 0x86u ? 0x8fu : (byte) == 0x87u ? 0x80u : \
	 (byte) == 0x91u ? 0x92u : (byte) == 0x94u ? 0x99u : \
	 (byte) == 0xa4u ? 0xa5u : (byte) == 0xe5u ? 0xe4u : \
	 (byte) == 0xedu ? 0xe8u : CP437_UPCASE_A_FOLD(byte) ? 'A' : \
	 CP437_UPCASE_E_FOLD(byte) ? 'E' : \
	 CP437_UPCASE_I_FOLD(byte) ? 'I' : \
	 CP437_UPCASE_O_FOLD(byte) ? 'O' : \
	 CP437_UPCASE_U_FOLD(byte) ? 'U' : \
	 (byte) == 0x98u ? 'Y' : (byte)))

#define DBCS_BYTE_ORDER(byte) ((uint8_t)(byte))
#define CP932_COLLATE_KEY(byte) \
	((uint8_t)(((byte) >= 0x81u && (byte) <= 0x9fu) ? \
	 (byte) + 0x40u : ((byte) >= 0xa0u && (byte) <= 0xdfu) ? \
	 (byte) - 0x1fu : (byte)))
#define CP934_COLLATE_KEY(byte) \
	((uint8_t)(((byte) >= 0x81u && (byte) <= 0xbfu) ? \
	 (byte) + 0x3du : ((byte) >= 0xc0u && (byte) <= 0xfcu) ? \
	 (byte) - 0x3fu : (byte)))
static const uint8_t cp437_collate_high[] = {
	NLS_MAP_HIGH_HALF(CP437_COLLATE_KEY),
};
static const uint8_t cp437_upcase_high[] = {
	NLS_MAP_HIGH_HALF(CP437_UPCASE),
};
static const uint8_t dbcs_byte_order_high[] = {
	NLS_MAP_HIGH_HALF(DBCS_BYTE_ORDER),
};
static const uint8_t cp932_collate_high[] = {
	NLS_MAP_HIGH_HALF(CP932_COLLATE_KEY),
};
static const uint8_t cp934_collate_high[] = {
	NLS_MAP_HIGH_HALF(CP934_COLLATE_KEY),
};
static_assert_expression(ARRAY_SIZE(cp437_collate_high) ==
			 DOS_NLS_HIGH_TABLE_BYTES,
			 "CP437 collate table must cover the high half");
static_assert_expression(ARRAY_SIZE(cp437_upcase_high) ==
			 DOS_NLS_HIGH_TABLE_BYTES,
			 "CP437 upcase table must cover the high half");
static_assert_expression(ARRAY_SIZE(dbcs_byte_order_high) ==
			 DOS_NLS_HIGH_TABLE_BYTES,
			 "DBCS byte-order table must cover the high half");
static_assert_expression(ARRAY_SIZE(cp932_collate_high) ==
			 DOS_NLS_HIGH_TABLE_BYTES,
			 "CP932 collate table must cover the high half");
static_assert_expression(ARRAY_SIZE(cp934_collate_high) ==
			 DOS_NLS_HIGH_TABLE_BYTES,
			 "CP934 collate table must cover the high half");
#undef CP934_COLLATE_KEY
#undef CP932_COLLATE_KEY
#undef DBCS_BYTE_ORDER
#undef CP437_UPCASE
#undef CP437_UPCASE_U_FOLD
#undef CP437_UPCASE_O_FOLD
#undef CP437_UPCASE_I_FOLD
#undef CP437_UPCASE_E_FOLD
#undef CP437_UPCASE_A_FOLD
#undef CP437_COLLATE_KEY
#undef CP437_COLLATE_U
#undef CP437_COLLATE_O
#undef CP437_COLLATE_N
#undef CP437_COLLATE_I
#undef CP437_COLLATE_E
#undef CP437_COLLATE_C
#undef CP437_COLLATE_A
#undef NLS_MAP_HIGH_HALF
#undef NLS_MAP_ROW

#define SBCS_PACKAGE(code) \
	{ \
		.dbcs = {sbcs_ranges, (uint16_t)sizeof(sbcs_ranges)}, \
		.format = {.country = DOS_NLS_DEFAULT_COUNTRY}, \
		.collate_high = (code) == 437u ? cp437_collate_high : NULL, \
		.upcase_high = (code) == 437u ? cp437_upcase_high : NULL, \
		.code_page = (code), \
		.complete = (code) == 437u, \
	}

static const struct dos_nls_package packages[] = {
	SBCS_PACKAGE(437u),
	SBCS_PACKAGE(850u),
	SBCS_PACKAGE(860u),
	SBCS_PACKAGE(862u),
	SBCS_PACKAGE(863u),
	SBCS_PACKAGE(864u),
	SBCS_PACKAGE(865u),
	{
		.dbcs = {cp932_ranges, (uint16_t)sizeof(cp932_ranges)},
		.format = {
		    .currency = {0x5cu}, .country = 81u, .date_format = 2u,
		    .thousands_separator = ',', .decimal_separator = '.',
		    .date_separator = '-', .time_separator = ':',
		    .currency_digits = 0u, .time_format = 1u,
		    .list_separator = ',',
		},
		.collate_high = cp932_collate_high,
		.upcase_high = dbcs_byte_order_high,
		.code_page = 932u,
		.complete = true,
	},
	{
		.dbcs = {cp934_ranges, (uint16_t)sizeof(cp934_ranges)},
		.format = {
		    .currency = {0x5cu}, .country = 82u, .date_format = 2u,
		    .thousands_separator = ',', .decimal_separator = '.',
		    .date_separator = '.', .time_separator = ':',
		    .currency_digits = 0u, .time_format = 1u,
		    .list_separator = ',',
		},
		.collate_high = cp934_collate_high,
		.upcase_high = dbcs_byte_order_high,
		.code_page = 934u,
		.complete = true,
	},
	{
		.dbcs = {cp936_ranges, (uint16_t)sizeof(cp936_ranges)},
		.format = {
		    .currency = {0x5cu}, .country = 86u, .date_format = 2u,
		    .thousands_separator = ',', .decimal_separator = '.',
		    .date_separator = '.', .time_separator = ':',
		    .currency_digits = 2u, .time_format = 0u,
		    .list_separator = ',',
		},
		.collate_high = dbcs_byte_order_high,
		.upcase_high = dbcs_byte_order_high,
		.code_page = 936u,
		.complete = true,
	},
	{
		.dbcs = {cp936_ranges, (uint16_t)sizeof(cp936_ranges)},
		.format = {.country = 88u},
		.collate_high = NULL,
		.upcase_high = NULL,
		.code_page = 938u,
		.complete = false,
	},
};

static bool dbcs_table_is_valid(const struct dos_nls_dbcs_table *table)
{
	uint8_t previous_end = 0u;
	size_t index;

	if (table == NULL || table->ranges == NULL || table->length < 2u ||
	    table->length > DOS_NLS_DBCS_MAXIMUM_BYTES ||
	    (table->length & 1u) != 0u ||
	    table->ranges[table->length - 2u] != 0u ||
	    table->ranges[table->length - 1u] != 0u)
		return false;
	for (index = 0u; index + 2u < table->length; index += 2u) {
		uint8_t first = table->ranges[index];
		uint8_t last = table->ranges[index + 1u];

		if (first == 0u || first > last ||
		    (index != 0u && first <= previous_end))
			return false;
		previous_end = last;
	}
	return true;
}

bool dos_nls_validate_package(const struct dos_nls_package *package)
{
	return package != NULL && package->format.country != 0u &&
	       package->code_page != 0u && dbcs_table_is_valid(&package->dbcs) &&
	       (!package->complete || (package->collate_high != NULL &&
				 package->upcase_high != NULL));
}

size_t dos_nls_package_count(void)
{
	return ARRAY_SIZE(packages);
}

const struct dos_nls_package *dos_nls_package_at(size_t index)
{
	if (index >= ARRAY_SIZE(packages) ||
	    !dos_nls_validate_package(&packages[index]))
		return NULL;
	return &packages[index];
}

const struct dos_nls_package *dos_nls_find_package(uint16_t code_page)
{
	size_t index;

	for (index = 0u; index < sizeof(packages) / sizeof(packages[0]); ++index)
		if (packages[index].code_page == code_page &&
		    dos_nls_validate_package(&packages[index]))
			return &packages[index];
	return NULL;
}

bool dos_nls_get_dbcs_table(uint16_t code_page,
			    struct dos_nls_dbcs_table *table)
{
	const struct dos_nls_package *package;

	if (table == NULL)
		return false;
	package = dos_nls_find_package(code_page);
	if (package == NULL)
		return false;
	*table = package->dbcs;
	return true;
}

bool dos_nls_runtime_initialize(struct dos_nls_runtime *runtime,
				uint16_t system_code_page,
				uint16_t active_code_page)
{
	const struct dos_nls_package *package;
	const struct dos_nls_package *system_package;

	if (runtime == NULL)
		return false;
	package = dos_nls_find_package(active_code_page);
	system_package = dos_nls_find_package(system_code_page);
	if (package == NULL || !package->complete ||
	    system_package == NULL || !system_package->complete)
		return false;
	*runtime = (struct dos_nls_runtime){
	    .active = package,
	    .generation = 1u,
	    .system_code_page = system_code_page,
	    .initialized = true,
	};
	return true;
}

static const struct dos_nls_package *runtime_find_package(
	const struct dos_nls_runtime *runtime, uint16_t code_page)
{
	size_t index;

	if (runtime != NULL) {
		for (index = 0u; index < runtime->package_count; ++index)
			if (runtime->packages[index] != NULL &&
			    runtime->packages[index]->code_page == code_page)
				return runtime->packages[index];
	}
	return dos_nls_find_package(code_page);
}

bool dos_nls_runtime_publish_catalog(
	struct dos_nls_runtime *runtime,
	const struct dos_nls_package *const *new_packages,
	size_t package_count)
{
	const struct dos_nls_package *active = NULL;
	const struct dos_nls_package *system = NULL;
	size_t index;

	if (runtime == NULL || new_packages == NULL || !runtime->initialized ||
	    runtime->active == NULL || runtime->generation == 0xffffffffu ||
	    package_count == 0u || package_count > DOS_NLS_RUNTIME_PACKAGE_MAX)
		return false;
	for (index = 0u; index < package_count; ++index) {
		size_t previous;

		if (!dos_nls_validate_package(new_packages[index]) ||
		    !new_packages[index]->complete)
			return false;
		for (previous = 0u; previous < index; ++previous)
			if (new_packages[previous]->code_page ==
			    new_packages[index]->code_page)
				return false;
		if (new_packages[index]->code_page == runtime->active->code_page)
			active = new_packages[index];
		if (new_packages[index]->code_page == runtime->system_code_page)
			system = new_packages[index];
	}
	if (active == NULL || system == NULL)
		return false;
	for (index = 0u; index < package_count; ++index)
		runtime->packages[index] = new_packages[index];
	for (; index < DOS_NLS_RUNTIME_PACKAGE_MAX; ++index)
		runtime->packages[index] = NULL;
	runtime->package_count = (uint16_t)package_count;
	runtime->active = active;
	++runtime->generation;
	return true;
}

bool dos_nls_prepare_switch(const struct dos_nls_runtime *runtime,
			    uint16_t code_page,
			    struct dos_nls_switch *transaction)
{
	const struct dos_nls_package *package;

	if (runtime == NULL || transaction == NULL || !runtime->initialized ||
	    runtime->active == NULL || runtime->generation == 0xffffffffu)
		return false;
	package = runtime_find_package(runtime, code_page);
	if (package == NULL || !package->complete)
		return false;
	*transaction = (struct dos_nls_switch){
	    .target = package,
	    .expected_generation = runtime->generation,
	    .prepared = true,
	};
	return true;
}

bool dos_nls_commit_switch(struct dos_nls_runtime *runtime,
			   struct dos_nls_switch *transaction)
{
	if (runtime == NULL || transaction == NULL || !runtime->initialized ||
	    !transaction->prepared || transaction->target == NULL ||
	    !transaction->target->complete ||
	    runtime->generation != transaction->expected_generation)
		return false;
	if (runtime->active != transaction->target) {
		runtime->active = transaction->target;
		++runtime->generation;
	}
	dos_nls_abort_switch(transaction);
	return true;
}

void dos_nls_abort_switch(struct dos_nls_switch *transaction)
{
	if (transaction == NULL)
		return;
	*transaction = (struct dos_nls_switch){0};
}
