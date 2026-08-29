// SPDX-License-Identifier: GPL-2.0-only
/* One synchronous protected-mode user process with checked copy boundaries. */
#include "x86_user.h"

#include "c32_syscall.h"
#include "x86_paging.h"

#define X86_KERNEL_CODE_SELECTOR 0x08u
#define X86_USER_COPY_CHUNK 128u
#define X86_USER_PATH_MAXIMUM 128u
#define X86_USER_TAIL_MAXIMUM 126u
#define X86_USER_FILE_SLOTS 4u
#define X86_USER_FILE_SLOT_BITS 4u
#define X86_USER_FILE_SLOT_MASK 0x0fu
#define X86_USER_FILE_GENERATION_MAXIMUM 0x0fffffffu
#define X86_USER_ENVIRONMENT_NAME_MAXIMUM 126u
#define X86_USER_ENVIRONMENT_VALUE_CAPACITY 0x8000u

extern bool x86_user_enter(uint32_t entry_point, uint32_t stack_top);
extern void x86_user_kernel_return(void);

struct x86_user_file_slot {
	kernel_object_handle_t backend;
	uint64_t offset;
	uint32_t generation;
	uint8_t in_use;
	uint8_t reserved[3];
} __aligned(8);

struct x86_user_owner {
	struct x86_user_services services;
	uint32_t exit_code;
	uint32_t fault_vector;
	struct x86_user_file_slot files[X86_USER_FILE_SLOTS];
	uint8_t active;
	uint8_t exited;
	uint8_t faulted;
	uint8_t reserved;
};

static struct x86_user_owner user_owner;

static void copy_bytes(uint8_t *destination, const uint8_t *source,
		       size_t count)
{
	while (count-- != 0u)
		*destination++ = *source++;
}

static bool services_are_valid(const struct x86_user_services *services)
{
	return services != NULL && services->console_write != NULL &&
	       services->console_read_line != NULL &&
	       services->console_clear != NULL && services->dos_exec != NULL &&
	       services->dos_chdir != NULL && services->dos_getcwd != NULL &&
	       services->dos_file_open != NULL &&
	       services->dos_file_read != NULL &&
	       services->dos_file_close != NULL &&
	       services->dos_environment_get != NULL &&
	       services->context != KERNEL_OBJECT_HANDLE_INVALID;
}

static bool user_copy_from(uint8_t *destination, size_t capacity,
			   uint32_t source, size_t count)
{
	if (count > capacity ||
	    !x86_paging_user_range_is_accessible(source, count))
		return false;
	copy_bytes(destination, (const uint8_t *)(uintptr_t)source, count);
	return true;
}

static bool user_copy_to(uint32_t destination, const uint8_t *source,
			 size_t count)
{
	if (!x86_paging_user_range_is_accessible(destination, count))
		return false;
	copy_bytes((uint8_t *)(uintptr_t)destination, source, count);
	return true;
}

static uint32_t syscall_console_write(const struct x86_trap_frame *frame)
{
	uint8_t scratch[X86_USER_COPY_CHUNK];
	uint32_t source = frame->ebx;
	size_t remaining = (size_t)frame->ecx;

	if (!x86_paging_user_range_is_accessible(source, remaining))
		return C32_SYSCALL_ERROR;
	while (remaining != 0u) {
		size_t amount = remaining < sizeof(scratch) ? remaining
							  : sizeof(scratch);

		if (!user_copy_from(scratch, sizeof(scratch), source, amount) ||
		    !user_owner.services.console_write(
			    user_owner.services.context, (const char *)scratch,
			    amount))
			return C32_SYSCALL_ERROR;
		source += (uint32_t)amount;
		remaining -= amount;
	}
	return 0u;
}

static uint32_t syscall_console_read_line(const struct x86_trap_frame *frame)
{
	char scratch[X86_USER_COPY_CHUNK];
	size_t capacity = (size_t)frame->ecx;
	size_t length;

	if (capacity == 0u || capacity > sizeof(scratch) ||
	    !x86_paging_user_range_is_accessible(frame->ebx, capacity))
		return C32_SYSCALL_ERROR;
	length = user_owner.services.console_read_line(
		user_owner.services.context, scratch, capacity);
	if (length >= capacity ||
	    !user_copy_to(frame->ebx, (const uint8_t *)scratch, length + 1u))
		return C32_SYSCALL_ERROR;
	return (uint32_t)length;
}

