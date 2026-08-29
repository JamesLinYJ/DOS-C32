// SPDX-License-Identifier: GPL-2.0-only
/* Host-safe register-sequence tests for the isolated legacy chipset model. */
#include "test_entry.h"
#include "x86_legacy_bios.h"
#include "x86_legacy_chipset.h"

#define REGISTRY_IDENTITY ((kernel_object_handle_t)0x5245474953545259ull)
#define CONTEXT_IDENTITY ((kernel_object_handle_t)0x4348495053455431ull)
#define OWNER_IDENTITY ((kernel_object_handle_t)0x434849504f574e31ull)
#define SOURCE_IDENTITY ((kernel_object_handle_t)0x495251534f555243ull)
#define OTHER_SOURCE_IDENTITY                                           \
	((kernel_object_handle_t)0x495251534f555232ull)

static bool write8(uint16_t port, uint8_t value)
{
	return x86_io_resource_write(KERNEL_OBJECT_HANDLE_INVALID, port,
				     DOS_IO_WIDTH_8, value) ==
	       X86_IO_RESOURCE_OK;
}

static bool read8(uint16_t port, uint8_t expected)
{
	uint32_t value = 0x12345678u;

	return x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, port,
				    DOS_IO_WIDTH_8, &value) ==
		       X86_IO_RESOURCE_OK &&
	       value == expected;
}

static int test_lifecycle_and_register(void)
{
	struct x86_legacy_chipset_config config;
	const struct x86_legacy_bios_snapshot platform = {
		.rtc_year = 2000u,
		.rtc_weekday = 7u,
		.rtc_day = 1u,
		.rtc_month = 1u,
		.rtc_valid = 1u,
	};
	const struct x86_legacy_chipset_source_config source_config = {
		.pit_input_quantum = 1193u,
		.capabilities = X86_LEGACY_CHIPSET_SOURCE_CAPABILITIES,
		.pit_rate_calibrated = 0u,
		.reserved = {0u},
	};
	struct x86_io_resource_descriptor
		descriptors[X86_LEGACY_CHIPSET_RESOURCE_COUNT];
	x86_io_resource_handle_t resources[X86_LEGACY_CHIPSET_RESOURCE_COUNT];
	uint32_t value = 0xabcdef01u;

	if (x86_legacy_chipset_policy_config(&platform, &config) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_prepare(CONTEXT_IDENTITY, OWNER_IDENTITY,
				       &config, descriptors,
				       X86_LEGACY_CHIPSET_RESOURCE_COUNT - 1u) !=
		    X86_LEGACY_CHIPSET_CAPACITY_EXHAUSTED)
		return 1;
	if (x86_legacy_chipset_prepare(CONTEXT_IDENTITY, OWNER_IDENTITY,
				       &config, descriptors,
				       ARRAY_SIZE(descriptors)) !=
		    X86_LEGACY_CHIPSET_OK ||
	    descriptors[0].write(CONTEXT_IDENTITY,
				 X86_PIC_PRIMARY_COMMAND_PORT,
				 DOS_IO_WIDTH_8, 0x20u) !=
		    X86_IO_CALLBACK_FAULT ||
	    x86_legacy_chipset_abort(CONTEXT_IDENTITY) !=
		    X86_LEGACY_CHIPSET_OK)
		return 2;
	if (x86_legacy_chipset_prepare(CONTEXT_IDENTITY, OWNER_IDENTITY,
				       &config, descriptors,
				       ARRAY_SIZE(descriptors)) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_io_resource_registry_initialize(REGISTRY_IDENTITY) !=
		    X86_IO_RESOURCE_OK ||
	    x86_io_resource_register_batch(descriptors,
					   ARRAY_SIZE(descriptors), resources,
					   ARRAY_SIZE(resources)) !=
		    X86_IO_RESOURCE_OK ||
	    x86_legacy_chipset_publish(CONTEXT_IDENTITY) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_bind(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, OWNER_IDENTITY,
		    &source_config) != X86_LEGACY_CHIPSET_INVALID_ARGUMENT ||
	    x86_legacy_chipset_source_bind(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, SOURCE_IDENTITY,
		    &source_config) != X86_LEGACY_CHIPSET_OK)
		return 3;
	if (x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID,
				 X86_PIC_PRIMARY_COMMAND_PORT,
				 DOS_IO_WIDTH_16, &value) !=
		    X86_IO_RESOURCE_ACCESS_DENIED ||
	    value != 0xabcdef01u)
		return 4;
	return 0;
}

