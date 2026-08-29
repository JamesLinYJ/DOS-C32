// SPDX-License-Identifier: GPL-2.0-only
/*
 * Production composition of the initial PDB/arena with the common C EXEC
 * coordinator.  DOS remains one process-global namespace; generation
 * identities make its native ownership explicit.
 */
#include "dos_runtime_owner.h"

#include "dos_environment.h"
#include "dos_drive.h"
#include "dos_path.h"
#include "dos_process.h"
#include "dos_vectors.h"
#include "string.h"

#define DOS_INITIAL_PSP_PARAGRAPHS 0x10u
#define DOS_INITIAL_ENVIRONMENT_PARAGRAPHS 10u
#define DOS_INITIAL_ENVIRONMENT_BYTES                                         \
	(DOS_INITIAL_ENVIRONMENT_PARAGRAPHS * DOS_ENVIRONMENT_PARAGRAPH_BYTES)

static const uint8_t initial_path[] = "PATH=";
static const uint8_t initial_comspec_name[] = "COMSPEC=";
static const char initial_command_component[] = "COMMAND.COM";

struct dos_runtime_owner_state {
	struct dos_exec_transaction_table transactions;
	struct dos_exec_file_lease_table file_leases;
	struct dos_memory_lease_table memory_leases;
	struct dos_exec_backend_session_table backend_sessions;
	struct dos_personality personality;
	struct dos_runtime_owner_config config;
	kernel_object_handle_t machine_identity;
	kernel_object_handle_t machine_context;
	uint64_t machine_address_limit;
	kernel_object_handle_t file_adapter_identity;
	kernel_object_handle_t file_adapter_context;
	kernel_object_handle_t observer_adapter_identity;
	kernel_object_handle_t observer_adapter_context;
	kernel_object_handle_t sft_adapter_identity;
	kernel_object_handle_t sft_adapter_context;
	kernel_object_handle_t drive_adapter_identity;
	kernel_object_handle_t drive_adapter_context;
	kernel_object_handle_t backend_adapter_identity;
	kernel_object_handle_t backend_adapter_context;
	uint16_t initial_psp;
	uint8_t initialized;
	uint8_t reserved[5];
} __aligned(8);

static struct dos_runtime_owner_state owner;

