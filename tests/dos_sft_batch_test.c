// SPDX-License-Identifier: GPL-2.0-only
/* Freestanding DOS-contract tests for EXEC JFT/SFT inheritance batches. */
#include "dos_sft_batch.h"
#include "test_entry.h"

#define TEST_CONTEXT ((kernel_object_handle_t)0x5346544241544348ull)
#define WRONG_CONTEXT ((kernel_object_handle_t)0x424144434f4e5445ull)
#define TEST_ADAPTER_IDENTITY ((kernel_object_handle_t)0x5346544144415054ull)
#define WRONG_ADAPTER_IDENTITY ((kernel_object_handle_t)0x4241444144415054ull)
#define TEST_SFTS 32u
#define EVENT_CAPACITY 192u
#define REFERENCE_BASE ((dos_sft_reference_handle_t)0x5300000000000000ull)

enum event_type {
	EVENT_LOOKUP = 1,
	EVENT_DEVICE_OPEN,
	EVENT_REFERENCE_ACQUIRE,
	EVENT_REFERENCE_RELEASE,
	EVENT_DEVICE_CLOSE
};

struct fixture_sft {
	dos_sft_reference_handle_t reference_handle;
	uint16_t flags;
	uint16_t mode;
	uint16_t open_count;
	uint16_t reference_count;
	bool valid;
};

struct event_record {
	uint8_t type;
	uint8_t sfn;
};

static struct fixture_sft fixture_sfts[TEST_SFTS];
static struct event_record events[EVENT_CAPACITY];
static uint32_t event_count;
static uint32_t lookup_calls;
static uint32_t device_open_calls;
static uint32_t acquire_calls;
static uint32_t release_calls;
static uint32_t device_close_calls;
static uint32_t fail_lookup_call;
static uint32_t fail_device_open_call;
static uint32_t fail_acquire_call;
static uint32_t fail_release_call;
static uint32_t fail_device_close_call;

static uint8_t reference_sfn(dos_sft_reference_handle_t reference_handle)
{
	return (uint8_t)(reference_handle & 0xffu);
}

static void record_event(uint8_t type, uint8_t sfn)
{
	if (event_count < EVENT_CAPACITY) {
		events[event_count].type = type;
		events[event_count].sfn = sfn;
	}
	++event_count;
}

static void reset_fixture(void)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(fixture_sfts); ++index) {
		fixture_sfts[index].reference_handle =
		    REFERENCE_BASE | (dos_sft_reference_handle_t)index;
		fixture_sfts[index].flags = 0u;
		fixture_sfts[index].mode = 0u;
		fixture_sfts[index].open_count = 0u;
		fixture_sfts[index].reference_count = 0u;
		fixture_sfts[index].valid = false;
	}
	for (index = 0u; index < ARRAY_SIZE(events); ++index) {
		events[index].type = 0u;
		events[index].sfn = 0u;
	}
	event_count = 0u;
	lookup_calls = 0u;
	device_open_calls = 0u;
	acquire_calls = 0u;
	release_calls = 0u;
	device_close_calls = 0u;
	fail_lookup_call = 0u;
	fail_device_open_call = 0u;
	fail_acquire_call = 0u;
	fail_release_call = 0u;
	fail_device_close_call = 0u;
}

static void reset_jft(struct dos_sft_jft20 *jft)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(jft->entries); ++index)
		jft->entries[index] = DOS_JFT_ENTRY_UNUSED;
}

static void make_valid(uint8_t sfn, uint16_t flags, uint16_t mode)
{
	fixture_sfts[sfn].valid = true;
	fixture_sfts[sfn].flags = flags;
	fixture_sfts[sfn].mode = mode;
}