static int test_dma(void)
{
	struct x86_legacy_chipset_snapshot snapshot;
	const struct x86_io_resource_descriptor post_port = {
		.owner_identity = OWNER_IDENTITY,
		.first_port = 0x0080u,
		.last_port = 0x0080u,
		.read_width_mask = X86_IO_WIDTH_MASK_8,
		.write_width_mask = X86_IO_WIDTH_MASK_8,
		.read_action = X86_IO_RESOURCE_ACTION_ABSENT,
		.write_action = X86_IO_RESOURCE_ACTION_ABSENT,
		.reserved = {0u},
	};
	x86_io_resource_handle_t post_resource;
	uint32_t value = 0xabcdef01u;

	if (x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK ||
	    snapshot.dma[0].mask != 0x0fu ||
	    snapshot.dma[0].word_addressed != 0u ||
	    snapshot.dma[1].mask != 0x0eu ||
	    snapshot.dma[1].word_addressed != 1u ||
	    snapshot.dma[1].channel[0].mode != X86_DMA_MODE_CASCADE ||
	    x86_io_resource_register(&post_port, &post_resource) !=
		    X86_IO_RESOURCE_OK ||
	    x86_io_resource_unregister(post_resource, OWNER_IDENTITY) !=
		    X86_IO_RESOURCE_OK)
		return 1;

	/* Program channel 2 exactly as a byte-addressed ISA device would. */
	if (!write8(X86_DMA_PRIMARY_CLEAR_FLIP_FLOP_PORT, 0u) ||
	    !write8(0x0004u, 0x34u) || !write8(0x0004u, 0x12u) ||
	    !write8(0x0005u, 0x78u) || !write8(0x0005u, 0x56u) ||
	    !write8(0x0081u, 0xabu) ||
	    !write8(X86_DMA_PRIMARY_MODE_PORT, 0x46u) ||
	    !write8(X86_DMA_PRIMARY_SINGLE_MASK_PORT, 0x02u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK)
		return 2;
	if (snapshot.dma[0].channel[2].base_address != 0x1234u ||
	    snapshot.dma[0].channel[2].current_address != 0x1234u ||
	    snapshot.dma[0].channel[2].base_count != 0x5678u ||
	    snapshot.dma[0].channel[2].current_count != 0x5678u ||
	    snapshot.dma[0].channel[2].page != 0xabu ||
	    snapshot.dma[0].channel[2].mode != 0x46u ||
	    snapshot.dma[0].mask != 0x0bu)
		return 3;
	if (!write8(X86_DMA_PRIMARY_CLEAR_FLIP_FLOP_PORT, 0u) ||
	    !read8(0x0004u, 0x34u) || !read8(0x0004u, 0x12u) ||
	    !read8(0x0005u, 0x78u) || !read8(0x0005u, 0x56u) ||
	    !read8(0x0081u, 0xabu) || !read8(0x0080u, 0xffu))
		return 4;
	/* Each byte write updates the corresponding base and current byte. */
	if (!write8(X86_DMA_PRIMARY_CLEAR_FLIP_FLOP_PORT, 0u) ||
	    !write8(0x0004u, 0x9au) ||
	    !write8(X86_DMA_PRIMARY_CLEAR_FLIP_FLOP_PORT, 0u) ||
	    !read8(0x0004u, 0x9au) || !read8(0x0004u, 0x12u) ||
	    !write8(X86_DMA_PRIMARY_CLEAR_FLIP_FLOP_PORT, 0u) ||
	    !write8(0x0004u, 0x34u) || !write8(0x0004u, 0x12u))
		return 4;

	/* Software requests are visible in status; status reads clear only TC. */
	if (!write8(X86_DMA_PRIMARY_REQUEST_PORT, 0x06u) ||
	    !read8(X86_DMA_PRIMARY_STATUS_COMMAND_PORT, 0x40u) ||
	    !read8(X86_DMA_PRIMARY_STATUS_COMMAND_PORT, 0x40u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK ||
	    snapshot.dma[0].channel[2].request_active != 1u ||
	    !write8(X86_DMA_PRIMARY_REQUEST_PORT, 0x02u) ||
	    !write8(X86_DMA_PRIMARY_STATUS_COMMAND_PORT, 0xa5u) ||
	    !write8(X86_DMA_PRIMARY_ALL_MASK_PORT, 0x05u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK ||
	    snapshot.dma[0].mask != 0x05u ||
	    !write8(X86_DMA_PRIMARY_CLEAR_MASK_PORT, 0u) ||
	    !write8(X86_DMA_PRIMARY_SINGLE_MASK_PORT, 0x07u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK ||
	    snapshot.dma[0].mask != 0x08u ||
	    !read8(X86_DMA_PRIMARY_REQUEST_PORT, 0xffu) ||
	    !read8(X86_DMA_PRIMARY_ALL_MASK_PORT, 0xffu))
		return 5;

	/* Master clear resets controller state but not programmed channel tuples. */
	if (!write8(X86_DMA_PRIMARY_TEMPORARY_MASTER_CLEAR_PORT, 0u) ||
	    !read8(X86_DMA_PRIMARY_TEMPORARY_MASTER_CLEAR_PORT, 0u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK ||
	    snapshot.dma[0].command != 0u || snapshot.dma[0].status != 0u ||
	    snapshot.dma[0].mask != 0x0fu || snapshot.dma[0].flip_flop != 0u ||
	    snapshot.dma[0].channel[2].base_address != 0x1234u ||
	    snapshot.dma[0].channel[2].mode != 0x46u)
		return 6;

	/* The secondary controller uses word registers and doubled port spacing.
	 * Its ignored low address bit aliases the preceding even register. */
	if (!write8(X86_DMA_SECONDARY_CLEAR_FLIP_FLOP_PORT, 0u) ||
	    !write8(0x00c0u, 0xdau) || !write8(0x00c1u, 0x03u) ||
	    !write8(0x00c6u, 0xefu) || !write8(0x00c7u, 0xbeu) ||
	    !write8(0x008bu, 0x35u) ||
	    !write8(X86_DMA_SECONDARY_MODE_PORT, 0x45u) ||
	    !write8(X86_DMA_SECONDARY_SINGLE_MASK_PORT, 0x01u) ||
	    !write8(0x008fu, 0x11u) || !read8(0x008fu, 0x10u) ||
	    !write8(X86_DMA_SECONDARY_CLEAR_FLIP_FLOP_PORT, 0u) ||
	    !read8(0x00c0u, 0xdau) || !read8(0x00c1u, 0x03u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK)
		return 7;
	if (snapshot.dma[1].channel[0].base_address != 0x03dau ||
	    snapshot.dma[1].channel[0].page != 0x10u ||
	    snapshot.dma[1].channel[0].mode != X86_DMA_MODE_CASCADE ||
	    snapshot.dma[1].channel[1].base_count != 0xbeefu ||
	    snapshot.dma[1].channel[1].page != 0x34u ||
	    snapshot.dma[1].channel[1].mode != 0x45u ||
	    snapshot.dma[1].mask != 0x0cu)
		return 8;

	/* Width policy rejects a word access before any callback can mutate state. */
	if (x86_io_resource_write(KERNEL_OBJECT_HANDLE_INVALID, 0x00c0u,
				  DOS_IO_WIDTH_16, 0x1234u) !=
		    X86_IO_RESOURCE_ACCESS_DENIED ||
	    x86_io_resource_read(KERNEL_OBJECT_HANDLE_INVALID, 0x00c0u,
				 DOS_IO_WIDTH_16, &value) !=
		    X86_IO_RESOURCE_ACCESS_DENIED ||
	    value != 0xabcdef01u)
		return 9;
	return 0;
}

static int test_pic(void)
{
	struct x86_legacy_chipset_source_event event = {
		.kind = (uint8_t)X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE,
		.reserved = {0u},
	};
	struct x86_legacy_interrupt_claim claim = {0};
	struct x86_legacy_chipset_snapshot snapshot;

	/* The boot regression: nonspecific EOI with no pending ISR is legal. */
	if (!write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u) ||
	    !write8(X86_PIC_SECONDARY_COMMAND_PORT, 0x20u))
		return 1;
	if (!write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x11u) ||
	    !write8(X86_PIC_SECONDARY_COMMAND_PORT, 0x11u) ||
	    !write8(X86_PIC_PRIMARY_DATA_PORT, 0x20u) ||
	    !write8(X86_PIC_SECONDARY_DATA_PORT, 0x28u) ||
	    !write8(X86_PIC_PRIMARY_DATA_PORT, 0x04u) ||
	    !write8(X86_PIC_SECONDARY_DATA_PORT, 0x02u) ||
	    !write8(X86_PIC_PRIMARY_DATA_PORT, 0x01u) ||
	    !write8(X86_PIC_SECONDARY_DATA_PORT, 0x01u) ||
	    !write8(X86_PIC_PRIMARY_DATA_PORT, 0xf8u) ||
	    !write8(X86_PIC_SECONDARY_DATA_PORT, 0xfdu) ||
	    !read8(X86_PIC_PRIMARY_DATA_PORT, 0xf8u) ||
	    !read8(X86_PIC_SECONDARY_DATA_PORT, 0xfdu))
		return 2;

	event.irq = 1u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, OTHER_SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_IDENTITY_MISMATCH ||
	    x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK)
		return 3;
	event.irq = 0u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OTHER_SOURCE_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_IDENTITY_MISMATCH ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 0u || claim.vector != 0x20u || claim.cascaded != 0u ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_NO_INTERRUPT ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u) ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 1u || claim.vector != 0x21u ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u))
		return 4;

	event.irq = 9u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 9u || claim.vector != 0x29u || claim.cascaded != 1u ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK ||
	    snapshot.pic[0].in_service != 0x04u ||
	    snapshot.pic[1].in_service != 0x02u ||
	    !write8(X86_PIC_SECONDARY_COMMAND_PORT, 0x20u) ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u))
		return 5;

	if (!write8(X86_PIC_PRIMARY_DATA_PORT, 0xf9u))
		return 6;
	event.irq = 0u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_NO_INTERRUPT ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x0au) ||
	    !read8(X86_PIC_PRIMARY_COMMAND_PORT, 0x01u) ||
	    !write8(X86_PIC_PRIMARY_DATA_PORT, 0xf8u) ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.vector != 0x20u ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u) ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x0bu) ||
	    !read8(X86_PIC_PRIMARY_COMMAND_PORT, 0u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK)
		return 7;
	return snapshot.generation == 2u &&
		       snapshot.pic[0].vector_base == 0x20u &&
		       snapshot.pic[1].vector_base == 0x28u &&
		       snapshot.pic[0].cascade_config == 0x04u &&
		       snapshot.pic[1].cascade_config == 0x02u &&
		       snapshot.pic[0].mode_8086 == 1u &&
		       snapshot.pic[1].mode_8086 == 1u &&
		       snapshot.interrupt_delivery_active == 1u &&
		       snapshot.interrupt_source_identity == SOURCE_IDENTITY
		       ? 0
		       : 8;
}