static uint32_t syscall_dos_exec(const struct x86_trap_frame *frame)
{
	uint8_t path[X86_USER_PATH_MAXIMUM];
	uint8_t tail[X86_USER_TAIL_MAXIMUM];
	size_t path_length = (size_t)frame->ecx;
	size_t tail_length = (size_t)frame->esi;

	if (path_length == 0u || path_length > sizeof(path) ||
	    tail_length > sizeof(tail) ||
	    !user_copy_from(path, sizeof(path), frame->ebx, path_length) ||
	    (tail_length != 0u &&
	     !user_copy_from(tail, sizeof(tail), frame->edx, tail_length)))
		return C32_SYSCALL_ERROR;
	return user_owner.services.dos_exec(
		       user_owner.services.context, path, path_length, tail,
		       tail_length)
		       ? 0u
		       : C32_SYSCALL_ERROR;
}

static uint32_t syscall_dos_chdir(const struct x86_trap_frame *frame)
{
	uint8_t path[X86_USER_PATH_MAXIMUM];
	size_t path_length = (size_t)frame->ecx;

	if (path_length == 0u || path_length > sizeof(path) ||
	    !user_copy_from(path, sizeof(path), frame->ebx, path_length))
		return C32_SYSCALL_ERROR;
	return user_owner.services.dos_chdir(user_owner.services.context, path,
					     path_length)
		       ? 0u
		       : C32_SYSCALL_ERROR;
}

static uint32_t syscall_dos_getcwd(const struct x86_trap_frame *frame)
{
	char path[X86_USER_PATH_MAXIMUM];
	size_t capacity = (size_t)frame->ecx;
	size_t path_length;

	if (capacity == 0u || capacity > sizeof(path) ||
	    !x86_paging_user_range_is_accessible(frame->ebx, capacity) ||
	    !user_owner.services.dos_getcwd(user_owner.services.context, path,
					    sizeof(path), &path_length) ||
	    path_length >= capacity ||
	    !user_copy_to(frame->ebx, (const uint8_t *)path, path_length + 1u))
		return C32_SYSCALL_ERROR;
	return (uint32_t)path_length;
}

static uint32_t make_file_handle(uint32_t slot, uint32_t generation)
{
	return (generation << X86_USER_FILE_SLOT_BITS) | (slot + 1u);
}

static struct x86_user_file_slot *resolve_file_handle(uint32_t handle)
{
	uint32_t encoded_slot = handle & X86_USER_FILE_SLOT_MASK;
	uint32_t generation = handle >> X86_USER_FILE_SLOT_BITS;
	uint32_t slot;

	if (encoded_slot == 0u || generation == 0u)
		return NULL;
	slot = encoded_slot - 1u;
	if (slot >= X86_USER_FILE_SLOTS ||
	    user_owner.files[slot].in_use != 1u ||
	    user_owner.files[slot].generation != generation)
		return NULL;
	return &user_owner.files[slot];
}

static uint32_t syscall_dos_file_open(const struct x86_trap_frame *frame)
{
	uint8_t path[X86_USER_PATH_MAXIMUM];
	kernel_object_handle_t backend = KERNEL_OBJECT_HANDLE_INVALID;
	size_t path_length = (size_t)frame->ecx;
	enum x86_user_file_open_status status;
	uint32_t slot;

	if (path_length == 0u || path_length > sizeof(path) ||
	    !user_copy_from(path, sizeof(path), frame->ebx, path_length))
		return C32_SYSCALL_ERROR;
	for (slot = 0u; slot < X86_USER_FILE_SLOTS; ++slot) {
		if (user_owner.files[slot].in_use == 0u &&
		    user_owner.files[slot].generation <
			    X86_USER_FILE_GENERATION_MAXIMUM)
			break;
	}
	if (slot == X86_USER_FILE_SLOTS)
		return C32_SYSCALL_ERROR;
	status = user_owner.services.dos_file_open(
		user_owner.services.context, path, path_length, &backend);
	if (status == X86_USER_FILE_OPEN_NOT_FOUND)
		return C32_SYSCALL_NOT_FOUND;
	if (status != X86_USER_FILE_OPEN_OK ||
	    backend == KERNEL_OBJECT_HANDLE_INVALID)
		return C32_SYSCALL_ERROR;
	if (user_owner.files[slot].generation == 0u)
		user_owner.files[slot].generation = 1u;
	user_owner.files[slot].backend = backend;
	user_owner.files[slot].offset = 0u;
	user_owner.files[slot].in_use = 1u;
	return make_file_handle(slot, user_owner.files[slot].generation);
}