static enum dos_sft_adapter_status test_lookup(kernel_object_handle_t context,
					       uint8_t sfn,
					       struct dos_sft_view *view)
{
	++lookup_calls;
	record_event(EVENT_LOOKUP, sfn);
	if (context != TEST_CONTEXT || view == NULL || sfn >= TEST_SFTS ||
	    lookup_calls == fail_lookup_call)
		return DOS_SFT_ADAPTER_FAULT;
	if (!fixture_sfts[sfn].valid)
		return DOS_SFT_ADAPTER_INVALID_SFT;
	view->reference_handle = fixture_sfts[sfn].reference_handle;
	view->flags = fixture_sfts[sfn].flags;
	view->mode = fixture_sfts[sfn].mode;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status
test_device_open(kernel_object_handle_t context,
		 dos_sft_reference_handle_t reference_handle)
{
	uint8_t sfn = reference_sfn(reference_handle);

	++device_open_calls;
	record_event(EVENT_DEVICE_OPEN, sfn);
	if (context != TEST_CONTEXT || sfn >= TEST_SFTS ||
	    reference_handle != fixture_sfts[sfn].reference_handle ||
	    device_open_calls == fail_device_open_call)
		return DOS_SFT_ADAPTER_FAULT;
	++fixture_sfts[sfn].open_count;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status
test_reference_acquire(kernel_object_handle_t context,
		       dos_sft_reference_handle_t reference_handle)
{
	uint8_t sfn = reference_sfn(reference_handle);

	++acquire_calls;
	record_event(EVENT_REFERENCE_ACQUIRE, sfn);
	if (context != TEST_CONTEXT || sfn >= TEST_SFTS ||
	    reference_handle != fixture_sfts[sfn].reference_handle ||
	    acquire_calls == fail_acquire_call)
		return DOS_SFT_ADAPTER_FAULT;
	++fixture_sfts[sfn].reference_count;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status
test_reference_release(kernel_object_handle_t context,
		       dos_sft_reference_handle_t reference_handle)
{
	uint8_t sfn = reference_sfn(reference_handle);

	++release_calls;
	record_event(EVENT_REFERENCE_RELEASE, sfn);
	if (context != TEST_CONTEXT || sfn >= TEST_SFTS ||
	    reference_handle != fixture_sfts[sfn].reference_handle ||
	    fixture_sfts[sfn].reference_count == 0u ||
	    release_calls == fail_release_call)
		return DOS_SFT_ADAPTER_FAULT;
	--fixture_sfts[sfn].reference_count;
	return DOS_SFT_ADAPTER_OK;
}

static enum dos_sft_adapter_status
test_device_close(kernel_object_handle_t context,
		  dos_sft_reference_handle_t reference_handle)
{
	uint8_t sfn = reference_sfn(reference_handle);

	++device_close_calls;
	record_event(EVENT_DEVICE_CLOSE, sfn);
	if (context != TEST_CONTEXT || sfn >= TEST_SFTS ||
	    reference_handle != fixture_sfts[sfn].reference_handle ||
	    fixture_sfts[sfn].open_count == 0u ||
	    device_close_calls == fail_device_close_call)
		return DOS_SFT_ADAPTER_FAULT;
	--fixture_sfts[sfn].open_count;
	return DOS_SFT_ADAPTER_OK;
}

static const struct dos_sft_batch_ops test_ops = {
    .identity = TEST_ADAPTER_IDENTITY,
    .lookup = test_lookup,
    .device_open = test_device_open,
    .reference_acquire = test_reference_acquire,
    .reference_release = test_reference_release,
    .device_close = test_device_close,
};

static const struct dos_sft_batch_ops wrong_identity_ops = {
    .identity = WRONG_ADAPTER_IDENTITY,
    .lookup = test_lookup,
    .device_open = test_device_open,
    .reference_acquire = test_reference_acquire,
    .reference_release = test_reference_release,
    .device_close = test_device_close,
};

static bool jft_is_unused_except(const struct dos_sft_jft20 *jft, size_t first,
				 uint8_t first_sfn, size_t second,
				 uint8_t second_sfn)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(jft->entries); ++index) {
		uint8_t expected = DOS_JFT_ENTRY_UNUSED;

		if (index == first)
			expected = first_sfn;
		if (index == second)
			expected = second_sfn;
		if (jft->entries[index] != expected)
			return false;
	}
	return true;
}

static bool jfts_are_equal(const struct dos_sft_jft20 *left,
			   const struct dos_sft_jft20 *right)
{
	size_t index;

	for (index = 0u; index < ARRAY_SIZE(left->entries); ++index) {
		if (left->entries[index] != right->entries[index])
			return false;
	}
	return true;
}

static bool event_is(size_t index, uint8_t type, uint8_t sfn)
{
	return index < event_count && events[index].type == type &&
	       events[index].sfn == sfn;
}

static int test_source_filtering(void)
{
	struct dos_sft_jft20 parent;
	struct dos_sft_jft20 child;
	dos_sft_batch_handle_t handle;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[1] = 1u; /* invalid SFT */
	parent.entries[2] = 2u; /* sf_no_inherit */
	parent.entries[3] = 3u; /* sharing_net_fcb */
	parent.entries[4] = 4u; /* local inheritable */
	parent.entries[5] = 5u; /* ordinary network SFT */
	parent.entries[6] = 6u; /* both exclusion forms */
	make_valid(2u, DOS_SFT_FLAG_NO_INHERIT, 0u);
	make_valid(3u, 0u, DOS_SFT_SHARING_NETWORK_FCB);
	make_valid(4u, 0u, 0u);
	make_valid(5u, DOS_SFT_FLAG_IS_NETWORK, 0u);
	make_valid(6u, DOS_SFT_FLAG_NO_INHERIT, DOS_SFT_SHARING_NETWORK_FCB);

	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &handle) !=
		DOS_SFT_BATCH_OK ||
	    handle == DOS_SFT_BATCH_HANDLE_INVALID || lookup_calls != 6u ||
	    device_open_calls != 1u || acquire_calls != 2u ||
	    fixture_sfts[4].open_count != 1u ||
	    fixture_sfts[4].reference_count != 1u ||
	    fixture_sfts[5].open_count != 0u ||
	    fixture_sfts[5].reference_count != 1u)
		return 1;
	reset_jft(&child);
	child.entries[0] = 0x5au;
	if (dos_sft_batch_copy_child_jft(handle, &child) != DOS_SFT_BATCH_OK ||
	    !jft_is_unused_except(&child, 4u, 4u, 5u, 5u))
		return 2;
	if (dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    fixture_sfts[4].open_count != 0u ||
	    fixture_sfts[4].reference_count != 0u ||
	    fixture_sfts[5].reference_count != 0u ||
	    dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    dos_sft_batch_retire(handle) != DOS_SFT_BATCH_OK)
		return 3;
	return 0;
}