static bool pic_delivery_state_equal(
	const struct x86_legacy_chipset_snapshot *left,
	const struct x86_legacy_chipset_snapshot *right)
{
	return left->pic[0].interrupt_request ==
		       right->pic[0].interrupt_request &&
	       left->pic[0].in_service == right->pic[0].in_service &&
	       left->pic[1].interrupt_request ==
		       right->pic[1].interrupt_request &&
	       left->pic[1].in_service == right->pic[1].in_service;
}

static int test_pic_delivery_transaction(void)
{
	struct x86_legacy_chipset_source_event event = {
		.kind = (uint8_t)X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE,
		.reserved = {0u},
	};
	struct x86_legacy_interrupt_claim claim = {0};
	struct x86_legacy_interrupt_claim stale;
	struct x86_legacy_chipset_snapshot before;
	struct x86_legacy_chipset_snapshot after;

	/* Reserving an eligible request must not perform the PIC intack. */
	event.irq = 1u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &before) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_prepare(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 1u || claim.vector != 0x21u ||
	    claim.delivery_token == 0u ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &after) !=
		    X86_LEGACY_CHIPSET_OK ||
	    !pic_delivery_state_equal(&before, &after) ||
	    after.pic[0].interrupt_request != 0x02u ||
	    after.pic[0].in_service != 0u)
		return 1;

	/* A second reservation and a forged token cannot consume the first.  A
	 * higher-priority edge may arrive concurrently, but commit still intacks
	 * the exact generation-bound request that was reserved. */
	stale = claim;
	++stale.delivery_token;
	if (x86_legacy_chipset_interrupt_prepare(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &stale) !=
		    X86_LEGACY_CHIPSET_INVALID_STATE ||
	    x86_legacy_chipset_interrupt_commit(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &stale) !=
		    X86_LEGACY_CHIPSET_STALE_INTERRUPT)
		return 2;
	event.irq = 0u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_commit(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &after) !=
		    X86_LEGACY_CHIPSET_OK ||
	    after.pic[0].interrupt_request != 0x01u ||
	    after.pic[0].in_service != 0x02u ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u) ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 0u || !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u))
		return 3;

	/* Cancel is the reflection-failure path: it drops only the reservation,
	 * leaving IRR and ISR byte-for-byte unchanged for a later retry. */
	event.irq = 1u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_prepare(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &before) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_cancel(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &after) !=
		    X86_LEGACY_CHIPSET_OK ||
	    !pic_delivery_state_equal(&before, &after) ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 1u || !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u))
		return 4;

	/* A masked IRR bit is real PIC state, but is not committable and therefore
	 * must be reported as no interrupt until the guest unmasks it. */
	if (!write8(X86_PIC_PRIMARY_DATA_PORT, 0xf9u))
		return 5;
	event.irq = 0u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_prepare(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_NO_INTERRUPT ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &after) !=
		    X86_LEGACY_CHIPSET_OK ||
	    after.pic[0].interrupt_request != 0x01u ||
	    after.pic[0].in_service != 0u ||
	    !write8(X86_PIC_PRIMARY_DATA_PORT, 0xf8u) ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 0u || !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u))
		return 6;
	return 0;
}

