// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe lifecycle and dispatch tests for the native legacy IRQ owner. */
#include "test_entry.h"
#include "x86_legacy_irq.h"

#define SOURCE_ID ((kernel_object_handle_t)0x4e41544956455352ull)
#define OTHER_ID ((kernel_object_handle_t)0x4e41544956454f54ull)
#define PIC_ID ((kernel_object_handle_t)0x4e41544956455049ull)
#define DISPATCH_ID ((kernel_object_handle_t)0x4e41544956454449ull)
#define ACTION0_ID ((kernel_object_handle_t)0x4e41544956454130ull)
#define ACTION7_ID ((kernel_object_handle_t)0x4e41544956454137ull)
#define ACTION15_ID ((kernel_object_handle_t)0x4e41544956413135ull)
#define WRITE_CAPACITY 96u
#define READ_CAPACITY 8u

struct port_write {
	uint16_t port;
	uint8_t value;
	uint8_t reserved;
};

static struct port_write writes[WRITE_CAPACITY];
static uint8_t reads[READ_CAPACITY];
static size_t write_count;
static size_t read_count;
static size_t read_index;
static size_t wait_count;
static uint64_t action0_count;
static uint64_t action7_count;
static uint64_t action15_count;

uint8_t x86_legacy_pic_test_inb(uint16_t port);
void x86_legacy_pic_test_outb(uint16_t port, uint8_t value);
void x86_legacy_pic_test_io_wait(void);

uint8_t x86_legacy_pic_test_inb(uint16_t port)
{
	if ((port != 0x20u && port != 0xa0u) || read_index >= read_count)
		return 0u;
	return reads[read_index++];
}

void x86_legacy_pic_test_outb(uint16_t port, uint8_t value)
{
	if (write_count < ARRAY_SIZE(writes)) {
		writes[write_count].port = port;
		writes[write_count].value = value;
		write_count++;
	}
}

void x86_legacy_pic_test_io_wait(void)
{
	wait_count++;
}

static bool write_is(size_t index, uint16_t port, uint8_t value)
{
	return index < write_count && writes[index].port == port &&
	       writes[index].value == value;
}

static void set_isr(uint8_t value)
{
	reads[0] = value;
	read_count = 1u;
	read_index = 0u;
}

static enum x86_native_irq_action_result action_handler(
	kernel_object_handle_t context, const struct x86_native_irq_event *event)
{
	if (event == NULL || event->controller_identity != PIC_ID ||
	    event->controller_generation == 0u)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	if (context == ACTION0_ID && event->hardware_irq == 0u)
		action0_count++;
	else if (context == ACTION7_ID && event->hardware_irq == 7u)
		action7_count++;
	else if (context == ACTION15_ID && event->hardware_irq == 15u)
		action15_count++;
	else
		return X86_NATIVE_IRQ_ACTION_FAULT;
	return X86_NATIVE_IRQ_ACTION_HANDLED;
}