static int test_twenty_entry_bound(void)
{
	struct dos_sft_jft20 parent;
	struct dos_sft_jft20 child;
	dos_sft_batch_handle_t handle = 0xa5a5u;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[DOS_SFT_BATCH_JFT_ENTRIES - 1u] = 9u;
	make_valid(9u, 0u, 0u);
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &handle) !=
		DOS_SFT_BATCH_OK ||
	    lookup_calls != 1u || acquire_calls != 1u ||
	    dos_sft_batch_copy_child_jft(handle, &child) != DOS_SFT_BATCH_OK ||
	    child.entries[DOS_SFT_BATCH_JFT_ENTRIES - 1u] != 9u)
		return 1;
	if (dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    dos_sft_batch_retire(handle) != DOS_SFT_BATCH_OK)
		return 2;
	return 0;
}

static int test_commit_preflight_is_pure(void)
{
	struct dos_sft_jft20 parent;
	struct dos_sft_jft20 before;
	struct dos_sft_jft20 after;
	dos_sft_batch_handle_t handle;
	enum dos_sft_batch_state state = DOS_SFT_BATCH_STATE_ABORTED;
	uint32_t callbacks_before_preflight;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[2] = 20u;
	parent.entries[7] = 21u;
	make_valid(20u, 0u, 0u);
	make_valid(21u, DOS_SFT_FLAG_IS_NETWORK, 0u);
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &handle) !=
		DOS_SFT_BATCH_OK ||
	    dos_sft_batch_copy_child_jft(handle, &before) != DOS_SFT_BATCH_OK)
		return 1;
	callbacks_before_preflight = event_count;
	reset_jft(&after);
	if (dos_sft_batch_preflight_commit(handle) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_preflight_commit(handle) != DOS_SFT_BATCH_OK ||
	    event_count != callbacks_before_preflight ||
	    dos_sft_batch_get_state(handle, &state) != DOS_SFT_BATCH_OK ||
	    state != DOS_SFT_BATCH_STATE_PREPARED ||
	    dos_sft_batch_copy_child_jft(handle, &after) != DOS_SFT_BATCH_OK ||
	    !jfts_are_equal(&before, &after) ||
	    event_count != callbacks_before_preflight)
		return 2;
	if (dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    dos_sft_batch_preflight_commit(handle) !=
		DOS_SFT_BATCH_INVALID_STATE ||
	    dos_sft_batch_retire(handle) != DOS_SFT_BATCH_OK)
		return 3;
	return 0;
}