static int test_pit(void)
{
	struct x86_legacy_chipset_source_event event = {
		.kind = (uint8_t)X86_LEGACY_CHIPSET_EVENT_PIT_CLOCK,
		.reserved = {0u},
	};
	struct x86_legacy_interrupt_claim claim = {0};
	struct x86_legacy_chipset_snapshot snapshot;

	if (!write8(X86_PIT_CONTROL_PORT, 0x36u) ||
	    !write8(X86_PIT_CHANNEL0_PORT, 0x34u) ||
	    !write8(X86_PIT_CHANNEL0_PORT, 0x12u) ||
	    !write8(X86_PIT_CONTROL_PORT, 0x00u) ||
	    !read8(X86_PIT_CHANNEL0_PORT, 0x34u) ||
	    !read8(X86_PIT_CHANNEL0_PORT, 0x12u))
		return 1;
	/* 8254 read-back: status only, channel 0 selected. */
	if (!write8(X86_PIT_CONTROL_PORT, 0xecu) ||
	    !read8(X86_PIT_CHANNEL0_PORT, 0xb6u) ||
	    !write8(X86_PIT_CONTROL_PORT, 0xb6u) ||
	    !write8(X86_PIT_CHANNEL2_PORT, 0xffu) ||
	    !write8(X86_PIT_CHANNEL2_PORT, 0x7fu) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK)
		return 2;

	event.pit_input_ticks = 0u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_INVALID_ARGUMENT)
		return 3;
	event.pit_input_ticks = 0x1233u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_NO_INTERRUPT ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK || snapshot.pit[0].current != 1u)
		return 4;
	event.pit_input_ticks = 1u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 0u || claim.vector != 0x20u ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u))
		return 5;

	/* A 5000-cycle divisor spans five 1193-cycle native samples. */
	if (!write8(X86_PIT_CONTROL_PORT, 0x36u) ||
	    !write8(X86_PIT_CHANNEL0_PORT, 0x88u) ||
	    !write8(X86_PIT_CHANNEL0_PORT, 0x13u))
		return 6;
	event.pit_input_ticks = 1193u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_NO_INTERRUPT ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK || snapshot.pit[0].current != 228u)
		return 7;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    claim.irq != 0u || claim.vector != 0x20u ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u))
		return 8;

	/* A 100-cycle divisor expires eleven/twelve times per native sample;
	 * the edge count remains visible while the PIC coalesces one IRR bit. */
	if (!write8(X86_PIT_CONTROL_PORT, 0x36u) ||
	    !write8(X86_PIT_CHANNEL0_PORT, 100u) ||
	    !write8(X86_PIT_CHANNEL0_PORT, 0u) ||
	    x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_interrupt_claim(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, &claim) !=
		    X86_LEGACY_CHIPSET_NO_INTERRUPT ||
	    !write8(X86_PIC_PRIMARY_COMMAND_PORT, 0x20u) ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK)
		return 9;
	return snapshot.pit[0].reload == 100u &&
		       snapshot.pit[0].current == 14u &&
		       snapshot.pit[0].mode == 3u &&
		       snapshot.pit[2].reload == 0x7fffu &&
		       snapshot.pit[0].interrupt_delivery_active == 1u &&
		       snapshot.pit_time_source_active == 1u &&
		       snapshot.pit_rate_calibrated == 0u &&
		       snapshot.pit_input_quantum == 1193u &&
		       snapshot.pit_irq0_edges == 25u &&
		       snapshot.pit_irq0_coalesced_edges == 22u
		       ? 0
		       : 10;
}