static bool identity_is_valid(kernel_object_handle_t identity)
{
	return identity != 0u && identity != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool config_is_valid(const struct dos_runtime_owner_config *config)
{
	uint16_t predicted_psp;

	if (config == NULL ||
	    !identity_is_valid(config->coordinator_identity) ||
	    !identity_is_valid(config->file_lease_table_identity) ||
	    !identity_is_valid(config->memory_arena_identity) ||
	    !identity_is_valid(config->backend_session_table_identity) ||
	    !identity_is_valid(config->runtime_identity) ||
	    !identity_is_valid(config->personality_identity) ||
	    config->memory_lease_table_identity == 0u ||
	    config->memory_lease_table_identity ==
		DOS_MEMORY_LEASE_TABLE_IDENTITY_INVALID ||
	    config->arena_end_segment <= config->arena_head_segment ||
	    !dos_int21_drive_config_is_valid(&config->drives))
		return false;
	predicted_psp = (uint16_t)(config->arena_head_segment + 1u);
	return predicted_psp != 0u &&
	       (uint32_t)predicted_psp + DOS_INITIAL_PSP_PARAGRAPHS + 1u +
		       DOS_INITIAL_ENVIRONMENT_PARAGRAPHS <=
		       config->arena_end_segment;
}

static bool ops_are_complete(const struct dos_runtime_owner_bindings *bindings)
{
	return bindings != NULL && bindings->machine != NULL &&
	       bindings->machine->ops != NULL &&
	       bindings->machine->ops->read_memory != NULL &&
	       bindings->machine->ops->write_memory != NULL &&
	       bindings->file_ops != NULL &&
	       identity_is_valid(bindings->file_ops->identity) &&
	       bindings->file_ops->open != NULL &&
	       bindings->file_ops->probe_device != NULL &&
	       bindings->file_ops->read != NULL &&
	       bindings->file_ops->close != NULL &&
	       bindings->observer_ops != NULL &&
	       identity_is_valid(bindings->observer_ops->identity) &&
	       bindings->observer_ops->acquire != NULL &&
	       bindings->observer_ops->release != NULL &&
	       bindings->observer_ops->quarantine != NULL &&
	       bindings->sft_ops != NULL &&
	       identity_is_valid(bindings->sft_ops->identity) &&
	       bindings->sft_ops->lookup != NULL &&
	       bindings->sft_ops->device_open != NULL &&
	       bindings->sft_ops->reference_acquire != NULL &&
	       bindings->sft_ops->reference_release != NULL &&
	       bindings->sft_ops->device_close != NULL &&
	       bindings->drive_ops != NULL &&
	       identity_is_valid(bindings->drive_ops->identity) &&
	       bindings->drive_ops->resolve != NULL &&
	       bindings->backend_ops != NULL &&
	       identity_is_valid(bindings->backend_ops->identity) &&
	       bindings->backend_ops->prepare != NULL &&
	       bindings->backend_ops->release != NULL &&
	       bindings->backend_ops->run_until_event != NULL &&
	       identity_is_valid(bindings->machine_identity) &&
	       identity_is_valid(bindings->file_adapter_context) &&
	       identity_is_valid(bindings->observer_adapter_context) &&
	       identity_is_valid(bindings->sft_adapter_context) &&
	       identity_is_valid(bindings->drive_adapter_context) &&
	       identity_is_valid(bindings->backend_adapter_context) &&
	       bindings->machine->address_limit != 0u &&
	       bindings->machine->address_limit <= DOS_GUEST_32_ADDRESS_LIMIT &&
	       !bindings->machine->poisoned;
}

static bool bindings_match_owner(
	const struct dos_runtime_owner_bindings *bindings)
{
	return ops_are_complete(bindings) && owner.initialized == 1u &&
	       bindings->machine_identity == owner.machine_identity &&
	       bindings->machine->context == owner.machine_context &&
	       bindings->machine->address_limit == owner.machine_address_limit &&
	       bindings->file_ops->identity == owner.file_adapter_identity &&
	       bindings->file_adapter_context == owner.file_adapter_context &&
	       bindings->observer_ops->identity == owner.observer_adapter_identity &&
	       bindings->observer_adapter_context ==
		       owner.observer_adapter_context &&
	       bindings->sft_ops->identity == owner.sft_adapter_identity &&
	       bindings->sft_adapter_context == owner.sft_adapter_context &&
	       bindings->drive_ops->identity == owner.drive_adapter_identity &&
	       bindings->drive_adapter_context == owner.drive_adapter_context &&
	       bindings->backend_ops->identity == owner.backend_adapter_identity &&
	       bindings->backend_adapter_context == owner.backend_adapter_context;
}

static enum dos_runtime_owner_status read_initial_vectors(
	const struct dos_machine *machine,
	struct dos_process_initial_psp_request *request)
{
	struct dos_far_pointer16 vector;

	if (dos_vector_get(machine, DOS_INTERRUPT_TERMINATE, &vector) !=
		DOS_VECTOR_OK)
		return DOS_RUNTIME_OWNER_MACHINE_FAULT;
	request->terminate_vector = (struct dos_process_far_address){
		.segment = vector.segment,
		.offset = vector.offset,
	};
	if (dos_vector_get(machine, DOS_INTERRUPT_CONTROL_C, &vector) !=
		DOS_VECTOR_OK)
		return DOS_RUNTIME_OWNER_MACHINE_FAULT;
	request->control_c_vector = (struct dos_process_far_address){
		.segment = vector.segment,
		.offset = vector.offset,
	};
	if (dos_vector_get(machine, DOS_INTERRUPT_CRITICAL_ERROR, &vector) !=
		DOS_VECTOR_OK)
		return DOS_RUNTIME_OWNER_MACHINE_FAULT;
	request->critical_error_vector = (struct dos_process_far_address){
		.segment = vector.segment,
		.offset = vector.offset,
	};
	return DOS_RUNTIME_OWNER_READY;
}

enum dos_runtime_owner_status dos_runtime_owner_initialize(
	const struct dos_runtime_owner_config *config,
	const struct dos_runtime_owner_bindings *bindings)
{
	struct dos_memory_arena arena;
	struct dos_memory_allocation_result allocation;
	struct dos_memory_owner_identity command_owner = {
		.psp_segment = 0u,
		.name = {'C', 'O', 'M', 'M', 'A', 'N', 'D', 0u},
	};
	uint8_t initial_environment[DOS_INITIAL_ENVIRONMENT_BYTES] = {0u};
	char initial_command_path[DOS_PATH_CAPACITY];
	struct dos_process_initial_psp_request psp_request = {0};
	struct dos_process_psp_image psp_image;
	struct dos_memory_allocation_result environment_allocation;
	uint16_t predicted_psp;
	uint32_t environment_offset;
	uint32_t trailer_offset;
	size_t initial_command_path_length;

	if (!config_is_valid(config) || !ops_are_complete(bindings))
		return DOS_RUNTIME_OWNER_INVALID_ARGUMENT;
	if (owner.initialized != 0u)
		return DOS_RUNTIME_OWNER_INVALID_STATE;
	predicted_psp = (uint16_t)(config->arena_head_segment + 1u);
	command_owner.psp_segment = predicted_psp;
	if (dos_drive_format_absolute(
		    (uint8_t)(config->drives.boot_drive - 1u),
		    initial_command_component,
		    sizeof(initial_command_component) - 1u,
		    initial_command_path, sizeof(initial_command_path),
		    &initial_command_path_length) != DOS_DRIVE_OK)
		return DOS_RUNTIME_OWNER_INVALID_ARGUMENT;

	if (dos_exec_transaction_table_construct(&owner.transactions) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_transaction_table_initialize(
		&owner.transactions, config->coordinator_identity) !=
		DOS_EXEC_TRANSACTION_OK ||
	    dos_exec_file_lease_table_construct(&owner.file_leases) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_exec_file_lease_table_initialize(
		&owner.file_leases, config->file_lease_table_identity) !=
		DOS_EXEC_FILE_LEASE_OK ||
	    dos_memory_lease_table_construct(
		&owner.memory_leases, config->memory_lease_table_identity) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_memory_lease_table_initialize(&owner.memory_leases) !=
		DOS_MEMORY_LEASE_OK ||
	    dos_exec_backend_session_table_construct(&owner.backend_sessions) !=
		DOS_EXEC_BACKEND_SESSION_OK ||
	    dos_exec_backend_session_table_initialize(
		&owner.backend_sessions,
		config->backend_session_table_identity) !=
		DOS_EXEC_BACKEND_SESSION_OK)
		return DOS_RUNTIME_OWNER_INVALID_STATE;

	if (dos_memory_arena_construct(&arena, config->memory_arena_identity) !=
		DOS_MEMORY_OK ||
	    dos_memory_arena_initialize_checked(
		&arena, bindings->machine, config->arena_head_segment,
		config->arena_end_segment) != DOS_MEMORY_OK)
		return DOS_RUNTIME_OWNER_MEMORY_FAULT;
	if (dos_memory_allocate_named_checked(
		&arena, bindings->machine, &command_owner,
		DOS_INITIAL_PSP_PARAGRAPHS, &allocation) != DOS_MEMORY_OK ||
	    allocation.block_segment != predicted_psp)
		return DOS_RUNTIME_OWNER_MEMORY_FAULT;
	if (dos_memory_allocate_checked(
		&arena, bindings->machine, predicted_psp,
		DOS_INITIAL_ENVIRONMENT_PARAGRAPHS,
		&environment_allocation) != DOS_MEMORY_OK)
		return DOS_RUNTIME_OWNER_MEMORY_FAULT;

	/*
	 * Native COMMAND still owns a real DOS compatibility PSP.  Give that PSP
	 * the same conventional environment contract as a DOS-launched command
	 * interpreter so an Exec0 environment word of zero can genuinely inherit:
	 * strings, double NUL, word one, then argv[0].
	 */
	if (memcpy_s(initial_environment, sizeof(initial_environment),
		     initial_path, sizeof(initial_path), sizeof(initial_path)) !=
	    MEMORY_OK)
		return DOS_RUNTIME_OWNER_INVALID_STATE;
	environment_offset = (uint32_t)sizeof(initial_path);
	if (memcpy_s(initial_environment + environment_offset,
		     sizeof(initial_environment) - (size_t)environment_offset,
		     initial_comspec_name, sizeof(initial_comspec_name),
		     sizeof(initial_comspec_name) - 1u) != MEMORY_OK)
		return DOS_RUNTIME_OWNER_INVALID_STATE;
	environment_offset += (uint32_t)sizeof(initial_comspec_name) - 1u;
	if (memcpy_s(initial_environment + environment_offset,
		     sizeof(initial_environment) - (size_t)environment_offset,
		     initial_command_path, sizeof(initial_command_path),
		     initial_command_path_length + 1u) != MEMORY_OK)
		return DOS_RUNTIME_OWNER_INVALID_STATE;
	environment_offset += (uint32_t)initial_command_path_length + 1u;
	/* The zero-filled byte after COMSPEC's NUL completes the double NUL. */
	trailer_offset = environment_offset + 1u;
	initial_environment[trailer_offset] =
		(uint8_t)DOS_ENVIRONMENT_TRAILER_VALUE;
	initial_environment[trailer_offset + 1u] =
		(uint8_t)(DOS_ENVIRONMENT_TRAILER_VALUE >> 8u);
	if (memcpy_s(initial_environment + trailer_offset + 2u,
		     sizeof(initial_environment) - (size_t)trailer_offset - 2u,
		     initial_command_path, sizeof(initial_command_path),
		     initial_command_path_length + 1u) != MEMORY_OK ||
	    dos_machine_write_far(
		bindings->machine, environment_allocation.block_segment, 0u,
		initial_environment, sizeof(initial_environment),
		sizeof(initial_environment)) != DOS_MACHINE_OK)
		return DOS_RUNTIME_OWNER_MACHINE_FAULT;

	psp_request.psp_segment = predicted_psp;
	psp_request.block_end_segment =
		(uint16_t)(predicted_psp + DOS_INITIAL_PSP_PARAGRAPHS);
	psp_request.environment_segment = environment_allocation.block_segment;
	if (read_initial_vectors(bindings->machine, &psp_request) !=
		DOS_RUNTIME_OWNER_READY)
		return DOS_RUNTIME_OWNER_MACHINE_FAULT;
	if (dos_process_prepare_initial_psp(&psp_request, &psp_image) !=
		DOS_PROCESS_OK ||
	    dos_process_commit_psp(bindings->machine, &psp_image) !=
		DOS_PROCESS_OK)
		return DOS_RUNTIME_OWNER_PROCESS_FAULT;
	if (dos_personality_initialize(
		&owner.personality, config->personality_identity,
		bindings->machine_identity, bindings->machine, &arena,
		config->runtime_identity, predicted_psp, &config->drives) !=
	    DOS_PERSONALITY_READY)
		return DOS_RUNTIME_OWNER_PROCESS_FAULT;

	owner.config = *config;
	owner.machine_identity = bindings->machine_identity;
	owner.machine_context = bindings->machine->context;
	owner.machine_address_limit = bindings->machine->address_limit;
	owner.file_adapter_identity = bindings->file_ops->identity;
	owner.file_adapter_context = bindings->file_adapter_context;
	owner.observer_adapter_identity = bindings->observer_ops->identity;
	owner.observer_adapter_context = bindings->observer_adapter_context;
	owner.sft_adapter_identity = bindings->sft_ops->identity;
	owner.sft_adapter_context = bindings->sft_adapter_context;
	owner.drive_adapter_identity = bindings->drive_ops->identity;
	owner.drive_adapter_context = bindings->drive_adapter_context;
	owner.backend_adapter_identity = bindings->backend_ops->identity;
	owner.backend_adapter_context = bindings->backend_adapter_context;
	owner.initial_psp = predicted_psp;
	owner.initialized = 1u;
	return DOS_RUNTIME_OWNER_READY;
}

static_assert_expression(
	(sizeof(initial_path) + sizeof(initial_comspec_name) - 1u +
	 DOS_PATH_CAPACITY + 1u + 2u + DOS_PATH_CAPACITY) <=
		DOS_INITIAL_ENVIRONMENT_BYTES,
	"initial COMMAND environment exceeds its owned MCB");

enum dos_runtime_owner_status dos_runtime_owner_borrow_exec_services(
	const struct dos_runtime_owner_bindings *bindings,
	struct dos_exec_transaction_services *services)
{
	struct dos_exec_transaction_services prepared;

	if (services == NULL || !bindings_match_owner(bindings))
		return DOS_RUNTIME_OWNER_INVALID_ARGUMENT;
	prepared = (struct dos_exec_transaction_services){
		.file_leases = &owner.file_leases,
		.file_ops = bindings->file_ops,
		.observer_ops = bindings->observer_ops,
		.sft_ops = bindings->sft_ops,
		.drive_ops = bindings->drive_ops,
		.runtime = &owner.personality.int21.process_runtime,
		.machine = bindings->machine,
		.memory_leases = &owner.memory_leases,
		.memory_arena = &owner.personality.int21.memory_arena,
		.backend_sessions = &owner.backend_sessions,
		.backend_ops = bindings->backend_ops,
		.coordinator_identity = owner.config.coordinator_identity,
		.machine_identity = owner.machine_identity,
		.file_lease_table_identity =
			owner.config.file_lease_table_identity,
		.file_adapter_context = owner.file_adapter_context,
		.sft_adapter_identity = owner.sft_adapter_identity,
		.sft_adapter_context = owner.sft_adapter_context,
		.drive_adapter_identity = owner.drive_adapter_identity,
		.drive_adapter_context = owner.drive_adapter_context,
		.observer_adapter_context = owner.observer_adapter_context,
		.backend_session_table_identity =
			owner.config.backend_session_table_identity,
		.backend_adapter_context = owner.backend_adapter_context,
		.memory_lease_table_identity =
			owner.config.memory_lease_table_identity,
	};
	*services = prepared;
	return DOS_RUNTIME_OWNER_READY;
}

struct dos_exec_transaction_table *dos_runtime_owner_transactions(void)
{
	return owner.initialized == 1u ? &owner.transactions : NULL;
}

struct dos_exec_backend_session_table *dos_runtime_owner_sessions(void)
{
	return owner.initialized == 1u ? &owner.backend_sessions : NULL;
}

struct dos_personality *dos_runtime_owner_personality(void)
{
	return owner.initialized == 1u ? &owner.personality : NULL;
}

uint16_t dos_runtime_owner_initial_psp(void)
{
	return owner.initialized == 1u ? owner.initial_psp : 0u;
}