static int test_duplicate_sfn_has_distinct_references(void)
{
	struct dos_sft_jft20 parent;
	dos_sft_batch_handle_t handle;
	uint32_t calls_before_second_abort;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[0] = 7u;
	parent.entries[1] = 7u;
	make_valid(7u, 0u, 0u);
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &handle) !=
		DOS_SFT_BATCH_OK ||
	    lookup_calls != 2u || device_open_calls != 2u ||
	    acquire_calls != 2u || fixture_sfts[7].open_count != 2u ||
	    fixture_sfts[7].reference_count != 2u)
		return 1;
	if (dos_sft_batch_abort(handle, &test_ops, WRONG_CONTEXT) !=
		DOS_SFT_BATCH_INVALID_ARGUMENT ||
	    release_calls != 0u || device_close_calls != 0u)
		return 2;
	if (dos_sft_batch_abort(handle, &wrong_identity_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_INVALID_ARGUMENT ||
	    release_calls != 0u || device_close_calls != 0u)
		return 3;
	if (dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    release_calls != 2u || device_close_calls != 2u ||
	    fixture_sfts[7].open_count != 0u ||
	    fixture_sfts[7].reference_count != 0u)
		return 4;
	calls_before_second_abort = event_count;
	if (dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    event_count != calls_before_second_abort ||
	    dos_sft_batch_retire(handle) != DOS_SFT_BATCH_OK)
		return 5;
	return 0;
}

static int test_failed_prepare_unwinds_in_reverse(void)
{
	struct dos_sft_jft20 parent;
	dos_sft_batch_handle_t handle = 0xa5a5u;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[0] = 10u; /* local */
	parent.entries[1] = 11u; /* network */
	parent.entries[2] = 12u; /* local, acquire fails */
	make_valid(10u, 0u, 0u);
	make_valid(11u, DOS_SFT_FLAG_IS_NETWORK, 0u);
	make_valid(12u, 0u, 0u);
	fail_acquire_call = 3u;
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &handle) !=
		DOS_SFT_BATCH_ADAPTER_FAULT ||
	    handle != DOS_SFT_BATCH_HANDLE_INVALID || event_count != 12u)
		return 1;
	/* Failing C: close C; then release B; then release/close A. */
	if (!event_is(8u, EVENT_DEVICE_CLOSE, 12u) ||
	    !event_is(9u, EVENT_REFERENCE_RELEASE, 11u) ||
	    !event_is(10u, EVENT_REFERENCE_RELEASE, 10u) ||
	    !event_is(11u, EVENT_DEVICE_CLOSE, 10u))
		return 2;
	if (fixture_sfts[10].open_count != 0u ||
	    fixture_sfts[10].reference_count != 0u ||
	    fixture_sfts[11].reference_count != 0u ||
	    fixture_sfts[12].open_count != 0u ||
	    fixture_sfts[12].reference_count != 0u)
		return 3;
	return 0;
}

static int test_undo_failure_is_sticky_poison(void)
{
	struct dos_sft_jft20 parent;
	struct dos_sft_jft20 child;
	dos_sft_batch_handle_t handle;
	enum dos_sft_batch_state state = DOS_SFT_BATCH_STATE_PREPARED;
	uint32_t events_after_prepare;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[0] = 14u;
	parent.entries[1] = 15u;
	make_valid(14u, 0u, 0u);
	make_valid(15u, 0u, 0u);
	fail_acquire_call = 2u;
	fail_device_close_call = 1u; /* closing partial entry 15 fails */
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &handle) !=
		DOS_SFT_BATCH_POISONED ||
	    handle == DOS_SFT_BATCH_HANDLE_INVALID ||
	    dos_sft_batch_get_state(handle, &state) != DOS_SFT_BATCH_OK ||
	    state != DOS_SFT_BATCH_STATE_POISONED ||
	    fixture_sfts[15].open_count != 1u ||
	    fixture_sfts[14].open_count != 0u ||
	    fixture_sfts[14].reference_count != 0u)
		return 1;
	events_after_prepare = event_count;
	reset_jft(&child);
	child.entries[0] = 0x3cu;
	if (dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_POISONED ||
	    dos_sft_batch_preflight_commit(handle) != DOS_SFT_BATCH_POISONED ||
	    dos_sft_batch_commit(handle) != DOS_SFT_BATCH_POISONED ||
	    dos_sft_batch_copy_child_jft(handle, &child) !=
		DOS_SFT_BATCH_POISONED ||
	    child.entries[0] != 0x3cu ||
	    dos_sft_batch_retire(handle) != DOS_SFT_BATCH_POISONED ||
	    event_count != events_after_prepare)
		return 2;
	return 0;
}