static int test_rtc(void)
{
	struct x86_legacy_chipset_source_event event = {
		.pit_input_ticks = X86_LEGACY_PIT_INPUT_HZ,
		.kind = (uint8_t)X86_LEGACY_CHIPSET_EVENT_PIT_CLOCK,
		.reserved = {0u},
	};
	struct x86_legacy_chipset_snapshot snapshot;

	if (!write8(X86_RTC_INDEX_PORT, 0x80u) ||
	    !read8(X86_RTC_INDEX_PORT, 0x80u) ||
	    !read8(X86_RTC_DATA_PORT, 0x00u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x09u) ||
	    !read8(X86_RTC_DATA_PORT, 0x00u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x32u) ||
	    !read8(X86_RTC_DATA_PORT, 0x20u))
		return 1;
	/* Unsupported interrupt enables never appear enabled without delivery. */
	if (!write8(X86_RTC_INDEX_PORT, 0x0bu) ||
	    !write8(X86_RTC_DATA_PORT, 0x72u) ||
	    !read8(X86_RTC_DATA_PORT, 0x02u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x0fu) ||
	    !write8(X86_RTC_DATA_PORT, 0x05u) ||
	    !read8(X86_RTC_DATA_PORT, 0x05u) ||
	    x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK)
		return 2;
	if (snapshot.rtc.year != 2000u || snapshot.rtc.second != 1u ||
	    snapshot.rtc.month != 1u || snapshot.rtc.day != 1u ||
	    snapshot.rtc.nmi_disabled != 0u ||
	    snapshot.rtc.time_source_active != 1u)
		return 3;

	/* SET freezes calendar updates while the guest changes a coherent tuple. */
	if (!write8(X86_RTC_INDEX_PORT, 0x0bu) ||
	    !write8(X86_RTC_DATA_PORT, 0x82u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x32u) ||
	    !write8(X86_RTC_DATA_PORT, 0x20u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x09u) ||
	    !write8(X86_RTC_DATA_PORT, 0x99u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x08u) ||
	    !write8(X86_RTC_DATA_PORT, 0x12u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x07u) ||
	    !write8(X86_RTC_DATA_PORT, 0x31u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x06u) ||
	    !write8(X86_RTC_DATA_PORT, 0x05u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x04u) ||
	    !write8(X86_RTC_DATA_PORT, 0x23u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x02u) ||
	    !write8(X86_RTC_DATA_PORT, 0x59u) ||
	    !write8(X86_RTC_INDEX_PORT, 0x00u) ||
	    !write8(X86_RTC_DATA_PORT, 0x59u))
		return 4;
	event.pit_input_ticks = (uint64_t)X86_LEGACY_PIT_INPUT_HZ * 2u;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK ||
	    snapshot.rtc.year != 2099u || snapshot.rtc.second != 59u)
		return 5;
	if (!write8(X86_RTC_INDEX_PORT, 0x0bu) ||
	    !write8(X86_RTC_DATA_PORT, 0x02u))
		return 6;
	event.pit_input_ticks = X86_LEGACY_PIT_INPUT_HZ;
	if (x86_legacy_chipset_source_submit(
		    CONTEXT_IDENTITY, SOURCE_IDENTITY, &event) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &snapshot) !=
		    X86_LEGACY_CHIPSET_OK)
		return 7;
	return snapshot.rtc.year == 2100u && snapshot.rtc.month == 1u &&
		       snapshot.rtc.day == 1u && snapshot.rtc.hour == 0u &&
		       snapshot.rtc.minute == 0u && snapshot.rtc.second == 0u &&
		       snapshot.rtc.weekday == 6u
		       ? 0
		       : 8;
}