static uint32_t syscall_dos_file_read(const struct x86_trap_frame *frame)
{
	struct x86_user_file_slot *slot = resolve_file_handle(frame->ebx);
	uint8_t scratch[X86_USER_COPY_CHUNK];
	size_t capacity = (size_t)frame->edx;
	size_t bytes_read;

	if (slot == NULL || capacity > sizeof(scratch) ||
	    !x86_paging_user_range_is_accessible(frame->ecx, capacity) ||
	    !user_owner.services.dos_file_read(
		    user_owner.services.context, slot->backend, slot->offset,
		    scratch, capacity, &bytes_read) ||
	    bytes_read > capacity ||
	    slot->offset > ~(uint64_t)0u - (uint64_t)bytes_read ||
	    !user_copy_to(frame->ecx, scratch, bytes_read))
		return C32_SYSCALL_ERROR;
	slot->offset += bytes_read;
	return (uint32_t)bytes_read;
}

static bool close_file_slot(struct x86_user_file_slot *slot)
{
	if (!user_owner.services.dos_file_close(user_owner.services.context,
						slot->backend))
		return false;
	slot->backend = KERNEL_OBJECT_HANDLE_INVALID;
	slot->offset = 0u;
	slot->in_use = 0u;
	++slot->generation;
	return true;
}

static uint32_t syscall_dos_file_close(const struct x86_trap_frame *frame)
{
	struct x86_user_file_slot *slot = resolve_file_handle(frame->ebx);

	return slot != NULL && close_file_slot(slot) ? 0u : C32_SYSCALL_ERROR;
}

static uint32_t syscall_dos_environment_get(
	const struct x86_trap_frame *frame)
{
	uint8_t name[X86_USER_ENVIRONMENT_NAME_MAXIMUM];
	uint8_t scratch[X86_USER_COPY_CHUNK];
	size_t name_length = (size_t)frame->ecx;
	size_t capacity = (size_t)frame->esi;
	size_t value_length = 0u;
	size_t completed = 0u;
	size_t bytes_read;
	enum x86_user_environment_status status;
	uint8_t terminator = 0u;

	if (name_length == 0u || name_length > sizeof(name) ||
	    capacity == 0u || capacity > X86_USER_ENVIRONMENT_VALUE_CAPACITY ||
	    !user_copy_from(name, sizeof(name), frame->ebx, name_length) ||
	    !x86_paging_user_range_is_accessible(frame->edx, capacity))
		return C32_SYSCALL_ERROR;
	status = user_owner.services.dos_environment_get(
		user_owner.services.context, name, name_length, 0u, NULL, 0u,
		&value_length, &bytes_read);
	if (status == X86_USER_ENVIRONMENT_NOT_FOUND)
		return C32_SYSCALL_NOT_FOUND;
	if (status != X86_USER_ENVIRONMENT_OK || bytes_read != 0u)
		return C32_SYSCALL_ERROR;
	if (value_length >= capacity)
		return C32_SYSCALL_BUFFER_TOO_SMALL;
	while (completed < value_length) {
		size_t remaining = value_length - completed;
		size_t amount = remaining < sizeof(scratch) ? remaining
							    : sizeof(scratch);
		size_t observed_length = 0u;

		status = user_owner.services.dos_environment_get(
			user_owner.services.context, name, name_length,
			(uint32_t)completed, scratch, amount, &observed_length,
			&bytes_read);
		if (status != X86_USER_ENVIRONMENT_OK ||
		    observed_length != value_length || bytes_read != amount ||
		    !user_copy_to(frame->edx + (uint32_t)completed, scratch,
				  bytes_read))
			return C32_SYSCALL_ERROR;
		completed += bytes_read;
	}
	if (!user_copy_to(frame->edx + (uint32_t)completed, &terminator,
			  sizeof(terminator)))
		return C32_SYSCALL_ERROR;
	return (uint32_t)value_length;
}