static int test_release_failure_preserves_dependent_open(void)
{
	struct dos_sft_jft20 parent;
	dos_sft_batch_handle_t handle;
	enum dos_sft_batch_state state = DOS_SFT_BATCH_STATE_PREPARED;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[0] = 18u;
	parent.entries[1] = 19u;
	make_valid(18u, 0u, 0u);
	make_valid(19u, 0u, 0u);
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &handle) !=
	    DOS_SFT_BATCH_OK)
		return 1;
	fail_release_call = 1u; /* Reverse entry 19 cannot drop its ref. */
	if (dos_sft_batch_abort(handle, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_POISONED ||
	    release_calls != 2u || device_close_calls != 1u ||
	    fixture_sfts[19].reference_count != 1u ||
	    fixture_sfts[19].open_count != 1u ||
	    fixture_sfts[18].reference_count != 0u ||
	    fixture_sfts[18].open_count != 0u ||
	    !event_is(6u, EVENT_REFERENCE_RELEASE, 19u) ||
	    !event_is(7u, EVENT_REFERENCE_RELEASE, 18u) ||
	    !event_is(8u, EVENT_DEVICE_CLOSE, 18u))
		return 2;
	if (dos_sft_batch_get_state(handle, &state) != DOS_SFT_BATCH_OK ||
	    state != DOS_SFT_BATCH_STATE_POISONED)
		return 3;
	return 0;
}

static int test_commit_abort_and_generation_aba(void)
{
	struct dos_sft_jft20 parent;
	dos_sft_batch_handle_t committed;
	dos_sft_batch_handle_t first;
	dos_sft_batch_handle_t second;
	enum dos_sft_batch_state state;
	uint32_t callbacks_before_abort;

	reset_fixture();
	reset_jft(&parent);
	parent.entries[0] = 16u;
	make_valid(16u, 0u, 0u);
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent,
				  &committed) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_preflight_commit(committed) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_commit(committed) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_preflight_commit(committed) !=
		DOS_SFT_BATCH_INVALID_STATE ||
	    dos_sft_batch_commit(committed) != DOS_SFT_BATCH_OK)
		return 1;
	callbacks_before_abort = event_count;
	if (dos_sft_batch_abort(committed, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_ALREADY_COMMITTED ||
	    event_count != callbacks_before_abort ||
	    fixture_sfts[16].open_count != 1u ||
	    fixture_sfts[16].reference_count != 1u ||
	    dos_sft_batch_retire(committed) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_preflight_commit(committed) !=
		DOS_SFT_BATCH_STALE_HANDLE ||
	    dos_sft_batch_commit(committed) != DOS_SFT_BATCH_STALE_HANDLE)
		return 2;

	/* The committed references now belong to the simulated child. */
	reset_fixture();
	reset_jft(&parent);
	parent.entries[0] = 17u;
	make_valid(17u, DOS_SFT_FLAG_IS_NETWORK, 0u);
	if (dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &first) !=
		DOS_SFT_BATCH_OK ||
	    dos_sft_batch_abort(first, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    dos_sft_batch_retire(first) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_prepare(&test_ops, TEST_CONTEXT, &parent, &second) !=
		DOS_SFT_BATCH_OK ||
	    first == second)
		return 3;
	state = DOS_SFT_BATCH_STATE_ABORTED;
	if (dos_sft_batch_get_state(first, &state) !=
		DOS_SFT_BATCH_STALE_HANDLE ||
	    state != DOS_SFT_BATCH_STATE_ABORTED ||
	    dos_sft_batch_preflight_commit(first) !=
		DOS_SFT_BATCH_STALE_HANDLE ||
	    dos_sft_batch_commit(first) != DOS_SFT_BATCH_STALE_HANDLE ||
	    dos_sft_batch_preflight_commit(second) != DOS_SFT_BATCH_OK ||
	    dos_sft_batch_abort(second, &test_ops, TEST_CONTEXT) !=
		DOS_SFT_BATCH_OK ||
	    dos_sft_batch_preflight_commit(second) !=
		DOS_SFT_BATCH_INVALID_STATE ||
	    dos_sft_batch_retire(second) != DOS_SFT_BATCH_OK)
		return 4;
	return 0;
}

static int run_tests(void)
{
	int result;

	result = test_source_filtering();
	if (result != 0)
		return 10 + result;
	result = test_twenty_entry_bound();
	if (result != 0)
		return 20 + result;
	result = test_commit_preflight_is_pure();
	if (result != 0)
		return 25 + result;
	result = test_duplicate_sfn_has_distinct_references();
	if (result != 0)
		return 30 + result;
	result = test_failed_prepare_unwinds_in_reverse();
	if (result != 0)
		return 40 + result;
	result = test_undo_failure_is_sticky_poison();
	if (result != 0)
		return 50 + result;
	result = test_release_failure_preserves_dependent_open();
	if (result != 0)
		return 60 + result;
	result = test_commit_abort_and_generation_aba();
	if (result != 0)
		return 70 + result;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)