static int test_source_lifecycle(void)
{
	const struct x86_legacy_chipset_source_config source_config = {
		.pit_input_quantum = 1193u,
		.capabilities = X86_LEGACY_CHIPSET_SOURCE_CAPABILITIES,
		.pit_rate_calibrated = 0u,
		.reserved = {0u},
	};
	const struct x86_legacy_chipset_source_event event = {
		.kind = (uint8_t)X86_LEGACY_CHIPSET_EVENT_IRQ_EDGE,
		.irq = 1u,
		.reserved = {0u},
	};
	struct x86_legacy_chipset_snapshot before;
	struct x86_legacy_chipset_snapshot after;

	if (x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &before) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_quiesce(CONTEXT_IDENTITY,
					 SOURCE_IDENTITY) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_submit(CONTEXT_IDENTITY, SOURCE_IDENTITY,
					&event) !=
		    X86_LEGACY_CHIPSET_INVALID_STATE ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &after) !=
		    X86_LEGACY_CHIPSET_OK ||
	    after.interrupt_delivery_active != 0u ||
	    after.pit_time_source_active != 0u ||
	    x86_legacy_chipset_source_resume(CONTEXT_IDENTITY,
					SOURCE_IDENTITY) !=
		    X86_LEGACY_CHIPSET_OK)
		return 1;
	if (x86_legacy_chipset_source_quiesce(CONTEXT_IDENTITY,
					 SOURCE_IDENTITY) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_unbind(CONTEXT_IDENTITY,
					SOURCE_IDENTITY) !=
		    X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_source_submit(CONTEXT_IDENTITY, SOURCE_IDENTITY,
					&event) !=
		    X86_LEGACY_CHIPSET_IDENTITY_MISMATCH ||
	    x86_legacy_chipset_source_bind(
		    CONTEXT_IDENTITY, OWNER_IDENTITY, OTHER_SOURCE_IDENTITY,
		    &source_config) != X86_LEGACY_CHIPSET_OK ||
	    x86_legacy_chipset_snapshot(CONTEXT_IDENTITY, &after) !=
		    X86_LEGACY_CHIPSET_OK ||
	    after.interrupt_source_identity != OTHER_SOURCE_IDENTITY ||
	    after.interrupt_source_generation <=
		    before.interrupt_source_generation ||
	    after.interrupt_delivery_active != 1u)
		return 2;
	return 0;
}

static int run_tests(void)
{
	int status;

	status = test_lifecycle_and_register();
	if (status != 0)
		return 10 + status;
	status = test_dma();
	if (status != 0)
		return 20 + status;
	status = test_pic();
	if (status != 0)
		return 30 + status;
	status = test_pic_delivery_transaction();
	if (status != 0)
		return 40 + status;
	status = test_pit();
	if (status != 0)
		return 50 + status;
	status = test_rtc();
	if (status != 0)
		return 60 + status;
	status = test_source_lifecycle();
	if (status != 0)
		return 70 + status;
	return 0;
}

DOSC32_TEST_ENTRY(run_tests)