static bool close_all_files(void)
{
	size_t index;
	bool closed = true;

	for (index = 0u; index < X86_USER_FILE_SLOTS; ++index) {
		if (user_owner.files[index].in_use == 1u &&
		    !close_file_slot(&user_owner.files[index]))
			closed = false;
	}
	return closed;
}

static void leave_user(struct x86_trap_frame *frame)
{
	frame->instruction_pointer =
		(uint32_t)(uintptr_t)&x86_user_kernel_return;
	frame->code_segment = X86_KERNEL_CODE_SELECTOR;
	frame->flags &= ~((uint32_t)3u << 12u);
}

bool x86_user_handle_trap(struct x86_trap_frame *frame)
{
	if (frame == NULL || user_owner.active != 1u)
		return false;
	if (frame->vector == C32_SYSCALL_VECTOR &&
	    (frame->code_segment & 3u) == 3u) {
		switch (frame->eax) {
		case C32_SYSCALL_CONSOLE_WRITE:
			frame->eax = syscall_console_write(frame);
			break;
		case C32_SYSCALL_CONSOLE_READ_LINE:
			frame->eax = syscall_console_read_line(frame);
			break;
		case C32_SYSCALL_CONSOLE_CLEAR:
			frame->eax = user_owner.services.console_clear(
					     user_owner.services.context)
					     ? 0u
					     : C32_SYSCALL_ERROR;
			break;
		case C32_SYSCALL_DOS_EXEC:
			frame->eax = syscall_dos_exec(frame);
			break;
		case C32_SYSCALL_DOS_CHDIR:
			frame->eax = syscall_dos_chdir(frame);
			break;
		case C32_SYSCALL_DOS_GETCWD:
			frame->eax = syscall_dos_getcwd(frame);
			break;
		case C32_SYSCALL_DOS_FILE_OPEN:
			frame->eax = syscall_dos_file_open(frame);
			break;
		case C32_SYSCALL_DOS_FILE_READ:
			frame->eax = syscall_dos_file_read(frame);
			break;
		case C32_SYSCALL_DOS_FILE_CLOSE:
			frame->eax = syscall_dos_file_close(frame);
			break;
		case C32_SYSCALL_DOS_ENV_GET:
			frame->eax = syscall_dos_environment_get(frame);
			break;
		case C32_SYSCALL_PROCESS_EXIT:
			user_owner.exit_code = frame->ebx;
			user_owner.exited = 1u;
			leave_user(frame);
			break;
		default:
			frame->eax = C32_SYSCALL_ERROR;
			break;
		}
		return true;
	}
	if ((frame->code_segment & 3u) == 3u) {
		user_owner.fault_vector = frame->vector;
		user_owner.faulted = 1u;
		leave_user(frame);
		return true;
	}
	return false;
}

enum x86_user_run_status x86_user_run(
	uint32_t entry_point, uint32_t stack_top,
	const struct x86_user_services *services, uint32_t *exit_code)
{
	bool entered;

	if (!services_are_valid(services) || exit_code == NULL ||
	    !x86_paging_user_range_is_accessible(entry_point, 1u) ||
	    stack_top != X86_PROTECTED_USER_STACK_TOP ||
	    !x86_paging_user_range_is_accessible(stack_top - 1u, 1u))
		return X86_USER_RUN_INVALID_ARGUMENT;
	if (user_owner.active != 0u)
		return X86_USER_RUN_BUSY;
	user_owner = (struct x86_user_owner){
		.services = *services,
		.active = 1u,
	};
	entered = x86_user_enter(entry_point, stack_top);
	if (!close_all_files()) {
		user_owner.active = 0u;
		return X86_USER_RUN_RESOURCE_FAULT;
	}
	user_owner.active = 0u;
	if (!entered)
		return X86_USER_RUN_ENTRY_FAILED;
	if (user_owner.faulted != 0u)
		return X86_USER_RUN_FAULT;
	if (user_owner.exited == 0u)
		return X86_USER_RUN_ENTRY_FAILED;
	*exit_code = user_owner.exit_code;
	return X86_USER_RUN_OK;
}
