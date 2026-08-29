// SPDX-License-Identifier: GPL-2.0-only
/* Native dual-8259 backend tested through the generic IRQ dispatcher. */
#include "test_entry.h"
#include "x86_legacy_pic.h"

#define PIC_ID ((kernel_object_handle_t)0x4c45474143595049ull)
#define DISPATCH_ID ((kernel_object_handle_t)0x4c45474143594449ull)
#define ACTION7_ID ((kernel_object_handle_t)0x4c45474143594137ull)
#define ACTION15_ID ((kernel_object_handle_t)0x4c45474143413135ull)
#define WRITE_CAPACITY 64u
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
	if (event == NULL || event->controller_identity != PIC_ID)
		return X86_NATIVE_IRQ_ACTION_FAULT;
	if (context == ACTION7_ID && event->hardware_irq == 7u) {
		action7_count++;
		return X86_NATIVE_IRQ_ACTION_HANDLED;
	}
	if (context == ACTION15_ID && event->hardware_irq == 15u) {
		action15_count++;
		return X86_NATIVE_IRQ_ACTION_HANDLED;
	}
	return X86_NATIVE_IRQ_ACTION_FAULT;
}

static int run_legacy_pic_test(void)
{
	static struct x86_native_irq_dispatch dispatch;
	static struct x86_native_irq_line_slot line_slots[16];
	static struct x86_native_irq_action_slot action_slots[4];
	struct x86_native_irq_line_config lines[16];
	struct x86_native_irq_action_binding action7;
	struct x86_native_irq_action_binding action15;
	struct x86_legacy_pic_snapshot snapshot;
	uint32_t line_count = 0xa5a5a5a5u;
	struct x86_legacy_pic_config pic_config = {
		.controller_identity = PIC_ID,
		.dispatch_identity = DISPATCH_ID,
		.pit_input_quantum = 1193u,
		.vector_base = 0x20u,
		.present_irq_mask = (uint16_t)((1u << 0u) | (1u << 2u) |
					      (1u << 7u) | (1u << 15u)),
		.enabled_irq_mask = (uint16_t)((1u << 7u) | (1u << 15u)),
		.pit_reload = 1193u,
		.pit_rate_calibrated = 0u,
		.present = 1u,
		.presence_evidence =
			X86_LEGACY_PIC_EVIDENCE_PLATFORM_ASSIGNED,
		.reserved = {0u},
	};
	struct x86_native_irq_dispatch_config dispatch_config = {
		.identity = DISPATCH_ID,
		.controller_identity = PIC_ID,
		.controller_context = PIC_ID,
		.controller = {0},
	};
	struct x86_native_irq_action_config action_config = {
		.identity = ACTION7_ID,
		.context = ACTION7_ID,
		.hardware_irq = 7u,
		.shared = 0u,
		.reserved = {0u},
		.handler = action_handler,
	};
	size_t before;

	pic_config.controller_identity = KERNEL_OBJECT_HANDLE_INVALID;
	if (x86_legacy_pic_prepare(&pic_config, lines, ARRAY_SIZE(lines),
				   &line_count) !=
		    X86_LEGACY_PIC_INVALID_ARGUMENT ||
	    write_count != 0u || line_count != 0xa5a5a5a5u)
		return 1;
	pic_config.controller_identity = PIC_ID;
	if (x86_legacy_pic_prepare(&pic_config, lines, 3u, &line_count) !=
		    X86_LEGACY_PIC_CAPACITY_EXHAUSTED ||
	    write_count != 0u || line_count != 0xa5a5a5a5u ||
	    x86_legacy_pic_prepare(&pic_config, lines, ARRAY_SIZE(lines),
				   &line_count) !=
		    X86_LEGACY_PIC_OK ||
	    write_count != 13u || wait_count != 8u ||
	    !write_is(0u, 0x20u, 0x11u) ||
	    !write_is(1u, 0xa0u, 0x11u) ||
	    !write_is(2u, 0x21u, 0x20u) ||
	    !write_is(3u, 0xa1u, 0x28u) ||
	    !write_is(8u, 0xa1u, 0xffu) ||
	    !write_is(9u, 0x21u, 0xffu) ||
	    !write_is(10u, 0x43u, 0x36u) ||
	    !write_is(11u, 0x40u, 0xa9u) ||
	    !write_is(12u, 0x40u, 0x04u))
		return 2;
	if (line_count != 4u ||
	    lines[0].vector != 0x20u || lines[0].hardware_irq != 0u ||
	    lines[1].vector != 0x22u || lines[1].hardware_irq != 2u ||
	    lines[2].vector != 0x27u || lines[2].hardware_irq != 7u ||
	    lines[3].vector != 0x2fu || lines[3].hardware_irq != 15u)
		return 3;

	dispatch_config.controller = x86_legacy_pic_controller_ops();
	x86_native_irq_dispatch_construct(&dispatch);
	if (x86_native_irq_dispatch_initialize(
		    &dispatch, line_slots, ARRAY_SIZE(line_slots), action_slots,
		    ARRAY_SIZE(action_slots)) != X86_NATIVE_IRQ_OK ||
	    x86_native_irq_dispatch_prepare(&dispatch, &dispatch_config, lines,
					    line_count) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_register(&dispatch, &action_config, &action7) !=
		    X86_NATIVE_IRQ_OK)
		return 4;
	action_config.identity = ACTION15_ID;
	action_config.context = ACTION15_ID;
	action_config.hardware_irq = 15u;
	if (x86_native_irq_action_register(&dispatch, &action_config, &action15) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_dispatch_publish(&dispatch) != X86_NATIVE_IRQ_OK ||
	    write_count != 15u || !write_is(13u, 0xa1u, 0x7fu) ||
	    !write_is(14u, 0x21u, 0x7bu) ||
	    x86_legacy_pic_snapshot(PIC_ID, &snapshot) != X86_LEGACY_PIC_BUSY)
		return 5;

	before = write_count;
	set_isr(0u);
	if (x86_native_irq_dispatch_vector(&dispatch, 0x27u) !=
		    X86_NATIVE_IRQ_SPURIOUS ||
	    action7_count != 0u || write_count != before + 1u ||
	    !write_is(before, 0x20u, 0x0bu))
		return 6;
	before = write_count;
	set_isr(0x80u);
	if (x86_native_irq_dispatch_vector(&dispatch, 0x27u) !=
		    X86_NATIVE_IRQ_OK ||
	    action7_count != 1u || write_count != before + 2u ||
	    !write_is(before, 0x20u, 0x0bu) ||
	    !write_is(before + 1u, 0x20u, 0x20u))
		return 7;
	before = write_count;
	set_isr(0u);
	if (x86_native_irq_dispatch_vector(&dispatch, 0x2fu) !=
		    X86_NATIVE_IRQ_SPURIOUS ||
	    action15_count != 0u || write_count != before + 2u ||
	    !write_is(before, 0xa0u, 0x0bu) ||
	    !write_is(before + 1u, 0x20u, 0x20u))
		return 8;
	before = write_count;
	set_isr(0x80u);
	if (x86_native_irq_dispatch_vector(&dispatch, 0x2fu) !=
		    X86_NATIVE_IRQ_OK ||
	    action15_count != 1u || write_count != before + 3u ||
	    !write_is(before, 0xa0u, 0x0bu) ||
	    !write_is(before + 1u, 0xa0u, 0x20u) ||
	    !write_is(before + 2u, 0x20u, 0x20u))
		return 9;

	before = write_count;
	if (x86_native_irq_dispatch_quiesce(&dispatch, DISPATCH_ID) !=
		    X86_NATIVE_IRQ_OK ||
	    write_count != before + 2u || !write_is(before, 0xa1u, 0xffu) ||
	    !write_is(before + 1u, 0x21u, 0xffu) ||
	    x86_legacy_pic_snapshot(PIC_ID, &snapshot) != X86_LEGACY_PIC_OK ||
	    snapshot.phase != X86_LEGACY_PIC_QUIESCED ||
	    x86_legacy_pic_set_enabled_irqs(PIC_ID, DISPATCH_ID, 1u) !=
		    X86_LEGACY_PIC_OK)
		return 10;
	if (x86_native_irq_action_quiesce(&dispatch, &action15) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_unregister(&dispatch, &action15) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_quiesce(&dispatch, &action7) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_action_unregister(&dispatch, &action7) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_native_irq_dispatch_retire(&dispatch, DISPATCH_ID) !=
		    X86_NATIVE_IRQ_OK ||
	    x86_legacy_pic_retire(PIC_ID, DISPATCH_ID) != X86_LEGACY_PIC_OK)
		return 11;
	return 0;
}

DOSC32_TEST_ENTRY(run_legacy_pic_test)