static int run_tests(void)
{
	struct x86_legacy_irq_config config = {
		.source_identity = SOURCE_ID,
		.controller_identity = PIC_ID,
		.dispatch_identity = DISPATCH_ID,
		.pit_input_quantum = 1193u,
		.vector_base = 0x20u,
		.present_irq_mask = (uint16_t)((1u << 0u) | (1u << 2u) |
					      (1u << 7u) | (1u << 15u)),
		.enabled_irq_mask = (uint16_t)((1u << 0u) | (1u << 7u) |
					      (1u << 15u)),
		.pit_reload = 1193u,
		.pit_rate_calibrated = 0u,
		.present = 1u,
		.presence_evidence =
			X86_LEGACY_PIC_EVIDENCE_PLATFORM_ASSIGNED,
		.reserved = {0u},
	};
	struct x86_native_irq_action_config action = {
		.identity = ACTION0_ID,
		.context = ACTION0_ID,
		.hardware_irq = 0u,
		.shared = 0u,
		.reserved = {0u},
		.handler = action_handler,
	};
	struct x86_native_irq_action_binding binding0;
	struct x86_native_irq_action_binding binding7;
	struct x86_native_irq_action_binding binding15;
	struct x86_legacy_irq_source_info info = {0};
	struct x86_legacy_irq_snapshot snapshot = {0};
	size_t before;

	config.source_identity = KERNEL_OBJECT_HANDLE_INVALID;
	if (x86_legacy_irq_prepare(&config) !=
		    X86_LEGACY_IRQ_INVALID_ARGUMENT ||
	    write_count != 0u || x86_legacy_irq_is_initialized())
		return 1;
	config.source_identity = SOURCE_ID;
	config.present_irq_mask = (uint16_t)((1u << 0u) | (1u << 15u));
	if (x86_legacy_irq_prepare(&config) !=
		    X86_LEGACY_IRQ_INVALID_ARGUMENT ||
	    write_count != 0u)
		return 2;
	config.present_irq_mask = (uint16_t)((1u << 0u) | (1u << 2u) |
					     (1u << 7u) | (1u << 15u));
	if (x86_legacy_irq_prepare(&config) != X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_is_initialized() || write_count != 13u ||
	    wait_count != 8u || !write_is(0u, 0x20u, 0x11u) ||
	    !write_is(1u, 0xa0u, 0x11u) ||
	    !write_is(8u, 0xa1u, 0xffu) ||
	    !write_is(9u, 0x21u, 0xffu) ||
	    !write_is(10u, 0x43u, 0x36u) ||
	    !write_is(11u, 0x40u, 0xa9u) ||
	    !write_is(12u, 0x40u, 0x04u))
		return 3;
	if (x86_legacy_irq_source_info(OTHER_ID, &info) !=
		    X86_LEGACY_IRQ_IDENTITY_MISMATCH ||
	    x86_legacy_irq_source_info(SOURCE_ID, &info) !=
		    X86_LEGACY_IRQ_OK ||
	    info.source_identity != SOURCE_ID || info.generation != 1u ||
	    info.pit_input_quantum != 1193u ||
	    info.capabilities != X86_LEGACY_IRQ_SOURCE_PIT_CLOCK ||
	    info.phase != X86_LEGACY_IRQ_PREPARED)
		return 4;
	/* Enabled lines cannot be published until each action is present. */
	if (x86_legacy_irq_publish(SOURCE_ID) !=
		    X86_LEGACY_IRQ_INVALID_STATE ||
	    write_count != 13u)
		return 5;
	if (x86_legacy_irq_action_register(SOURCE_ID, &action, &binding0) !=
		    X86_LEGACY_IRQ_OK)
		return 14;
	if (x86_legacy_irq_publish(SOURCE_ID) !=
		    X86_LEGACY_IRQ_INVALID_STATE)
		return 15;
	action.identity = ACTION7_ID;
	action.context = ACTION7_ID;
	action.hardware_irq = 7u;
	if (x86_legacy_irq_action_register(SOURCE_ID, &action, &binding7) !=
		    X86_LEGACY_IRQ_OK)
		return 6;
	action.identity = ACTION15_ID;
	action.context = ACTION15_ID;
	action.hardware_irq = 15u;
	if (x86_legacy_irq_action_register(SOURCE_ID, &action, &binding15) !=
		    X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_publish(SOURCE_ID) != X86_LEGACY_IRQ_OK ||
	    !x86_legacy_irq_is_initialized() || write_count != 15u ||
	    !write_is(13u, 0xa1u, 0x7fu) ||
	    !write_is(14u, 0x21u, 0x7au) ||
	    x86_legacy_irq_snapshot(SOURCE_ID, &snapshot) !=
		    X86_LEGACY_IRQ_BUSY)
		return 7;

	before = write_count;
	if (x86_legacy_irq_dispatch_vector(0x20u) != X86_LEGACY_IRQ_OK ||
	    action0_count != 1u || write_count != before + 1u ||
	    !write_is(before, 0x20u, 0x20u) ||
	    x86_legacy_irq_dispatch_vector(0x21u) !=
		    X86_LEGACY_IRQ_NOT_MAPPED ||
	    write_count != before + 1u)
		return 8;
	before = write_count;
	set_isr(0u);
	if (x86_legacy_irq_dispatch_vector(0x27u) !=
		    X86_LEGACY_IRQ_SPURIOUS ||
	    action7_count != 0u || write_count != before + 1u ||
	    !write_is(before, 0x20u, 0x0bu))
		return 9;
	before = write_count;
	set_isr(0u);
	if (x86_legacy_irq_dispatch_vector(0x2fu) !=
		    X86_LEGACY_IRQ_SPURIOUS ||
	    action15_count != 0u || write_count != before + 2u ||
	    !write_is(before, 0xa0u, 0x0bu) ||
	    !write_is(before + 1u, 0x20u, 0x20u))
		return 10;
	before = write_count;
	set_isr(0x80u);
	if (x86_legacy_irq_dispatch_vector(0x2fu) != X86_LEGACY_IRQ_OK ||
	    action15_count != 1u || write_count != before + 3u ||
	    !write_is(before, 0xa0u, 0x0bu) ||
	    !write_is(before + 1u, 0xa0u, 0x20u) ||
	    !write_is(before + 2u, 0x20u, 0x20u))
		return 11;

	before = write_count;
	if (x86_legacy_irq_quiesce(OTHER_ID) !=
		    X86_LEGACY_IRQ_IDENTITY_MISMATCH ||
	    x86_legacy_irq_quiesce(SOURCE_ID) != X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_is_initialized() || write_count != before + 2u ||
	    !write_is(before, 0xa1u, 0xffu) ||
	    !write_is(before + 1u, 0x21u, 0xffu) ||
	    x86_legacy_irq_snapshot(SOURCE_ID, &snapshot) !=
		    X86_LEGACY_IRQ_OK ||
	    snapshot.line_count != 4u || snapshot.action_count != 3u ||
	    snapshot.handled_count != 2u || snapshot.spurious_count != 2u ||
	    snapshot.unhandled_count != 0u || snapshot.fault_count != 0u)
		return 12;
	if (x86_legacy_irq_action_quiesce(SOURCE_ID, &binding15) !=
		    X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_action_unregister(SOURCE_ID, &binding15) !=
		    X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_action_quiesce(SOURCE_ID, &binding7) !=
		    X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_action_unregister(SOURCE_ID, &binding7) !=
		    X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_action_quiesce(SOURCE_ID, &binding0) !=
		    X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_action_unregister(SOURCE_ID, &binding0) !=
		    X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_retire(SOURCE_ID) != X86_LEGACY_IRQ_OK ||
	    x86_legacy_irq_is_initialized())
		return 13;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)
