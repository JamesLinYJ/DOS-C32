// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core DOS INT 21h register dispatcher
 *
 * Compatibility contract: functions 00h-02h, 06h-0Eh, 25h, 30h, 35h, 40h, 44h,
 *                 48h-4Ah, 4Ch-4Dh, 50h-52h, 58h, 59h, 62h-63h and 65h-67h;
 *                 SYS_RET_OK/ERR carry and error-map behavior
 * Safety changes: request-local decoding, explicit low-register writes,
 *                 typed machine failures, and no dependency on VM86
 */
#include "dos_int21.h"

#include "dos_jft.h"
#include "dos_nls.h"
#include "dos_sft_adapter.h"
#include "dos_vectors.h"
#include "iomgr_device.h"

#define INT21_SET_INTERRUPT_VECTOR 0x25u
#define INT21_STANDARD_CONSOLE_OUTPUT 0x02u
#define INT21_STANDARD_INPUT_ECHO 0x01u
#define INT21_RAW_CONSOLE_IO 0x06u
#define INT21_RAW_CONSOLE_INPUT 0x07u
#define INT21_STANDARD_INPUT_DIRECT 0x08u
#define INT21_STANDARD_STRING_OUTPUT 0x09u
#define INT21_BUFFERED_CONSOLE_INPUT 0x0au
#define INT21_STANDARD_INPUT_STATUS 0x0bu
#define INT21_STANDARD_INPUT_FLUSH 0x0cu
#define INT21_DISK_RESET 0x0du
#define INT21_SELECT_DEFAULT_DRIVE 0x0eu
#define INT21_GET_CURRENT_DRIVE 0x19u
#define INT21_SET_DTA 0x1au
#define INT21_GET_DEFAULT_DPB 0x1fu
#define INT21_PARSE_FILE_DESCRIPTOR 0x29u
#define INT21_WRITE_HANDLE 0x40u
#define INT21_DEVICE_CONTROL 0x44u
#define INT21_GET_VERSION 0x30u
#define INT21_GET_DTA 0x2fu
#define INT21_GET_DPB 0x32u
#define INT21_DOS_VARIABLES 0x33u
#define INT21_GET_INDOS_ADDRESS 0x34u
#define INT21_GET_INTERRUPT_VECTOR 0x35u
#define INT21_GET_DISK_FREE_SPACE 0x36u
#define INT21_CREATE_DIRECTORY 0x39u
#define INT21_CHANGE_DIRECTORY 0x3bu
#define INT21_CREATE_FILE 0x3cu
#define INT21_OPEN_FILE 0x3du
#define INT21_CLOSE_FILE 0x3eu
#define INT21_READ_FILE 0x3fu
#define INT21_DELETE_FILE 0x41u
#define INT21_FILE_ATTRIBUTES 0x43u
#define INT21_GET_CURRENT_DIRECTORY 0x47u
#define INT21_SEEK_FILE 0x42u
#define INT21_TERMINATE_COMPATIBLE 0x00u
#define INT21_ALLOCATE_MEMORY 0x48u
#define INT21_FREE_MEMORY 0x49u
#define INT21_RESIZE_MEMORY 0x4au
#define INT21_EXECUTE 0x4bu
#define INT21_TERMINATE 0x4cu
#define INT21_GET_CHILD_RETURN 0x4du
#define INT21_FIND_FIRST 0x4eu
#define INT21_FIND_NEXT 0x4fu
#define INT21_SET_CURRENT_PDB 0x50u
#define INT21_GET_CURRENT_PDB 0x51u
#define INT21_GET_LIST_OF_LISTS 0x52u
#define INT21_RENAME_FILE 0x56u
#define INT21_FILE_DATE_TIME 0x57u
#define INT21_GET_EXTENDED_COUNTRY 0x65u
#define INT21_GET_SET_CODE_PAGE 0x66u
#define INT21_SET_HANDLE_COUNT 0x67u
#define INT21_ALLOCATION_OPERATION 0x58u
#define INT21_GET_EXTENDED_ERROR 0x59u
#define INT21_NETWORK_REDIRECTION 0x5fu
#define INT21_GET_CURRENT_PSP 0x62u
#define INT21_EXTENDED_CODE_SYSTEM 0x63u
#define INT21_MAXIMUM_FUNCTION 0x6cu
#define INT21_STRING_SCAN_LIMIT 0x10000u
#define INT21_LIST_OF_LISTS_SEGMENT 0x0f00u
#define INT21_LIST_OF_LISTS_OFFSET 0x0002u
#define INT21_INDOS_SEGMENT 0x0f00u
#define INT21_INDOS_OFFSET 0x0180u
#define INT21_RAW_CONSOLE_INPUT_REQUEST 0xffu
#define INT21_IOCTL_GET_DEVICE_INFO 0u
#define INT21_IOCTL_CONTROL_READ 2u
#define INT21_IOCTL_CONTROL_WRITE 3u
#define INT21_IOCTL_CHECK_REMOVABLE 8u
#define INT21_DEVICE_CHARACTER 0x8000u
#define INT21_DEVICE_SUPPORTS_CONTROL 0x4000u
#define INT21_DEVICE_FLAG 0x0080u
#define INT21_DEVICE_EOF 0x0040u
#define INT21_FILE_CLEAN 0x0040u
#define INT21_FILE_DRIVE_MASK 0x003fu
#define INT21_DEVICE_CONSOLE_OUTPUT 0x0002u
#define INT21_DEVICE_CONSOLE_INPUT 0x0001u
#define INT21_NLS_SYSTEM_CODE_PAGE 437u
#define INT21_NLS_GET_CODE_PAGE 1u
#define INT21_NLS_SET_CODE_PAGE 2u
#define INT21_NLS_ACTIVE_COUNTRY 1u
#define INT21_NLS_COUNTRY_INFORMATION 1u
#define INT21_NLS_COLLATE 6u
#define INT21_NLS_DBCS 7u
#define INT21_NLS_POINTER_RESULT_BYTES 5u
#define INT21_NLS_COUNTRY_DATA_BYTES 36u
#define INT21_NLS_COUNTRY_RESULT_BYTES \
	(INT21_NLS_COUNTRY_DATA_BYTES + 3u)
#define INT21_NLS_SEGMENT 0x0e00u
#define INT21_NLS_COLLATE_OFFSET 0u
#define INT21_NLS_COLLATE_LENGTH 256u
#define INT21_NLS_DBCS_OFFSET (INT21_NLS_COLLATE_LENGTH + 2u)
#define INT21_NLS_DBCS_DATA_OFFSET (INT21_NLS_DBCS_OFFSET + 2u)
#define INT21_NLS_DBCS_CAPACITY \
	(INT21_NLS_UPCASE_CODE_OFFSET - INT21_NLS_DBCS_DATA_OFFSET)
#define INT21_NLS_UPCASE_CODE_OFFSET 0x0110u
#define INT21_NLS_UPCASE_CODE_BYTES 11u
#define INT21_NLS_UPCASE_TABLE_OFFSET 0x0120u
#define INT21_NLS_UPCASE_TABLE_BYTES 256u
#define INT21_NLS_STORAGE_BYTES \
	(INT21_NLS_UPCASE_TABLE_OFFSET + INT21_NLS_UPCASE_TABLE_BYTES)
#define INT21_DOS_VARIABLE_GET_BREAK 0x00u
#define INT21_DOS_VARIABLE_SET_BREAK 0x01u
#define INT21_DOS_VARIABLE_EXCHANGE_BREAK 0x02u
#define INT21_DOS_VARIABLE_GET_BOOT_DRIVE 0x05u
#define INT21_DOS_VARIABLE_GET_REAL_VERSION 0x06u
#define INT21_FILE_ATTRIBUTES_GET 0u
#define INT21_FILE_ATTRIBUTES_SET 1u
#define INT21_PATH_CAPACITY 128u
#define INT21_FILE_TRANSFER_BYTES 512u
#define INT21_DOS_DEVICE_NAME_BYTES 8u
#define INT21_DIRECTORY_ATTRIBUTE 0x0010u

#define INT21_ALLOC_GET 0u
#define INT21_ALLOC_SET 1u

struct int21_jft_control {
	struct dos_far_pointer16 pointer;
	uint16_t length;
};

enum int21_handle_status {
	INT21_HANDLE_OK = 0,
	INT21_HANDLE_INVALID,
	INT21_HANDLE_FULL,
	INT21_HANDLE_MACHINE_FAULT,
	INT21_HANDLE_MACHINE_POISONED
};

struct int21_resolved_handle {
	struct dos_sft_registry_view sft;
	struct int21_jft_control jft;
	uint16_t jfn;
	uint8_t sfn;
};

static void int21_return_success(struct dos_cpu_state *registers);
static void int21_return_error(struct dos_int21_context *context,
			       struct dos_cpu_state *registers,
			       uint8_t function,
			       enum dos_error real_error);
static enum dos_int21_status return_iomgr_device_error(
	struct dos_int21_context *context, struct dos_cpu_state *registers,
	uint8_t function, enum iomgr_status status,
	enum dos_error exact_error);
static enum int21_handle_status int21_resolve_handle(
	struct dos_int21_context *context, uint16_t jfn,
	struct int21_resolved_handle *resolved);
static enum dos_int21_status int21_return_handle_status(
	struct dos_int21_context *context, struct dos_cpu_state *registers,
	uint8_t function, enum int21_handle_status status);
static enum dos_int21_status int21_publish_sft_io(
	struct dos_int21_context *context,
	const struct dos_sft_registry_view *sft, uint64_t position,
	uint64_t size, uint16_t information);
static void int21_quarantine_sft(
	struct dos_int21_context *context,
	dos_sft_reference_handle_t reference_handle);

static bool machine_is_usable(const struct dos_machine *machine)
{
	return machine != NULL && machine->ops != NULL &&
	       machine->ops->read_memory != NULL &&
	       machine->ops->write_memory != NULL &&
	       machine->address_limit != 0u &&
	       machine->address_limit <= DOS_GUEST_32_ADDRESS_LIMIT;
}

static bool context_is_initialized(const struct dos_int21_context *context)
{
	return context != NULL && context->initialized &&
	       context->memory_arena.constructed &&
	       context->memory_arena.initialized &&
	       context->process_runtime.constructed &&
	       context->process_runtime.initialized;
}

static bool drive_index_is_available(const struct dos_int21_context *context,
				     uint8_t drive)
{
	return drive < DOS_DRIVE_COUNT &&
	       (context->available_drive_mask & ((uint32_t)1u << drive)) != 0u;
}

static bool drive_abi_is_available(const struct dos_int21_context *context,
				   uint8_t drive)
{
	return drive != 0u && drive <= DOS_DRIVE_COUNT &&
	       drive_index_is_available(context, (uint8_t)(drive - 1u));
}

static bool function_is_cpm_hole(uint8_t function)
{
	/* These five compatibility slots share the CP/M-style handler. */
	return function == 0x18u || function == 0x1du || function == 0x1eu ||
	       function == 0x20u || function == 0x61u;
}

static bool function_is_implemented(uint8_t function)
{
	return function == INT21_TERMINATE_COMPATIBLE ||
	       function == INT21_STANDARD_INPUT_ECHO ||
	       function == INT21_STANDARD_CONSOLE_OUTPUT ||
	       function == INT21_RAW_CONSOLE_IO ||
	       function == INT21_RAW_CONSOLE_INPUT ||
	       function == INT21_STANDARD_INPUT_DIRECT ||
	       function == INT21_STANDARD_STRING_OUTPUT ||
	       function == INT21_BUFFERED_CONSOLE_INPUT ||
	       function == INT21_STANDARD_INPUT_STATUS ||
	       function == INT21_STANDARD_INPUT_FLUSH ||
	       function == INT21_DISK_RESET ||
	       function == INT21_SELECT_DEFAULT_DRIVE ||
	       function == INT21_GET_CURRENT_DRIVE ||
	       function == INT21_SET_DTA ||
	       function == INT21_GET_DEFAULT_DPB ||
	       function == INT21_PARSE_FILE_DESCRIPTOR ||
	       function == INT21_SET_INTERRUPT_VECTOR ||
	       function == INT21_GET_DTA ||
	       function == INT21_GET_VERSION ||
	       function == INT21_GET_DPB ||
	       function == INT21_DOS_VARIABLES ||
	       function == INT21_GET_INDOS_ADDRESS ||
	       function == INT21_GET_INTERRUPT_VECTOR ||
	       function == INT21_GET_DISK_FREE_SPACE ||
	       function == INT21_CREATE_DIRECTORY ||
	       function == INT21_CHANGE_DIRECTORY ||
	       function == INT21_CREATE_FILE ||
	       function == INT21_OPEN_FILE ||
	       function == INT21_CLOSE_FILE ||
	       function == INT21_READ_FILE ||
	       function == INT21_DELETE_FILE ||
	       function == INT21_SEEK_FILE ||
	       function == INT21_FILE_ATTRIBUTES ||
	       function == INT21_GET_CURRENT_DIRECTORY ||
	       function == INT21_WRITE_HANDLE ||
	       function == INT21_DEVICE_CONTROL ||
	       function == INT21_ALLOCATE_MEMORY ||
	       function == INT21_FREE_MEMORY ||
	       function == INT21_RESIZE_MEMORY ||
	       function == INT21_EXECUTE ||
	       function == INT21_TERMINATE ||
	       function == INT21_GET_CHILD_RETURN ||
	       function == INT21_FIND_FIRST ||
	       function == INT21_FIND_NEXT ||
	       function == INT21_SET_CURRENT_PDB ||
	       function == INT21_GET_CURRENT_PDB ||
	       function == INT21_GET_LIST_OF_LISTS ||
	       function == INT21_RENAME_FILE ||
	       function == INT21_FILE_DATE_TIME ||
	       function == INT21_GET_EXTENDED_COUNTRY ||
	       function == INT21_GET_SET_CODE_PAGE ||
	       function == INT21_SET_HANDLE_COUNT ||
	       function == INT21_ALLOCATION_OPERATION ||
	       function == INT21_GET_EXTENDED_ERROR ||
	       function == INT21_NETWORK_REDIRECTION ||
	       function == INT21_GET_CURRENT_PSP ||
	       function == INT21_EXTENDED_CODE_SYSTEM;
}

/* push bx; xor bh,bh; mov bl,al; mov al,cs:[bx+0120h]; pop bx; retf */
static const uint8_t nls_upcase_code[INT21_NLS_UPCASE_CODE_BYTES] = {
	0x53u, 0x30u, 0xffu, 0x88u, 0xc3u, 0x2eu,
	0x8au, 0x87u, 0x20u, 0x01u, 0xcbu,
};

static bool build_nls_storage(const struct dos_nls_package *package,
			      uint8_t *storage, size_t capacity)
{
	uint16_t index;

	if (!dos_nls_validate_package(package) || !package->complete ||
	    storage == NULL || capacity < INT21_NLS_STORAGE_BYTES ||
	    package->dbcs.length > INT21_NLS_DBCS_CAPACITY)
		return false;
	storage[0] = (uint8_t)INT21_NLS_COLLATE_LENGTH;
	storage[1] = (uint8_t)(INT21_NLS_COLLATE_LENGTH >> 8u);
	for (index = 0u; index < INT21_NLS_COLLATE_LENGTH; ++index) {
		uint8_t value = (uint8_t)index;

		if (value >= (uint8_t)'a' && value <= (uint8_t)'z')
			value = (uint8_t)(value -
					  ((uint8_t)'a' - (uint8_t)'A'));
		else if (value >= 128u)
			value = package->collate_high[value - 128u];
		storage[index + 2u] = value;
	}
	storage[INT21_NLS_DBCS_OFFSET] = (uint8_t)package->dbcs.length;
	storage[INT21_NLS_DBCS_OFFSET + 1u] =
		(uint8_t)(package->dbcs.length >> 8u);
	for (index = INT21_NLS_DBCS_DATA_OFFSET;
	     index < INT21_NLS_STORAGE_BYTES; ++index)
		storage[index] = 0u;
	for (index = 0u; index < package->dbcs.length; ++index)
		storage[INT21_NLS_DBCS_DATA_OFFSET + index] =
			package->dbcs.ranges[index];
	for (index = 0u; index < INT21_NLS_UPCASE_CODE_BYTES; ++index)
		storage[INT21_NLS_UPCASE_CODE_OFFSET + index] =
			nls_upcase_code[index];
	for (index = 0u; index < INT21_NLS_UPCASE_TABLE_BYTES; ++index) {
		uint8_t value = (uint8_t)index;

		if (value >= (uint8_t)'a' && value <= (uint8_t)'z')
			value = (uint8_t)(value -
					  ((uint8_t)'a' - (uint8_t)'A'));
		else if (value >= 128u)
			value = package->upcase_high[value - 128u];
		storage[INT21_NLS_UPCASE_TABLE_OFFSET + index] = value;
	}
	return true;
}

static enum dos_int21_status dispatch_country_information(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint16_t requested = dos_register_low16(registers->ecx);
	uint16_t returned = requested < INT21_NLS_COUNTRY_RESULT_BYTES
				    ? requested
				    : INT21_NLS_COUNTRY_RESULT_BYTES;
	uint16_t code_page = dos_register_low16(registers->ebx);
	uint16_t country = dos_register_low16(registers->edx);
	uint8_t replacement[INT21_NLS_COUNTRY_RESULT_BYTES] = {0};
	uint8_t rollback[INT21_NLS_COUNTRY_RESULT_BYTES];
	enum dos_machine_status machine_status;

	if (requested < INT21_NLS_POINTER_RESULT_BYTES ||
	    (code_page != 0xffffu &&
	     code_page != context->nls.active->code_page) ||
	    (country != 0xffffu &&
	     country != context->nls.active->format.country)) {
		int21_return_error(context, registers,
				   INT21_GET_EXTENDED_COUNTRY,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	replacement[0] = INT21_NLS_COUNTRY_INFORMATION;
	replacement[1] = (uint8_t)(returned - 3u);
	replacement[2] = (uint8_t)((returned - 3u) >> 8u);
	replacement[3] = (uint8_t)context->nls.active->format.country;
	replacement[4] =
		(uint8_t)(context->nls.active->format.country >> 8u);
	replacement[5] = (uint8_t)context->nls.active->code_page;
	replacement[6] = (uint8_t)(context->nls.active->code_page >> 8u);
	replacement[7] = (uint8_t)context->nls.active->format.date_format;
	replacement[8] =
		(uint8_t)(context->nls.active->format.date_format >> 8u);
	for (code_page = 0u; code_page < 5u; ++code_page)
		replacement[9u + code_page] =
			context->nls.active->format.currency[code_page];
	replacement[14] = context->nls.active->format.thousands_separator;
	replacement[16] = context->nls.active->format.decimal_separator;
	replacement[18] = context->nls.active->format.date_separator;
	replacement[20] = context->nls.active->format.time_separator;
	replacement[22] = context->nls.active->format.currency_format;
	replacement[23] = context->nls.active->format.currency_digits;
	replacement[24] = context->nls.active->format.time_format;
	replacement[25] = (uint8_t)INT21_NLS_UPCASE_CODE_OFFSET;
	replacement[26] = (uint8_t)(INT21_NLS_UPCASE_CODE_OFFSET >> 8u);
	replacement[27] = (uint8_t)INT21_NLS_SEGMENT;
	replacement[28] = (uint8_t)(INT21_NLS_SEGMENT >> 8u);
	replacement[29] = context->nls.active->format.list_separator;
	machine_status = dos_machine_replace_far(
		&context->machine, registers->es,
		dos_register_low16(registers->edi), replacement,
		sizeof(replacement), rollback, sizeof(rollback), returned);
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (machine_status != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	dos_register_set_low16(&registers->eax,
			       context->nls.system_code_page);
	dos_register_set_low16(&registers->ecx, returned);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_extended_country(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t information = dos_register_low8(registers->eax);
	uint16_t code_page = dos_register_low16(registers->ebx);
	uint16_t country = dos_register_low16(registers->edx);
	uint16_t table_offset;
	uint8_t replacement[INT21_NLS_POINTER_RESULT_BYTES];
	uint8_t rollback[INT21_NLS_POINTER_RESULT_BYTES];
	enum dos_machine_status machine_status;

	if (information == INT21_NLS_COUNTRY_INFORMATION)
		return dispatch_country_information(context, registers);

	if ((information != INT21_NLS_COLLATE &&
	     information != INT21_NLS_DBCS) ||
	    dos_register_low16(registers->ecx) <
		    INT21_NLS_POINTER_RESULT_BYTES ||
	    (code_page != 0xffffu &&
	     code_page != context->nls.active->code_page) ||
	    (country != 0xffffu &&
	     country != context->nls.active->format.country)) {
		int21_return_error(context, registers,
				   INT21_GET_EXTENDED_COUNTRY,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	table_offset = information == INT21_NLS_COLLATE
			       ? INT21_NLS_COLLATE_OFFSET
			       : INT21_NLS_DBCS_OFFSET;
	replacement[0] = information;
	replacement[1] = (uint8_t)table_offset;
	replacement[2] = (uint8_t)(table_offset >> 8u);
	replacement[3] = (uint8_t)INT21_NLS_SEGMENT;
	replacement[4] = (uint8_t)(INT21_NLS_SEGMENT >> 8u);
	machine_status = dos_machine_replace_far(
		&context->machine, registers->es,
		dos_register_low16(registers->edi), replacement,
		sizeof(replacement), rollback, sizeof(rollback),
		sizeof(replacement));
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (machine_status != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	dos_register_set_low16(&registers->eax,
			       context->nls.system_code_page);
	dos_register_set_low16(&registers->ecx,
			       INT21_NLS_POINTER_RESULT_BYTES);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_device_control(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t operation = dos_register_low8(registers->eax);
	uint16_t handle = dos_register_low16(registers->ebx);
	struct int21_resolved_handle resolved;
	enum int21_handle_status handle_status;
	uint16_t information;

	if (operation == INT21_IOCTL_CHECK_REMOVABLE) {
		uint8_t drive = dos_register_low8(registers->ebx);

		if (drive == 0u)
			drive = (uint8_t)(context->current_drive + 1u);
		if (!drive_abi_is_available(context, drive)) {
			int21_return_error(context, registers,
					   INT21_DEVICE_CONTROL,
					   DOS_ERROR_INVALID_DRIVE);
			return DOS_INT21_HANDLED;
		}
		/* The current mounted-volume model exposes configured drives as fixed. */
		dos_register_set_low16(&registers->eax, 1u);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	handle_status = int21_resolve_handle(context, handle, &resolved);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_DEVICE_CONTROL, handle_status);
	if (operation == INT21_IOCTL_GET_DEVICE_INFO) {
		if (resolved.sft.backend_kind == DOS_SFT_BACKEND_FILE) {
			information = resolved.sft.information;
		} else if (resolved.sft.backend_kind ==
			   DOS_SFT_BACKEND_STANDARD) {
			information = INT21_DEVICE_CHARACTER | INT21_DEVICE_FLAG |
				      INT21_DEVICE_EOF |
				      INT21_DEVICE_CONSOLE_INPUT |
				      INT21_DEVICE_CONSOLE_OUTPUT;
		} else if (resolved.sft.backend_kind ==
			   DOS_SFT_BACKEND_DEVICE) {
			struct iomgr_device_info info;
			uint16_t updated = resolved.sft.information;
			enum iomgr_status status = iomgr_device_query_info(
				resolved.sft.backend_handle, &info);

			if (status != IOMGR_OK) {
				if (status == IOMGR_UNCERTAIN ||
				    status == IOMGR_POISONED)
					int21_quarantine_sft(
						context,
						resolved.sft.reference_handle);
				return return_iomgr_device_error(
					context, registers, INT21_DEVICE_CONTROL,
					status,
					status == IOMGR_STALE_HANDLE
						? DOS_ERROR_INVALID_HANDLE
						: DOS_ERROR_ACCESS_DENIED);
			}
			if ((info.state & IOMGR_DEVICE_STATE_END_OF_INPUT) != 0u)
				updated &= (uint16_t)~INT21_DEVICE_EOF;
			else
				updated |= INT21_DEVICE_EOF;
			if (updated != resolved.sft.information &&
			    int21_publish_sft_io(
				    context, &resolved.sft, resolved.sft.position,
				    resolved.sft.size, updated) != DOS_INT21_HANDLED)
				return DOS_INT21_MACHINE_POISONED;
			information = INT21_DEVICE_CHARACTER | updated;
			if ((info.capabilities & IOMGR_DEVICE_CAP_CONTROL) != 0u)
				information |= INT21_DEVICE_SUPPORTS_CONTROL;
		} else
			return DOS_INT21_MACHINE_FAULT;
		dos_register_set_low16(&registers->edx, information);
		dos_register_set_low16(&registers->eax, information);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (operation == INT21_IOCTL_CONTROL_READ ||
	    operation == INT21_IOCTL_CONTROL_WRITE) {
		uint8_t buffer[INT21_FILE_TRANSFER_BYTES];
		uint8_t accounting[INT21_FILE_TRANSFER_BYTES];
		uint16_t requested = dos_register_low16(registers->ecx);
		uint16_t guest_offset = dos_register_low16(registers->edx);
		size_t amount = requested;
		size_t completed = 0u;
		enum iomgr_status status;

		if (resolved.sft.backend_kind != DOS_SFT_BACKEND_DEVICE) {
			int21_return_error(context, registers,
					   INT21_DEVICE_CONTROL,
					   DOS_ERROR_INVALID_FUNCTION);
			return DOS_INT21_HANDLED;
		}
		/* Character-device IOCTL deliberately ignores the SFT access mode. */
		if (dos_machine_validate_far(&context->machine, registers->ds,
					     guest_offset, requested) !=
		    DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		if (amount > sizeof(buffer))
			amount = sizeof(buffer);
		if (operation == INT21_IOCTL_CONTROL_READ) {
			status = iomgr_device_control(
				resolved.sft.backend_handle,
				DOS_INT21_DEVICE_CONTROL_READ, NULL, 0u, 0u,
				buffer, amount, &completed);
		} else {
			if (dos_machine_read_far(
				    &context->machine, registers->ds,
				    guest_offset, buffer, amount, amount) !=
			    DOS_MACHINE_OK)
				return DOS_INT21_MACHINE_FAULT;
			status = iomgr_device_control(
				resolved.sft.backend_handle,
				DOS_INT21_DEVICE_CONTROL_WRITE, buffer, amount,
				amount, accounting, amount, &completed);
		}
		if (status != IOMGR_OK) {
			enum dos_error device_error = DOS_ERROR_ACCESS_DENIED;

			if (status == IOMGR_UNCERTAIN || status == IOMGR_POISONED)
				int21_quarantine_sft(
					context, resolved.sft.reference_handle);
			if (status == IOMGR_UNSUPPORTED)
				device_error = DOS_ERROR_INVALID_FUNCTION;
			else if (status == IOMGR_INVALID_ARGUMENT)
				device_error = DOS_ERROR_INVALID_DATA;
			else if (status == IOMGR_STALE_HANDLE ||
				 status == IOMGR_NOT_FOUND)
				device_error = DOS_ERROR_INVALID_HANDLE;
			return return_iomgr_device_error(
				context, registers, INT21_DEVICE_CONTROL, status,
				device_error);
		}
		if (completed > amount) {
			int21_quarantine_sft(
				context, resolved.sft.reference_handle);
			context->machine_poisoned = true;
			return DOS_INT21_MACHINE_POISONED;
		}
		if (operation == INT21_IOCTL_CONTROL_READ && completed != 0u) {
			enum dos_machine_status machine_status =
				dos_machine_replace_far(
					&context->machine, registers->ds,
					guest_offset, buffer, amount, accounting,
					sizeof(accounting), completed);

			/* A control read may be stateful and cannot be replayed
			 * safely after a failed guest publication. */
			if (machine_status != DOS_MACHINE_OK) {
				context->machine_poisoned = true;
				return DOS_INT21_MACHINE_POISONED;
			}
		}
		dos_register_set_low16(&registers->eax, (uint16_t)completed);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	return DOS_INT21_UNIMPLEMENTED;
}

static enum dos_int21_status dispatch_dos_variables(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t operation = dos_register_low8(registers->eax);
	bool previous;

	switch (operation) {
	case INT21_DOS_VARIABLE_GET_BREAK:
		dos_register_set_low8(&registers->edx,
				      context->break_enabled ? 1u : 0u);
		break;
	case INT21_DOS_VARIABLE_SET_BREAK:
		context->break_enabled =
			(dos_register_low8(registers->edx) & 1u) != 0u;
		dos_register_set_low8(&registers->edx,
				      context->break_enabled ? 1u : 0u);
		break;
	case INT21_DOS_VARIABLE_EXCHANGE_BREAK:
		previous = context->break_enabled;
		context->break_enabled =
			(dos_register_low8(registers->edx) & 1u) != 0u;
		dos_register_set_low8(&registers->edx, previous ? 1u : 0u);
		break;
	case INT21_DOS_VARIABLE_GET_BOOT_DRIVE:
		dos_register_set_low8(&registers->edx, context->boot_drive);
		break;
	case INT21_DOS_VARIABLE_GET_REAL_VERSION:
		dos_register_set_low16(
			&registers->ebx,
			(uint16_t)DOS_INT21_VERSION_MAJOR |
				((uint16_t)DOS_INT21_VERSION_MINOR << 8u));
		dos_register_set_low16(&registers->edx, 0u);
		break;
	default:
		dos_register_set_low8(&registers->eax, 0xffu);
		break;
	}
	/* AH=33h returns directly; carry is not rewritten. */
	return DOS_INT21_HANDLED;
}

static uint16_t int21_read_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u);
}

static enum int21_handle_status int21_read_jft_control(
	const struct dos_int21_context *context, struct int21_jft_control *control)
{
	uint8_t bytes[6];

	if (dos_machine_read_far(
		    &context->machine, context->process_runtime.current_psp,
		    (uint16_t)__builtin_offsetof(struct dos_psp_prefix40,
					     jft_length),
		    bytes, sizeof(bytes), sizeof(bytes)) != DOS_MACHINE_OK)
		return INT21_HANDLE_MACHINE_FAULT;
	control->length = int21_read_le16(bytes);
	control->pointer.offset = int21_read_le16(bytes + 2u);
	control->pointer.segment = int21_read_le16(bytes + 4u);
	if (dos_machine_validate_far(
		    &context->machine, control->pointer.segment,
		    control->pointer.offset, control->length) != DOS_MACHINE_OK)
		return INT21_HANDLE_MACHINE_FAULT;
	return INT21_HANDLE_OK;
}

static enum int21_handle_status int21_map_sft_status(
	struct dos_int21_context *context, enum dos_sft_registry_status status,
	bool stale_is_invalid)
{
	if (status == DOS_SFT_REGISTRY_READY)
		return INT21_HANDLE_OK;
	if (status == DOS_SFT_REGISTRY_POISONED) {
		context->machine_poisoned = true;
		return INT21_HANDLE_MACHINE_POISONED;
	}
	if (stale_is_invalid &&
	    (status == DOS_SFT_REGISTRY_STALE_REFERENCE ||
	     status == DOS_SFT_REGISTRY_INVALID_STATE))
		return INT21_HANDLE_INVALID;
	return INT21_HANDLE_MACHINE_FAULT;
}

static enum int21_handle_status int21_resolve_handle(
	struct dos_int21_context *context, uint16_t jfn,
	struct int21_resolved_handle *resolved)
{
	struct int21_resolved_handle prepared;
	enum int21_handle_status handle_status;
	enum dos_sft_registry_status sft_status;

	if (!context->sft_services_bound)
		return INT21_HANDLE_MACHINE_FAULT;
	handle_status = int21_read_jft_control(context, &prepared.jft);
	if (handle_status != INT21_HANDLE_OK)
		return handle_status;
	if (jfn >= prepared.jft.length)
		return INT21_HANDLE_INVALID;
	if (dos_machine_read_far(
		    &context->machine, prepared.jft.pointer.segment,
		    (uint16_t)((uint32_t)prepared.jft.pointer.offset + jfn),
		    &prepared.sfn, sizeof(prepared.sfn), sizeof(prepared.sfn)) !=
	    DOS_MACHINE_OK)
		return INT21_HANDLE_MACHINE_FAULT;
	if (prepared.sfn == DOS_JFT_UNUSED)
		return INT21_HANDLE_INVALID;
	sft_status = dos_sft_registry_resolve(
		context->sft_context, prepared.sfn, &prepared.sft);
	handle_status = int21_map_sft_status(context, sft_status, true);
	if (handle_status != INT21_HANDLE_OK)
		return handle_status;
	prepared.jfn = jfn;
	*resolved = prepared;
	return INT21_HANDLE_OK;
}

static enum int21_handle_status int21_find_free_jfn(
	const struct dos_int21_context *context, struct int21_jft_control *control,
	uint16_t *jfn)
{
	uint8_t bytes[64];
	size_t completed = 0u;
	enum int21_handle_status status = int21_read_jft_control(context, control);

	if (status != INT21_HANDLE_OK)
		return status;
	while (completed < control->length) {
		size_t remaining = (size_t)control->length - completed;
		size_t amount = remaining < sizeof(bytes) ? remaining
							 : sizeof(bytes);
		size_t index;

		if (dos_machine_read_far(
			    &context->machine, control->pointer.segment,
			    (uint16_t)((size_t)control->pointer.offset + completed),
			    bytes, sizeof(bytes), amount) != DOS_MACHINE_OK)
			return INT21_HANDLE_MACHINE_FAULT;
		for (index = 0u; index < amount; ++index) {
			if (bytes[index] == DOS_JFT_UNUSED) {
				*jfn = (uint16_t)(completed + index);
				return INT21_HANDLE_OK;
			}
		}
		completed += amount;
	}
	return INT21_HANDLE_FULL;
}

static enum int21_handle_status int21_replace_jft_entry(
	struct dos_int21_context *context, const struct int21_jft_control *control,
	uint16_t jfn, uint8_t expected, uint8_t replacement)
{
	uint8_t current;
	uint8_t rollback;
	uint16_t offset;
	enum dos_machine_status status;

	if (jfn >= control->length)
		return INT21_HANDLE_INVALID;
	offset = (uint16_t)((uint32_t)control->pointer.offset + jfn);
	if (dos_machine_read_far(&context->machine, control->pointer.segment,
				 offset, &current, sizeof(current),
				 sizeof(current)) != DOS_MACHINE_OK)
		return INT21_HANDLE_MACHINE_FAULT;
	if (current != expected)
		return INT21_HANDLE_MACHINE_FAULT;
	status = dos_machine_replace_far(
		&context->machine, control->pointer.segment, offset, &replacement,
		sizeof(replacement), &rollback, sizeof(rollback),
		sizeof(replacement));
	if (status == DOS_MACHINE_ROLLBACK_FAILED) {
		context->machine_poisoned = true;
		return INT21_HANDLE_MACHINE_POISONED;
	}
	return status == DOS_MACHINE_OK ? INT21_HANDLE_OK
					: INT21_HANDLE_MACHINE_FAULT;
}

static bool handle_allows_read(const struct dos_sft_registry_view *sft)
{
	uint8_t access = (uint8_t)(sft->mode & 0x0fu);

	return access == 0u || access == 2u;
}

static bool handle_allows_write(const struct dos_sft_registry_view *sft)
{
	uint8_t access = (uint8_t)(sft->mode & 0x0fu);

	return access == 1u || access == 2u;
}

static enum dos_int21_status int21_return_handle_status(
	struct dos_int21_context *context, struct dos_cpu_state *registers,
	uint8_t function, enum int21_handle_status status)
{
	if (status == INT21_HANDLE_MACHINE_POISONED)
		return DOS_INT21_MACHINE_POISONED;
	if (status == INT21_HANDLE_MACHINE_FAULT)
		return DOS_INT21_MACHINE_FAULT;
	int21_return_error(context, registers, function,
			   status == INT21_HANDLE_FULL
				   ? DOS_ERROR_TOO_MANY_OPEN_FILES
				   : DOS_ERROR_INVALID_HANDLE);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status int21_publish_sft_io(
	struct dos_int21_context *context,
	const struct dos_sft_registry_view *sft, uint64_t position,
	uint64_t size, uint16_t information)
{
	enum dos_sft_registry_status status = dos_sft_registry_update_io(
		context->sft_context, sft->reference_handle, position, size,
		information);

	if (status == DOS_SFT_REGISTRY_READY)
		return DOS_INT21_HANDLED;
	context->machine_poisoned = true;
	int21_quarantine_sft(context, sft->reference_handle);
	return DOS_INT21_MACHINE_POISONED;
}

static void int21_quarantine_sft(
	struct dos_int21_context *context,
	dos_sft_reference_handle_t reference_handle)
{
	enum dos_sft_registry_status status = dos_sft_registry_poison(
		context->sft_context, reference_handle);

	(void)status;
}

struct int21_open_reservation {
	struct int21_jft_control jft;
	dos_sft_reference_handle_t sft_reference;
	uint16_t jfn;
	uint8_t sfn;
};

static enum int21_handle_status int21_reserve_open_handle(
	struct dos_int21_context *context,
	struct int21_open_reservation *reservation)
{
	struct int21_open_reservation prepared;
	enum dos_sft_registry_status sft_status;
	enum int21_handle_status handle_status;

	sft_status = dos_sft_registry_reserve(
		context->sft_context, &prepared.sfn, &prepared.sft_reference);
	handle_status = int21_map_sft_status(context, sft_status, false);
	if (handle_status != INT21_HANDLE_OK) {
		if (sft_status == DOS_SFT_REGISTRY_NO_SLOT)
			return INT21_HANDLE_FULL;
		return handle_status;
	}
	handle_status = int21_find_free_jfn(
		context, &prepared.jft, &prepared.jfn);
	if (handle_status != INT21_HANDLE_OK) {
		if (dos_sft_registry_cancel(
			    context->sft_context, prepared.sft_reference) !=
		    DOS_SFT_REGISTRY_READY) {
			context->machine_poisoned = true;
			return INT21_HANDLE_MACHINE_POISONED;
		}
		return handle_status;
	}
	*reservation = prepared;
	return INT21_HANDLE_OK;
}

static enum dos_int21_status int21_cancel_open_reservation(
	struct dos_int21_context *context,
	const struct int21_open_reservation *reservation,
	enum dos_int21_status original)
{
	if (dos_sft_registry_cancel(context->sft_context,
				    reservation->sft_reference) ==
	    DOS_SFT_REGISTRY_READY)
		return original;
	context->machine_poisoned = true;
	return DOS_INT21_MACHINE_POISONED;
}

static enum dos_int21_status int21_publish_open_handle(
	struct dos_int21_context *context,
	const struct int21_open_reservation *reservation,
	const struct dos_sft_registry_publish_record *record)
{
	enum dos_sft_registry_status sft_status;
	enum int21_handle_status jft_status;
	enum dos_error close_error = DOS_SUCCESS;

	sft_status = dos_sft_registry_publish(
		context->sft_context, reservation->sft_reference, record);
	if (sft_status != DOS_SFT_REGISTRY_READY) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	jft_status = int21_replace_jft_entry(
		context, &reservation->jft, reservation->jfn, DOS_JFT_UNUSED,
		reservation->sfn);
	if (jft_status == INT21_HANDLE_OK)
		return DOS_INT21_HANDLED;
	/* The SFT is not guest-reachable.  Release its backend before failing. */
	sft_status = dos_sft_registry_close_reference(
		context->sft_context, reservation->sft_reference, &close_error);
	if (sft_status != DOS_SFT_REGISTRY_READY) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	return jft_status == INT21_HANDLE_MACHINE_POISONED
		       ? DOS_INT21_MACHINE_POISONED
		       : DOS_INT21_MACHINE_FAULT;
}

static enum dos_int21_status return_iomgr_device_error(
	struct dos_int21_context *context, struct dos_cpu_state *registers,
	uint8_t function, enum iomgr_status status, enum dos_error exact_error)
{
	if (status == IOMGR_UNCERTAIN || status == IOMGR_POISONED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (status == IOMGR_NOT_INITIALIZED || status == IOMGR_CORRUPT)
		return DOS_INT21_MACHINE_FAULT;
	context->extended_error.locus = DOS_ERROR_LOCUS_SERIAL_DEVICE;
	int21_return_error(context, registers, function, exact_error);
	return DOS_INT21_HANDLED;
}

static bool fcb_common_separator(uint8_t character)
{
	return character == (uint8_t)':' || character == (uint8_t)';' ||
	       character == (uint8_t)',' || character == (uint8_t)'=' ||
	       character == (uint8_t)'+' || character == (uint8_t)' ' ||
	       character == (uint8_t)'\t';
}

static bool fcb_field_separator(uint8_t character)
{
	static const uint8_t separators[] = {
		'/', '\\', '"', '[', ']', '<', '>', '|', '.', ':',
		';', ',', '=', '+', '\t',
	};
	size_t index;

	if (character <= (uint8_t)' ')
		return true;
	for (index = 0u; index < ARRAY_SIZE(separators); ++index) {
		if (character == separators[index])
			return true;
	}
	return false;
}

static uint8_t fcb_upcase(const uint8_t *upcase_high, uint8_t character)
{
	if (character >= (uint8_t)'a' && character <= (uint8_t)'z')
		return (uint8_t)(character - ((uint8_t)'a' - (uint8_t)'A'));
	if (character >= 128u)
		return upcase_high[character - 128u];
	return character;
}

static bool dos_device_name_character_is_valid(uint8_t character)
{
	static const uint8_t rejected[] = {
		'"', '*', '+', ',', '/', ':', ';', '<', '=', '>', '?',
		'[', '\\', ']', '|',
	};
	size_t index;

	if (character <= (uint8_t)' ' || character == 0x7fu)
		return false;
	for (index = 0u; index < ARRAY_SIZE(rejected); ++index) {
		if (character == rejected[index])
			return false;
	}
	return true;
}

static bool dos_path_separator(uint8_t character)
{
	return character == (uint8_t)'/' || character == (uint8_t)'\\';
}

static bool dos_nls_is_dbcs_lead(const struct dos_int21_context *context,
				 uint8_t character)
{
	const struct dos_nls_dbcs_table *table = &context->nls.active->dbcs;
	size_t index;

	for (index = 0u; index + 1u < table->length; index += 2u) {
		uint8_t first = table->ranges[index];
		uint8_t last = table->ranges[index + 1u];

		if (first == 0u && last == 0u)
			break;
		if (character >= first && character <= last)
			return true;
	}
	return false;
}

static size_t dos_path_next(const struct dos_int21_context *context,
			    const uint8_t *path, size_t end, size_t cursor)
{
	if (cursor < end && dos_nls_is_dbcs_lead(context, path[cursor]) &&
	    cursor + 1u < end)
		return cursor + 2u;
	return cursor + 1u;
}

enum dos_device_path_status {
	DOS_DEVICE_PATH_NOT_CANDIDATE = 0,
	DOS_DEVICE_PATH_READY,
	DOS_DEVICE_PATH_DOS_ERROR
};

static bool dos_component_is_dev(const struct dos_int21_context *context,
				 const uint8_t *path, size_t first,
				 size_t end)
{
	static const uint8_t dev[] = {'D', 'E', 'V'};
	size_t index;

	if (end - first != ARRAY_SIZE(dev))
		return false;
	for (index = 0u; index < ARRAY_SIZE(dev); ++index) {
		if (fcb_upcase(context->nls.active->upcase_high,
			       path[first + index]) != dev[index])
			return false;
	}
	return true;
}

static enum dos_device_path_status dos_device_validate_parent(
	const struct dos_int21_context *context, const uint8_t *path,
	size_t component, enum dos_error *error)
{
	uint8_t parent[INT21_PATH_CAPACITY];
	size_t length = component;
	uint16_t attributes;
	size_t index;

	while (length != 0u && dos_path_separator(path[length - 1u]))
		--length;
	if (length == 2u && path[1] == (uint8_t)':') {
		uint8_t drive = fcb_upcase(context->nls.active->upcase_high,
					   path[0]);

		if (drive < (uint8_t)'A' || drive > (uint8_t)'Z')
			return DOS_DEVICE_PATH_NOT_CANDIDATE;
		if (!drive_abi_is_available(
			    context, (uint8_t)(drive - (uint8_t)'A' + 1u))) {
			*error = DOS_ERROR_INVALID_DRIVE;
			return DOS_DEVICE_PATH_DOS_ERROR;
		}
		return DOS_DEVICE_PATH_READY;
	}
	if (length == 0u)
		return DOS_DEVICE_PATH_READY;
	if (context->file_attributes == NULL)
		return DOS_DEVICE_PATH_NOT_CANDIDATE;
	if (length + 1u > sizeof(parent)) {
		*error = DOS_ERROR_PATH_NOT_FOUND;
		return DOS_DEVICE_PATH_DOS_ERROR;
	}
	for (index = 0u; index < length; ++index)
		parent[index] = path[index];
	parent[length] = 0u;
	*error = context->file_attributes(
		context->file_attributes_context, parent, length + 1u,
		&attributes);
	if (*error != DOS_SUCCESS)
		return DOS_DEVICE_PATH_DOS_ERROR;
	if ((attributes & INT21_DIRECTORY_ATTRIBUTE) == 0u) {
		*error = DOS_ERROR_PATH_NOT_FOUND;
		return DOS_DEVICE_PATH_DOS_ERROR;
	}
	return DOS_DEVICE_PATH_READY;
}

/*
 * Device-name matching compares the eight-byte NAME1 field and ignores
 * an extension.  Its fast path recognizes a bare component and \\DEV\\name;
 * an ordinary qualified path may recognize only its final component after
 * the real parent directory has resolved.  Keep all of that DOS policy here:
 * the I/O Manager registry remains counted, exact and filesystem-neutral.
 */
static enum dos_device_path_status dos_device_name_from_path(
	const struct dos_int21_context *context, const uint8_t *path,
	size_t path_length, uint8_t storage[INT21_DOS_DEVICE_NAME_BYTES],
	struct iomgr_device_name *name, enum dos_error *path_error)
{
	size_t first = 0u;
	size_t component;
	size_t last_separator = (size_t)-1;
	size_t end;
	size_t index;
	size_t output = 0u;
	bool saw_dot = false;
	bool rooted;

	if (context == NULL || path == NULL || path_length == 0u ||
	    path[path_length - 1u] != 0u || storage == NULL || name == NULL ||
	    path_error == NULL)
		return DOS_DEVICE_PATH_NOT_CANDIDATE;
	end = path_length - 1u;
	if (end >= 2u && dos_path_separator(path[0]) &&
	    dos_path_separator(path[1]))
		return DOS_DEVICE_PATH_NOT_CANDIDATE;
	if (end >= 2u && path[1] == (uint8_t)':') {
		uint8_t drive = fcb_upcase(context->nls.active->upcase_high,
					   path[0]);

		if (drive < (uint8_t)'A' || drive > (uint8_t)'Z')
			return DOS_DEVICE_PATH_NOT_CANDIDATE;
		if (!drive_abi_is_available(
			    context, (uint8_t)(drive - (uint8_t)'A' + 1u))) {
			*path_error = DOS_ERROR_INVALID_DRIVE;
			return DOS_DEVICE_PATH_DOS_ERROR;
		}
		first = 2u;
	}
	rooted = first < end && dos_path_separator(path[first]);
	for (index = first; index < end;
	     index = dos_path_next(context, path, end, index)) {
		if (dos_path_separator(path[index]))
			last_separator = index;
	}
	component = last_separator == (size_t)-1 ? first : last_separator + 1u;
	while (end > component &&
	       (path[end - 1u] == (uint8_t)' ' ||
		path[end - 1u] == (uint8_t)'.'))
		--end;
	if (end == component)
		return DOS_DEVICE_PATH_NOT_CANDIDATE;
	if (last_separator != (size_t)-1) {
		size_t root = first;

		while (root < end && dos_path_separator(path[root]))
			++root;
		if (rooted && root < last_separator &&
		    dos_component_is_dev(context, path, root, last_separator)) {
			/* Only the canonical rooted \\DEV\\name form takes this path. */
			for (index = root; index < last_separator; ++index) {
				if (dos_path_separator(path[index]))
					return DOS_DEVICE_PATH_NOT_CANDIDATE;
			}
		} else {
			enum dos_device_path_status parent_status;

			parent_status = dos_device_validate_parent(
				context, path, component, path_error);
			if (parent_status != DOS_DEVICE_PATH_READY)
				return parent_status;
		}
	}
	for (index = 0u; index < INT21_DOS_DEVICE_NAME_BYTES; ++index)
		storage[index] = (uint8_t)' ';
	for (index = component; index < end;
	     index = dos_path_next(context, path, end, index)) {
		uint8_t character = path[index];

		if (dos_nls_is_dbcs_lead(context, character) &&
		    index + 1u < end) {
			if (!saw_dot && output < INT21_DOS_DEVICE_NAME_BYTES)
				storage[output++] = character;
			if (!saw_dot && output < INT21_DOS_DEVICE_NAME_BYTES)
				storage[output++] = path[index + 1u];
			continue;
		}
		if (character == (uint8_t)'.') {
			if (saw_dot)
				return DOS_DEVICE_PATH_NOT_CANDIDATE;
			saw_dot = true;
			continue;
		}
		if (!dos_device_name_character_is_valid(character))
			return DOS_DEVICE_PATH_NOT_CANDIDATE;
		if (!saw_dot && output < INT21_DOS_DEVICE_NAME_BYTES)
			storage[output++] = fcb_upcase(
				context->nls.active->upcase_high, character);
	}
	while (output != 0u && storage[output - 1u] == (uint8_t)' ')
		storage[--output] = (uint8_t)' ';
	if (output == 0u)
		return DOS_DEVICE_PATH_NOT_CANDIDATE;
	name->bytes = storage;
	name->length = INT21_DOS_DEVICE_NAME_BYTES;
	return DOS_DEVICE_PATH_READY;
}

static size_t fcb_parse_field(const uint8_t *source, size_t source_count,
			      size_t cursor, uint8_t *field,
			      size_t field_count, bool *wildcard,
			      const uint8_t *upcase_high)
{
	size_t output = 0u;
	uint8_t fill = (uint8_t)' ';

	while (cursor < source_count && source[cursor] != 0u &&
	       !fcb_field_separator(source[cursor]) && output < field_count) {
		uint8_t character = source[cursor++];

		if (character == (uint8_t)'*') {
			*wildcard = true;
			fill = (uint8_t)'?';
			break;
		}
		if (character == (uint8_t)'?')
			*wildcard = true;
		field[output++] = fcb_upcase(upcase_high, character);
	}
	while (output < field_count)
		field[output++] = fill;
	return cursor;
}

static enum dos_int21_status dispatch_parse_file_descriptor(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t source[INT21_PATH_CAPACITY];
	uint8_t replacement[16];
	uint8_t rollback[16];
	uint8_t control = dos_register_low8(registers->eax);
	uint16_t source_offset = dos_register_low16(registers->esi);
	size_t source_count;
	size_t cursor = 0u;
	bool wildcard = false;
	bool bad_drive = false;
	enum dos_machine_status machine_status;

	for (source_count = 0u; source_count < sizeof(source); ++source_count) {
		machine_status = dos_machine_read_far(
			&context->machine, registers->ds,
			(uint16_t)(source_offset + (uint16_t)source_count),
			&source[source_count], 1u, 1u);
		if (machine_status != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		if (source[source_count] == 0u) {
			++source_count;
			break;
		}
	}
	if (source_count == sizeof(source) && source[source_count - 1u] != 0u)
		return DOS_INT21_MACHINE_FAULT;
	machine_status = dos_machine_read_far(
		&context->machine, registers->es,
		dos_register_low16(registers->edi), replacement,
		sizeof(replacement), sizeof(replacement));
	if (machine_status != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	if ((control & 0x01u) != 0u) {
		while (cursor < source_count &&
		       fcb_common_separator(source[cursor]))
			++cursor;
	}
	while (cursor < source_count &&
	       (source[cursor] == (uint8_t)' ' ||
		source[cursor] == (uint8_t)'\t'))
		++cursor;
	if (cursor + 1u < source_count &&
	    !fcb_field_separator(source[cursor]) &&
	    source[cursor + 1u] == (uint8_t)':') {
		uint8_t drive = fcb_upcase(context->nls.active->upcase_high,
					   source[cursor]);

		if (drive < (uint8_t)'A' || drive > (uint8_t)'Z')
			bad_drive = true;
		else {
			replacement[0] = (uint8_t)(drive - (uint8_t)'A' + 1u);
			if (!drive_abi_is_available(context, replacement[0]))
				bad_drive = true;
		}
		cursor += 2u;
	} else if ((control & 0x02u) == 0u) {
		replacement[0] = 0u;
	}
	/* MS-DOS clears current block and record size during parsing. */
	replacement[12] = 0u;
	replacement[13] = 0u;
	replacement[14] = 0u;
	replacement[15] = 0u;
	if ((control & 0x04u) == 0u) {
		size_t index;

		for (index = 1u; index < 9u; ++index)
			replacement[index] = (uint8_t)' ';
	}
	if ((control & 0x08u) == 0u) {
		size_t index;

		for (index = 9u; index < 12u; ++index)
			replacement[index] = (uint8_t)' ';
	}
	if (cursor < source_count && source[cursor] == (uint8_t)'.') {
		replacement[1] = (uint8_t)'.';
		++cursor;
		if (cursor < source_count && source[cursor] == (uint8_t)'.') {
			replacement[2] = (uint8_t)'.';
			++cursor;
		}
	} else {
		cursor = fcb_parse_field(source, source_count, cursor,
					 replacement + 1u, 8u, &wildcard,
					 context->nls.active->upcase_high);
		if (cursor < source_count && source[cursor] == (uint8_t)'.') {
			++cursor;
			cursor = fcb_parse_field(source, source_count, cursor,
						 replacement + 9u, 3u,
						 &wildcard,
						 context->nls.active->upcase_high);
		}
	}
	machine_status = dos_machine_replace_far(
		&context->machine, registers->es,
		dos_register_low16(registers->edi), replacement,
		sizeof(replacement), rollback, sizeof(rollback),
		sizeof(replacement));
	if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (machine_status != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	dos_register_set_low16(
		&registers->esi, (uint16_t)(source_offset + (uint16_t)cursor));
	dos_register_set_low8(&registers->eax,
			      bad_drive ? 0xffu : (wildcard ? 1u : 0u));
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_open_file(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t path[INT21_PATH_CAPACITY];
	uint8_t device_name_storage[INT21_DOS_DEVICE_NAME_BYTES];
	struct iomgr_device_name device_name;
	struct int21_open_reservation reservation;
	struct dos_sft_registry_publish_record record;
	uint8_t mode = dos_register_low8(registers->eax);
	uint8_t access = mode & 0x07u;
	uint8_t sharing = mode & 0x70u;
	uint16_t offset = dos_register_low16(registers->edx);
	kernel_object_handle_t backend_handle = KERNEL_OBJECT_HANDLE_INVALID;
	uint64_t size = 0u;
	size_t path_length;
	enum int21_handle_status handle_status;
	enum dos_device_path_status path_status;
	enum dos_error error = DOS_SUCCESS;
	enum iomgr_status device_status;
	enum dos_int21_status result;

	/* Reserve both tables before translating or opening a path. */
	handle_status = int21_reserve_open_handle(context, &reservation);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_OPEN_FILE, handle_status);
	/* OPEN accepts access 0/1/2, sharing 0 through 4 and bit 7. */
	if (access > 2u || (mode & 0x08u) != 0u || sharing > 0x40u) {
		int21_return_error(context, registers, INT21_OPEN_FILE,
				   DOS_ERROR_INVALID_ACCESS);
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_HANDLED);
	}
	for (path_length = 0u; path_length < sizeof(path); ++path_length) {
		if (dos_machine_read_far(&context->machine, registers->ds, offset,
					 &path[path_length], 1u, 1u) !=
		    DOS_MACHINE_OK)
			return int21_cancel_open_reservation(
				context, &reservation, DOS_INT21_MACHINE_FAULT);
		offset = (uint16_t)(offset + 1u);
		if (path[path_length] == 0u) {
			++path_length;
			break;
		}
	}
	if (path_length == sizeof(path) && path[path_length - 1u] != 0u) {
		int21_return_error(context, registers, INT21_OPEN_FILE,
				   DOS_ERROR_PATH_NOT_FOUND);
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_HANDLED);
	}
	path_status = dos_device_name_from_path(
		context, path, path_length, device_name_storage, &device_name,
		&error);
	if (path_status == DOS_DEVICE_PATH_DOS_ERROR) {
		int21_return_error(context, registers, INT21_OPEN_FILE, error);
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_HANDLED);
	}
	if (path_status == DOS_DEVICE_PATH_READY) {
		device_status = iomgr_device_open(&device_name, &backend_handle);
		if (device_status == IOMGR_OK) {
			record = (struct dos_sft_registry_publish_record){
				.backend_handle = backend_handle,
				.position = 0u,
				.size = 0u,
				.flags = (mode & 0x80u) != 0u
						 ? DOS_SFT_FLAG_NO_INHERIT
						 : 0u,
				.mode = (uint16_t)(mode & 0x7fu),
				.information =
					INT21_DEVICE_FLAG | INT21_DEVICE_EOF,
				.backend_kind = DOS_SFT_BACKEND_DEVICE,
				.reserved = 0u,
			};
			result = int21_publish_open_handle(
				context, &reservation, &record);
			if (result != DOS_INT21_HANDLED)
				return result;
			dos_register_set_low16(&registers->eax, reservation.jfn);
			int21_return_success(registers);
			return DOS_INT21_HANDLED;
		}
		if (device_status != IOMGR_NOT_FOUND) {
			enum dos_error device_error = DOS_ERROR_ACCESS_DENIED;

			if (device_status == IOMGR_NO_SLOT)
				device_error = DOS_ERROR_TOO_MANY_OPEN_FILES;
			else if (device_status == IOMGR_INVALID_ARGUMENT)
				device_error = DOS_ERROR_INVALID_DATA;
			result = return_iomgr_device_error(
				context, registers, INT21_OPEN_FILE, device_status,
				device_error);
			return int21_cancel_open_reservation(
				context, &reservation, result);
		}
	}
	if (context->file_ops == NULL) {
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_UNIMPLEMENTED);
	}
	error = context->file_ops->open(context->file_context, path,
					path_length, &backend_handle, &size);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_OPEN_FILE, error);
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_HANDLED);
	}
	if (backend_handle == 0u ||
	    backend_handle == KERNEL_OBJECT_HANDLE_INVALID) {
		context->machine_poisoned = true;
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_MACHINE_POISONED);
	}
	record = (struct dos_sft_registry_publish_record){
		.backend_handle = backend_handle,
		.position = 0u,
		.size = size,
		.flags = (mode & 0x80u) != 0u ? DOS_SFT_FLAG_NO_INHERIT : 0u,
		.mode = (uint16_t)(mode & 0x7fu),
		.information = (uint16_t)(
			INT21_FILE_CLEAN |
			(context->current_drive & INT21_FILE_DRIVE_MASK)),
		.backend_kind = DOS_SFT_BACKEND_FILE,
		.reserved = 0u,
	};
	result = int21_publish_open_handle(context, &reservation, &record);
	if (result != DOS_INT21_HANDLED)
		return result;
	dos_register_set_low16(&registers->eax, reservation.jfn);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_create_file(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t path[INT21_PATH_CAPACITY];
	struct int21_open_reservation reservation;
	struct dos_sft_registry_publish_record record;
	uint16_t offset = dos_register_low16(registers->edx);
	uint16_t attributes = dos_register_low16(registers->ecx);
	kernel_object_handle_t backend_handle = KERNEL_OBJECT_HANDLE_INVALID;
	uint64_t size = 0u;
	size_t path_length;
	enum int21_handle_status handle_status;
	enum dos_error error;
	enum dos_int21_status result;

	handle_status = int21_reserve_open_handle(context, &reservation);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_CREATE_FILE, handle_status);
	if ((attributes & ~0x27u) != 0u) {
		int21_return_error(context, registers, INT21_CREATE_FILE,
				   DOS_ERROR_ACCESS_DENIED);
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_HANDLED);
	}
	if (context->file_ops == NULL || context->file_ops->create == NULL)
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_UNIMPLEMENTED);
	for (path_length = 0u; path_length < sizeof(path); ++path_length) {
		if (dos_machine_read_far(&context->machine, registers->ds, offset,
					 &path[path_length], 1u, 1u) !=
		    DOS_MACHINE_OK)
			return int21_cancel_open_reservation(
				context, &reservation, DOS_INT21_MACHINE_FAULT);
		offset = (uint16_t)(offset + 1u);
		if (path[path_length] == 0u) {
			++path_length;
			break;
		}
	}
	if (path_length == sizeof(path) && path[path_length - 1u] != 0u) {
		int21_return_error(context, registers, INT21_CREATE_FILE,
				   DOS_ERROR_PATH_NOT_FOUND);
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_HANDLED);
	}
	error = context->file_ops->create(
		context->file_context, path, path_length, attributes,
		&backend_handle, &size);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_CREATE_FILE, error);
		return int21_cancel_open_reservation(
			context, &reservation, DOS_INT21_HANDLED);
	}
	if (backend_handle == 0u ||
	    backend_handle == KERNEL_OBJECT_HANDLE_INVALID ||
	    size != 0u) {
		if (backend_handle != 0u &&
		    backend_handle != KERNEL_OBJECT_HANDLE_INVALID &&
		    context->file_ops->close(context->file_context,
					     backend_handle) != DOS_SUCCESS)
			context->machine_poisoned = true;
		return int21_cancel_open_reservation(
			context, &reservation,
			context->machine_poisoned
				? DOS_INT21_MACHINE_POISONED
				: DOS_INT21_MACHINE_FAULT);
	}
	record = (struct dos_sft_registry_publish_record){
		.backend_handle = backend_handle,
		.position = 0u,
		.size = 0u,
		.flags = 0u,
		.mode = 2u,
		.information = (uint16_t)(
			INT21_FILE_CLEAN |
			(context->current_drive & INT21_FILE_DRIVE_MASK)),
		.backend_kind = DOS_SFT_BACKEND_FILE,
		.reserved = 0u,
	};
	result = int21_publish_open_handle(context, &reservation, &record);
	if (result != DOS_INT21_HANDLED)
		return result;
	dos_register_set_low16(&registers->eax, reservation.jfn);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_close_file(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	struct int21_resolved_handle resolved;
	enum int21_handle_status handle_status;
	enum dos_sft_registry_status sft_status;
	enum dos_error exact_error = DOS_SUCCESS;

	handle_status = int21_resolve_handle(
		context, dos_register_low16(registers->ebx), &resolved);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_CLOSE_FILE, handle_status);
	sft_status = dos_sft_registry_close_reference(
		context->sft_context, resolved.sft.reference_handle, &exact_error);
	if (sft_status == DOS_SFT_REGISTRY_BACKEND_FAILURE) {
		int21_return_error(context, registers, INT21_CLOSE_FILE, exact_error);
		return DOS_INT21_HANDLED;
	}
	if (sft_status == DOS_SFT_REGISTRY_POISONED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (sft_status != DOS_SFT_REGISTRY_READY)
		return DOS_INT21_MACHINE_FAULT;
	handle_status = int21_replace_jft_entry(
		context, &resolved.jft, resolved.jfn, resolved.sfn, DOS_JFT_UNUSED);
	if (handle_status != INT21_HANDLE_OK) {
		int21_quarantine_sft(context, resolved.sft.reference_handle);
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_read_file(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	struct int21_resolved_handle resolved;
	uint8_t buffer[INT21_FILE_TRANSFER_BYTES];
	uint8_t rollback[INT21_FILE_TRANSFER_BYTES];
	uint16_t requested = dos_register_low16(registers->ecx);
	uint16_t guest_offset = dos_register_low16(registers->edx);
	size_t transferred = 0u;
	enum int21_handle_status handle_status;

	handle_status = int21_resolve_handle(
		context, dos_register_low16(registers->ebx), &resolved);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_READ_FILE, handle_status);
	if (!handle_allows_read(&resolved.sft)) {
		int21_return_error(context, registers, INT21_READ_FILE,
				   DOS_ERROR_ACCESS_DENIED);
		return DOS_INT21_HANDLED;
	}
	if (dos_machine_validate_far(&context->machine, registers->ds,
				     guest_offset, requested) != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	if (resolved.sft.backend_kind == DOS_SFT_BACKEND_STANDARD) {
		if (context->input_character == NULL)
			return DOS_INT21_UNIMPLEMENTED;
		while (transferred < requested) {
			size_t amount = (size_t)requested - transferred;
			size_t index;
			enum dos_machine_status machine_status;

			if (amount > sizeof(buffer))
				amount = sizeof(buffer);
			for (index = 0u; index < amount; ++index) {
				if (!context->input_character(
					    context->input_character_context,
					    &buffer[index]))
					return DOS_INT21_MACHINE_FAULT;
			}
			machine_status = dos_machine_replace_far(
				&context->machine, registers->ds,
				(uint16_t)(guest_offset + (uint16_t)transferred),
				buffer, sizeof(buffer), rollback, sizeof(rollback),
				amount);
			if (machine_status != DOS_MACHINE_OK) {
				int21_quarantine_sft(
					context, resolved.sft.reference_handle);
				context->machine_poisoned = true;
				return DOS_INT21_MACHINE_POISONED;
			}
			transferred += amount;
		}
		dos_register_set_low16(&registers->eax, requested);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (resolved.sft.backend_kind == DOS_SFT_BACKEND_DEVICE) {
		uint16_t information = resolved.sft.information;

		while (transferred < requested) {
			size_t amount = (size_t)requested - transferred;
			size_t bytes_read = 0u;
			enum dos_machine_status machine_status;
			enum iomgr_status status;

			if (amount > sizeof(buffer))
				amount = sizeof(buffer);
			status = iomgr_device_read(
				resolved.sft.backend_handle, buffer, amount, amount,
				&bytes_read);
			if (status != IOMGR_OK) {
				if (status == IOMGR_UNCERTAIN ||
				    status == IOMGR_POISONED) {
					int21_quarantine_sft(
						context,
						resolved.sft.reference_handle);
					return return_iomgr_device_error(
						context, registers, INT21_READ_FILE,
						status, DOS_ERROR_ACCESS_DENIED);
				}
				if (transferred != 0u)
					break;
				return return_iomgr_device_error(
					context, registers, INT21_READ_FILE,
					status,
					status == IOMGR_STALE_HANDLE
						? DOS_ERROR_INVALID_HANDLE
						: DOS_ERROR_ACCESS_DENIED);
			}
			if (bytes_read > amount) {
				int21_quarantine_sft(
					context, resolved.sft.reference_handle);
				context->machine_poisoned = true;
				return DOS_INT21_MACHINE_POISONED;
			}
			if (bytes_read == 0u) {
				information &= (uint16_t)~INT21_DEVICE_EOF;
				if (information != resolved.sft.information &&
				    int21_publish_sft_io(
					    context, &resolved.sft,
					    resolved.sft.position, resolved.sft.size,
					    information) != DOS_INT21_HANDLED)
					return DOS_INT21_MACHINE_POISONED;
				break;
			}
			machine_status = dos_machine_replace_far(
				&context->machine, registers->ds,
				(uint16_t)(guest_offset +
					   (uint16_t)transferred),
				buffer, sizeof(buffer), rollback,
				sizeof(rollback), bytes_read);
			if (machine_status != DOS_MACHINE_OK) {
				int21_quarantine_sft(
					context, resolved.sft.reference_handle);
				context->machine_poisoned = true;
				return DOS_INT21_MACHINE_POISONED;
			}
			information |= INT21_DEVICE_EOF;
			if (information != resolved.sft.information &&
			    int21_publish_sft_io(
				    context, &resolved.sft, resolved.sft.position,
				    resolved.sft.size, information) != DOS_INT21_HANDLED)
				return DOS_INT21_MACHINE_POISONED;
			transferred += bytes_read;
			if (bytes_read < amount)
				break;
		}
		dos_register_set_low16(&registers->eax,
				       (uint16_t)transferred);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (resolved.sft.backend_kind != DOS_SFT_BACKEND_FILE)
		return DOS_INT21_MACHINE_FAULT;
	if (context->file_ops == NULL || context->file_ops->read == NULL)
		return DOS_INT21_MACHINE_FAULT;
	{
		uint64_t position = resolved.sft.position;

	while (transferred < requested) {
		size_t amount = (size_t)requested - transferred;
		size_t bytes_read = 0u;
		enum dos_error error;
		enum dos_machine_status machine_status;

		if (amount > sizeof(buffer))
			amount = sizeof(buffer);
		error = context->file_ops->read(
			context->file_context, resolved.sft.backend_handle,
			position, buffer, sizeof(buffer), amount,
			&bytes_read);
		if (error != DOS_SUCCESS) {
			int21_return_error(context, registers, INT21_READ_FILE,
					   error);
			return DOS_INT21_HANDLED;
		}
		if (bytes_read > amount)
			return DOS_INT21_MACHINE_FAULT;
		if (bytes_read == 0u)
			break;
		machine_status = dos_machine_replace_far(
			&context->machine, registers->ds,
			(uint16_t)(guest_offset + (uint16_t)transferred),
			buffer, sizeof(buffer), rollback, sizeof(rollback),
			bytes_read);
		if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
			context->machine_poisoned = true;
			return DOS_INT21_MACHINE_POISONED;
		}
		if (machine_status != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		position += bytes_read;
		if (int21_publish_sft_io(
			    context, &resolved.sft, position, resolved.sft.size,
			    resolved.sft.information) != DOS_INT21_HANDLED)
			return DOS_INT21_MACHINE_POISONED;
		transferred += bytes_read;
		if (bytes_read < amount)
			break;
	}
	}
	dos_register_set_low16(&registers->eax, (uint16_t)transferred);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_seek_file(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	struct int21_resolved_handle resolved;
	uint8_t origin = dos_register_low8(registers->eax);
	uint32_t raw_offset =
		((uint32_t)dos_register_low16(registers->ecx) << 16u) |
		(uint32_t)dos_register_low16(registers->edx);
	int32_t signed_offset = (int32_t)raw_offset;
	uint64_t base;
	uint64_t target;
	enum int21_handle_status handle_status;

	handle_status = int21_resolve_handle(
		context, dos_register_low16(registers->ebx), &resolved);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_SEEK_FILE, handle_status);
	if (resolved.sft.backend_kind != DOS_SFT_BACKEND_FILE) {
		int21_return_error(context, registers, INT21_SEEK_FILE,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	if (origin == 0u)
		base = 0u;
	else if (origin == 1u)
		base = resolved.sft.position;
	else if (origin == 2u)
		base = resolved.sft.size;
	else {
		int21_return_error(context, registers, INT21_SEEK_FILE,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	if (signed_offset < 0) {
		uint64_t magnitude = (uint64_t)(-(int64_t)signed_offset);

		if (base < magnitude)
			target = (uint64_t)-1;
		else
			target = base - magnitude;
	} else if (base > (uint64_t)0xffffffffu -
				 (uint64_t)signed_offset) {
		target = (uint64_t)-1;
	} else {
		target = base + (uint64_t)signed_offset;
	}
	if (target > (uint64_t)0xffffffffu) {
		int21_return_error(context, registers, INT21_SEEK_FILE,
				   DOS_ERROR_SEEK);
		return DOS_INT21_HANDLED;
	}
	if (int21_publish_sft_io(
		    context, &resolved.sft, target, resolved.sft.size,
		    resolved.sft.information) != DOS_INT21_HANDLED)
		return DOS_INT21_MACHINE_POISONED;
	dos_register_set_low16(&registers->eax, (uint16_t)target);
	dos_register_set_low16(&registers->edx, (uint16_t)(target >> 16u));
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_file_date_time(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	struct int21_resolved_handle resolved;
	uint8_t operation = dos_register_low8(registers->eax);
	uint16_t date;
	uint16_t time;
	enum int21_handle_status handle_status;
	enum dos_error error;

	handle_status = int21_resolve_handle(
		context, dos_register_low16(registers->ebx), &resolved);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_FILE_DATE_TIME, handle_status);
	if (resolved.sft.backend_kind != DOS_SFT_BACKEND_FILE) {
		int21_return_error(context, registers, INT21_FILE_DATE_TIME,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	if (operation > 1u) {
		int21_return_error(context, registers, INT21_FILE_DATE_TIME,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	if (context->file_ops == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (operation == 1u) {
		if (context->file_ops->set_time == NULL)
			return DOS_INT21_UNIMPLEMENTED;
		error = context->file_ops->set_time(
			context->file_context, resolved.sft.backend_handle,
			dos_register_low16(registers->edx),
			dos_register_low16(registers->ecx));
		if (error != DOS_SUCCESS) {
			int21_return_error(context, registers, INT21_FILE_DATE_TIME,
					   error);
			return DOS_INT21_HANDLED;
		}
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (context->file_ops->get_time == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	error = context->file_ops->get_time(context->file_context,
					   resolved.sft.backend_handle,
					   &date, &time);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_FILE_DATE_TIME, error);
		return DOS_INT21_HANDLED;
	}
	dos_register_set_low16(&registers->ecx, time);
	dos_register_set_low16(&registers->edx, date);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_rename_file(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t old_path[INT21_PATH_CAPACITY];
	uint8_t new_path[INT21_PATH_CAPACITY];
	uint16_t old_offset = dos_register_low16(registers->edx);
	uint16_t new_offset = dos_register_low16(registers->edi);
	size_t old_length;
	size_t new_length;
	enum dos_error error;

	if (context->file_ops == NULL || context->file_ops->rename == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	for (old_length = 0u; old_length < sizeof(old_path); ++old_length) {
		if (dos_machine_read_far(&context->machine, registers->ds,
					 old_offset, &old_path[old_length], 1u,
					 1u) != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		old_offset = (uint16_t)(old_offset + 1u);
		if (old_path[old_length] == 0u) {
			++old_length;
			break;
		}
	}
	for (new_length = 0u; new_length < sizeof(new_path); ++new_length) {
		if (dos_machine_read_far(&context->machine, registers->es,
					 new_offset, &new_path[new_length], 1u,
					 1u) != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		new_offset = (uint16_t)(new_offset + 1u);
		if (new_path[new_length] == 0u) {
			++new_length;
			break;
		}
	}
	if ((old_length == sizeof(old_path) && old_path[old_length - 1u] != 0u) ||
	    (new_length == sizeof(new_path) && new_path[new_length - 1u] != 0u)) {
		int21_return_error(context, registers, INT21_RENAME_FILE,
				   DOS_ERROR_PATH_NOT_FOUND);
		return DOS_INT21_HANDLED;
	}
	error = context->file_ops->rename(
		context->file_context, old_path, old_length, new_path, new_length);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_RENAME_FILE, error);
		return DOS_INT21_HANDLED;
	}
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_file_attributes(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t operation = dos_register_low8(registers->eax);
	uint8_t path[INT21_PATH_CAPACITY];
	uint16_t offset = dos_register_low16(registers->edx);
	uint16_t attributes;
	size_t path_length;
	enum dos_error error;

	if (operation == INT21_FILE_ATTRIBUTES_SET) {
		/* The mounted FAT16 volume is intentionally read-only. */
		int21_return_error(context, registers, INT21_FILE_ATTRIBUTES,
				   DOS_ERROR_ACCESS_DENIED);
		return DOS_INT21_HANDLED;
	}
	if (operation != INT21_FILE_ATTRIBUTES_GET) {
		int21_return_error(context, registers, INT21_FILE_ATTRIBUTES,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	if (context->file_attributes == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	for (path_length = 0u; path_length < sizeof(path); ++path_length) {
		if (dos_machine_read_far(&context->machine, registers->ds, offset,
					 &path[path_length], sizeof(path[path_length]),
					 sizeof(path[path_length])) != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		offset = (uint16_t)(offset + 1u);
		if (path[path_length] == 0u) {
			++path_length;
			break;
		}
	}
	if (path_length == sizeof(path) && path[path_length - 1u] != 0u) {
		int21_return_error(context, registers, INT21_FILE_ATTRIBUTES,
				   DOS_ERROR_PATH_NOT_FOUND);
		return DOS_INT21_HANDLED;
	}
	error = context->file_attributes(context->file_attributes_context, path,
					 path_length, &attributes);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_FILE_ATTRIBUTES,
				   error);
		return DOS_INT21_HANDLED;
	}
	dos_register_set_low16(&registers->ecx, attributes);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_change_directory(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t path[INT21_PATH_CAPACITY];
	uint16_t offset = dos_register_low16(registers->edx);
	size_t path_length;
	enum dos_error error;

	if (context->change_directory == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	for (path_length = 0u; path_length < sizeof(path); ++path_length) {
		if (dos_machine_read_far(&context->machine, registers->ds, offset,
					 &path[path_length], 1u, 1u) !=
		    DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		offset = (uint16_t)(offset + 1u);
		if (path[path_length] == 0u) {
			++path_length;
			break;
		}
	}
	if (path_length == sizeof(path) && path[path_length - 1u] != 0u) {
		int21_return_error(context, registers, INT21_CHANGE_DIRECTORY,
				   DOS_ERROR_PATH_NOT_FOUND);
		return DOS_INT21_HANDLED;
	}
	error = context->change_directory(context->directory_context, path,
					  path_length);
	if (error != DOS_SUCCESS)
		int21_return_error(context, registers, INT21_CHANGE_DIRECTORY,
				   error);
	else
		int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_create_directory(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t path[INT21_PATH_CAPACITY];
	uint16_t offset = dos_register_low16(registers->edx);
	size_t path_length;
	enum dos_error error;

	if (context->create_directory == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	for (path_length = 0u; path_length < sizeof(path); ++path_length) {
		if (dos_machine_read_far(&context->machine, registers->ds, offset,
					 &path[path_length], 1u, 1u) !=
		    DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		offset = (uint16_t)(offset + 1u);
		if (path[path_length] == 0u) {
			++path_length;
			break;
		}
	}
	if (path_length == sizeof(path) && path[path_length - 1u] != 0u) {
		int21_return_error(context, registers, INT21_CREATE_DIRECTORY,
				   DOS_ERROR_PATH_NOT_FOUND);
		return DOS_INT21_HANDLED;
	}
	error = context->create_directory(context->create_directory_context,
					  path, path_length);
	if (error != DOS_SUCCESS)
		int21_return_error(context, registers, INT21_CREATE_DIRECTORY,
				   error);
	else
		int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_execute(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t path[INT21_PATH_CAPACITY];
	uint16_t offset = dos_register_low16(registers->edx);
	uint16_t attributes;
	size_t path_length;
	enum dos_error error;

	if (dos_register_low8(registers->eax) > 3u) {
		int21_return_error(context, registers, INT21_EXECUTE,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	if (context->file_attributes == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	for (path_length = 0u; path_length < sizeof(path); ++path_length) {
		if (dos_machine_read_far(&context->machine, registers->ds, offset,
					 &path[path_length], 1u, 1u) !=
		    DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		offset = (uint16_t)(offset + 1u);
		if (path[path_length] == 0u) {
			++path_length;
			break;
		}
	}
	if (path_length == sizeof(path) && path[path_length - 1u] != 0u) {
		int21_return_error(context, registers, INT21_EXECUTE,
				   DOS_ERROR_PATH_NOT_FOUND);
		return DOS_INT21_HANDLED;
	}
	error = context->file_attributes(context->file_attributes_context, path,
					 path_length, &attributes);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_EXECUTE, error);
		return DOS_INT21_HANDLED;
	}
	if ((attributes & 0x10u) != 0u) {
		int21_return_error(context, registers, INT21_EXECUTE,
				   DOS_ERROR_ACCESS_DENIED);
		return DOS_INT21_HANDLED;
	}
	/* Resolution succeeded.  The existing EXEC transaction needs a nested
	 * guest-backend publication binding before a real child can replace this
	 * caller;
	 * never forge success for an existing image. */
	return DOS_INT21_UNIMPLEMENTED;
}

static enum dos_int21_status dispatch_console_output(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t character = dos_register_low8(registers->edx);

	if (context->output_character == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (!context->output_character(context->output_context, character))
		return DOS_INT21_MACHINE_FAULT;
	/* Standard console output begins by copying DL to AL. */
	dos_register_set_low8(&registers->eax, character);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_raw_console_io(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t character;

	character = dos_register_low8(registers->edx);
	if (character != INT21_RAW_CONSOLE_INPUT_REQUEST) {
		if (context->output_character == NULL)
			return DOS_INT21_UNIMPLEMENTED;
		if (!context->output_character(context->output_context, character))
			return DOS_INT21_MACHINE_FAULT;
		dos_register_set_low8(&registers->eax, character);
		return DOS_INT21_HANDLED;
	}
	if (context->input_status == NULL || context->input_character == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (!context->input_status(context->input_context)) {
		dos_register_set_low8(&registers->eax, 0u);
		registers->eflags |= DOS_EFLAGS_ZF;
		return DOS_INT21_HANDLED;
	}
	if (!context->input_character(context->input_character_context,
				      &character))
		return DOS_INT21_MACHINE_FAULT;
	dos_register_set_low8(&registers->eax, character);
	registers->eflags &= ~DOS_EFLAGS_ZF;
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_console_input(
	struct dos_int21_context *context, struct dos_cpu_state *registers,
	bool echo)
{
	uint8_t character;

	if (context->input_character == NULL ||
	    (echo && context->output_character == NULL))
		return DOS_INT21_UNIMPLEMENTED;
	if (!context->input_character(context->input_character_context,
				      &character))
		return DOS_INT21_MACHINE_FAULT;
	if (echo &&
	    !context->output_character(context->output_context, character))
		return DOS_INT21_MACHINE_FAULT;
	dos_register_set_low8(&registers->eax, character);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_buffered_console_input(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t buffer[258] = {0};
	uint8_t rollback[258];
	uint16_t offset = dos_register_low16(registers->edx);
	size_t length = 0u;
	size_t count;
	uint8_t character;
	enum dos_machine_status status;

	if (context->input_character == NULL || context->output_character == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	status = dos_machine_read_far(&context->machine, registers->ds, offset,
				      buffer, sizeof(buffer), 2u);
	if (status != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	while (length < buffer[0]) {
		if (!context->input_character(context->input_character_context,
					      &character))
			return DOS_INT21_MACHINE_FAULT;
		if (character == (uint8_t)'\r' || character == (uint8_t)'\n')
			break;
		if (character == (uint8_t)'\b') {
			if (length != 0u) {
				--length;
				if (!context->output_character(context->output_context,
							      character) ||
				    !context->output_character(context->output_context,
							      (uint8_t)' ') ||
				    !context->output_character(context->output_context,
							      character))
					return DOS_INT21_MACHINE_FAULT;
			}
			continue;
		}
		if (character < 0x20u)
			continue;
		buffer[2u + length++] = character;
		if (!context->output_character(context->output_context, character))
			return DOS_INT21_MACHINE_FAULT;
	}
	buffer[1] = (uint8_t)length;
	buffer[2u + length] = (uint8_t)'\r';
	if (!context->output_character(context->output_context, (uint8_t)'\r') ||
	    !context->output_character(context->output_context, (uint8_t)'\n'))
		return DOS_INT21_MACHINE_FAULT;
	count = length + 3u;
	status = dos_machine_replace_far(&context->machine, registers->ds, offset,
					 buffer, sizeof(buffer), rollback,
					 sizeof(rollback), count);
	if (status == DOS_MACHINE_ROLLBACK_FAILED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	return status == DOS_MACHINE_OK ? DOS_INT21_HANDLED
					: DOS_INT21_MACHINE_FAULT;
}

static bool console_input_followup_is_available(
	const struct dos_int21_context *context,
	const struct dos_cpu_state *registers, uint8_t function)
{
	switch (function) {
	case INT21_STANDARD_INPUT_ECHO:
	case INT21_BUFFERED_CONSOLE_INPUT:
		return context->input_character != NULL &&
		       context->output_character != NULL;
	case INT21_RAW_CONSOLE_IO:
		return dos_register_low8(registers->edx) ==
			       INT21_RAW_CONSOLE_INPUT_REQUEST
			       ? context->input_status != NULL &&
					 context->input_character != NULL
			       : context->output_character != NULL;
	case INT21_RAW_CONSOLE_INPUT:
	case INT21_STANDARD_INPUT_DIRECT:
		return context->input_character != NULL;
	default:
		return true;
	}
}

static enum dos_int21_status dispatch_console_input_flush(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t function = dos_register_low8(registers->eax);

	if (context->input_flush == NULL ||
	    !console_input_followup_is_available(context, registers, function))
		return DOS_INT21_UNIMPLEMENTED;
	if (!context->input_flush(context->input_flush_context))
		return DOS_INT21_MACHINE_FAULT;

	/* Input flush redispatches 01h/06h/07h/08h/0Ah, but the return path copies
	 * only the resulting AL into user_AX.
	 * The caller-visible AH therefore remains 0Ch. */
	switch (function) {
	case INT21_STANDARD_INPUT_ECHO:
		return dispatch_console_input(context, registers, true);
	case INT21_RAW_CONSOLE_IO:
		return dispatch_raw_console_io(context, registers);
	case INT21_RAW_CONSOLE_INPUT:
	case INT21_STANDARD_INPUT_DIRECT:
		return dispatch_console_input(context, registers, false);
	case INT21_BUFFERED_CONSOLE_INPUT:
		return dispatch_buffered_console_input(context, registers);
	default:
		dos_register_set_low8(&registers->eax, 0u);
		return DOS_INT21_HANDLED;
	}
}

static enum dos_int21_status dispatch_string_output(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint16_t offset = dos_register_low16(registers->edx);
	uint32_t scanned;
	uint8_t character;

	for (scanned = 0u; scanned < INT21_STRING_SCAN_LIMIT; ++scanned) {
		if (dos_machine_read_far(&context->machine, registers->ds, offset,
					 &character, sizeof(character),
					 sizeof(character)) != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		offset = (uint16_t)(offset + 1u);
		if (character == (uint8_t)'$') {
			/* The terminating byte remains in AL. */
			dos_register_set_low8(&registers->eax, character);
			return DOS_INT21_HANDLED;
		}
		if (!context->output_character(context->output_context, character))
			return DOS_INT21_MACHINE_FAULT;
	}
	return DOS_INT21_MACHINE_FAULT;
}

static enum dos_int21_status dispatch_write_handle(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	struct int21_resolved_handle resolved;
	uint16_t count = dos_register_low16(registers->ecx);
	uint16_t offset = dos_register_low16(registers->edx);
	uint16_t transferred = 0u;
	enum int21_handle_status handle_status;

	handle_status = int21_resolve_handle(
		context, dos_register_low16(registers->ebx), &resolved);
	if (handle_status != INT21_HANDLE_OK)
		return int21_return_handle_status(
			context, registers, INT21_WRITE_HANDLE, handle_status);
	if (!handle_allows_write(&resolved.sft)) {
		int21_return_error(context, registers, INT21_WRITE_HANDLE,
				   DOS_ERROR_ACCESS_DENIED);
		return DOS_INT21_HANDLED;
	}
	if (dos_machine_validate_far(&context->machine, registers->ds, offset,
				     count) != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	if (resolved.sft.backend_kind == DOS_SFT_BACKEND_STANDARD) {
		uint8_t character;

		if (context->output_character == NULL)
			return DOS_INT21_UNIMPLEMENTED;
		for (transferred = 0u; transferred < count; ++transferred) {
			if (dos_machine_read_far(
				    &context->machine, registers->ds,
				    (uint16_t)(offset + transferred), &character,
				    sizeof(character), sizeof(character)) !=
			    DOS_MACHINE_OK)
				return DOS_INT21_MACHINE_FAULT;
			if (!context->output_character(context->output_context,
						       character))
				return DOS_INT21_MACHINE_FAULT;
		}
		dos_register_set_low16(&registers->eax, count);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (resolved.sft.backend_kind == DOS_SFT_BACKEND_DEVICE) {
		uint16_t information = resolved.sft.information;

		while (transferred < count) {
			uint8_t buffer[INT21_FILE_TRANSFER_BYTES];
			size_t amount = (size_t)count - transferred;
			size_t bytes_written = 0u;
			enum iomgr_status status;

			if (amount > sizeof(buffer))
				amount = sizeof(buffer);
			if (dos_machine_read_far(
				    &context->machine, registers->ds,
				    (uint16_t)(offset + transferred), buffer,
				    amount, amount) != DOS_MACHINE_OK)
				return DOS_INT21_MACHINE_FAULT;
			status = iomgr_device_write(
				resolved.sft.backend_handle, buffer, amount, amount,
				&bytes_written);
			if (status != IOMGR_OK) {
				if (status == IOMGR_UNCERTAIN ||
				    status == IOMGR_POISONED) {
					int21_quarantine_sft(
						context,
						resolved.sft.reference_handle);
					return return_iomgr_device_error(
						context, registers,
						INT21_WRITE_HANDLE, status,
						DOS_ERROR_ACCESS_DENIED);
				}
				if (transferred != 0u)
					break;
				return return_iomgr_device_error(
					context, registers, INT21_WRITE_HANDLE,
					status,
					status == IOMGR_STALE_HANDLE
						? DOS_ERROR_INVALID_HANDLE
						: DOS_ERROR_ACCESS_DENIED);
			}
			if (bytes_written > amount) {
				int21_quarantine_sft(
					context, resolved.sft.reference_handle);
				context->machine_poisoned = true;
				return DOS_INT21_MACHINE_POISONED;
			}
			information |= INT21_DEVICE_EOF;
			if (information != resolved.sft.information &&
			    int21_publish_sft_io(
				    context, &resolved.sft, resolved.sft.position,
				    resolved.sft.size, information) != DOS_INT21_HANDLED)
				return DOS_INT21_MACHINE_POISONED;
			transferred =
				(uint16_t)(transferred + bytes_written);
			if (bytes_written < amount)
				break;
		}
		dos_register_set_low16(&registers->eax, transferred);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (resolved.sft.backend_kind != DOS_SFT_BACKEND_FILE)
		return DOS_INT21_MACHINE_FAULT;
	if (context->file_ops == NULL || context->file_ops->write == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	{
		uint64_t position = resolved.sft.position;
		uint64_t size = resolved.sft.size;
		uint16_t information = resolved.sft.information;

	while (transferred < count) {
		uint8_t buffer[INT21_FILE_TRANSFER_BYTES];
		size_t amount = (size_t)count - transferred;
		size_t bytes_written = 0u;
		enum dos_error error;

		if (amount > sizeof(buffer))
			amount = sizeof(buffer);
		if (dos_machine_read_far(
			    &context->machine, registers->ds,
			    (uint16_t)(offset + transferred), buffer, sizeof(buffer),
			    amount) != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		error = context->file_ops->write(
			context->file_context, resolved.sft.backend_handle, position,
			buffer, sizeof(buffer), amount, &bytes_written);
		if (error != DOS_SUCCESS) {
			int21_return_error(context, registers, INT21_WRITE_HANDLE,
					   error);
			return DOS_INT21_HANDLED;
		}
		if (bytes_written > amount)
			return DOS_INT21_MACHINE_FAULT;
		position += bytes_written;
		if (bytes_written != 0u)
			information &= (uint16_t)~INT21_FILE_CLEAN;
		if (position > size)
			size = position;
		if (int21_publish_sft_io(
			    context, &resolved.sft, position, size, information) !=
		    DOS_INT21_HANDLED)
			return DOS_INT21_MACHINE_POISONED;
		transferred = (uint16_t)(transferred + bytes_written);
		if (bytes_written < amount)
			break;
	}
	}
	dos_register_set_low16(&registers->eax, transferred);
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static void dispatch_get_list_of_lists(struct dos_cpu_state *registers)
{
	registers->es = INT21_LIST_OF_LISTS_SEGMENT;
	dos_register_set_low16(&registers->ebx, INT21_LIST_OF_LISTS_OFFSET);
}

static void dispatch_get_indos_address(struct dos_cpu_state *registers)
{
	registers->es = INT21_INDOS_SEGMENT;
	dos_register_set_low16(&registers->ebx, INT21_INDOS_OFFSET);
}

static void int21_return_success(struct dos_cpu_state *registers)
{
	/* Successful return changes only the caller's carry flag. */
	registers->eflags &= ~DOS_EFLAGS_CF;
}

static void int21_return_error(struct dos_int21_context *context,
			       struct dos_cpu_state *registers,
			       uint8_t function, enum dos_error real_error)
{
	enum dos_error mapped_error;

	/* Record the real code before legacy mapping. */
	mapped_error =
	    dos_error_map_int21(function, real_error, &context->extended_error);
	dos_register_set_low16(&registers->eax, (uint16_t)mapped_error);
	registers->eflags |= DOS_EFLAGS_CF;
}

static enum dos_error memory_status_to_dos_error(enum dos_memory_status status,
						 enum dos_error invalid_error)
{
	switch (status) {
	case DOS_MEMORY_INVALID_ARGUMENT:
		return invalid_error;
	case DOS_MEMORY_INVALID_BLOCK:
		return DOS_ERROR_INVALID_BLOCK;
	case DOS_MEMORY_NOT_ENOUGH_MEMORY:
		return DOS_ERROR_NOT_ENOUGH_MEMORY;
	case DOS_MEMORY_ARENA_DAMAGED:
		return DOS_ERROR_ARENA_TRASHED;
	case DOS_MEMORY_OWNER_MISMATCH:
		return DOS_ERROR_ACCESS_DENIED;
	case DOS_MEMORY_IDENTITY_MISMATCH:
	case DOS_MEMORY_GENERATION_EXHAUSTED:
		return DOS_ERROR_ARENA_TRASHED;
	case DOS_MEMORY_OK:
	case DOS_MEMORY_MACHINE_FAULT:
	case DOS_MEMORY_MACHINE_POISONED:
		break;
	}
	return DOS_ERROR_ARENA_TRASHED;
}

static enum dos_int21_status
dispatch_memory_failure(struct dos_int21_context *context,
			struct dos_cpu_state *registers, uint8_t function,
			enum dos_memory_status status,
			enum dos_error invalid_error)
{
	if (status == DOS_MEMORY_MACHINE_FAULT)
		return DOS_INT21_MACHINE_FAULT;
	if (status == DOS_MEMORY_MACHINE_POISONED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	int21_return_error(context, registers, function,
			   memory_status_to_dos_error(status, invalid_error));
	return DOS_INT21_HANDLED;
}

static void dispatch_get_version(const struct dos_int21_context *context,
				 struct dos_cpu_state *registers)
{
	uint16_t version = (uint16_t)DOS_INT21_VERSION_MAJOR |
			   ((uint16_t)DOS_INT21_VERSION_MINOR << 8);
	uint16_t identity_high =
	    (uint16_t)((context->user_number >> 16) & 0xffu) |
	    ((uint16_t)context->oem_number << 8);

	/* Return AL:AH, BH and BL:CX, not packed host values. */
	dos_register_set_low16(&registers->eax, version);
	dos_register_set_low16(&registers->ebx, identity_high);
	dos_register_set_low16(&registers->ecx, (uint16_t)context->user_number);
	/* GET_VERSION returns normally rather than transferring to SYS_RET_OK.
	 */
}

static enum dos_int21_status dispatch_get_dpb(
	struct dos_int21_context *context, struct dos_cpu_state *registers,
	bool default_drive)
{
	uint16_t segment;
	uint16_t offset;
	uint8_t drive = default_drive ? 0u : dos_register_low8(registers->edx);

	if (context->get_dpb == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	/* Drive lookup returns AL=FFh for any unavailable drive;
	 * it does not route the failure through the carry/error mapper. */
	if (context->get_dpb(context->dpb_context, drive, &segment, &offset) !=
	    DOS_SUCCESS) {
		dos_register_set_low8(&registers->eax, 0xffu);
		return DOS_INT21_HANDLED;
	}
	registers->ds = segment;
	dos_register_set_low16(&registers->ebx, offset);
	dos_register_set_low8(&registers->eax, 0u);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_get_disk_free_space(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	struct dos_int21_disk_space space;
	uint64_t total_clusters;
	uint64_t free_clusters;
	uint32_t sectors_per_cluster;
	uint32_t unit_mask;
	uint8_t drive = dos_register_low8(registers->edx);

	if (context->get_disk_space == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (context->get_disk_space(context->disk_space_context, drive,
				    &space) != DOS_SUCCESS ||
	    space.reserved != 0u || space.allocation_unit_bytes < 512u ||
	    (space.allocation_unit_bytes &
	     (space.allocation_unit_bytes - 1u)) != 0u ||
	    (space.allocation_unit_bytes & 511u) != 0u ||
	    space.free_bytes > space.total_bytes) {
		dos_register_set_low16(&registers->eax, 0xffffu);
		return DOS_INT21_HANDLED;
	}
	unit_mask = space.allocation_unit_bytes - 1u;
	if ((space.total_bytes & unit_mask) != 0u ||
	    (space.free_bytes & unit_mask) != 0u) {
		dos_register_set_low16(&registers->eax, 0xffffu);
		return DOS_INT21_HANDLED;
	}
	total_clusters = space.total_bytes;
	free_clusters = space.free_bytes;
	for (uint32_t divisor = space.allocation_unit_bytes; divisor > 1u;
	     divisor >>= 1u) {
		total_clusters >>= 1u;
		free_clusters >>= 1u;
	}
	sectors_per_cluster = space.allocation_unit_bytes >> 9u;
	/* AH=36h has only 16-bit cluster counts.  Match DOS FAT32 practice by
	 * increasing the reported allocation unit until the tuple fits. */
	while (total_clusters > 0xfff6u && sectors_per_cluster <= 0x7fffu) {
		sectors_per_cluster <<= 1u;
		total_clusters >>= 1u;
		free_clusters >>= 1u;
	}
	if (sectors_per_cluster == 0u || sectors_per_cluster > 0xffffu) {
		dos_register_set_low16(&registers->eax, 0xffffu);
		return DOS_INT21_HANDLED;
	}
	if (total_clusters > 0xfff6u)
		total_clusters = 0xfff6u;
	if (free_clusters > 0xfff6u)
		free_clusters = 0xfff6u;
	dos_register_set_low16(&registers->eax,
			       (uint16_t)sectors_per_cluster);
	dos_register_set_low16(&registers->ebx, (uint16_t)free_clusters);
	dos_register_set_low16(&registers->ecx, 512u);
	dos_register_set_low16(&registers->edx, (uint16_t)total_clusters);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status
dispatch_set_interrupt_vector(struct dos_int21_context *context,
			      const struct dos_cpu_state *registers)
{
	struct dos_far_pointer16 address = {
	    .offset = dos_register_low16(registers->edx),
	    .segment = registers->ds,
	};
	enum dos_vector_status status;

	status = dos_vector_set(&context->machine,
				dos_register_low8(registers->eax), address);
	if (status == DOS_VECTOR_ROLLBACK_FAILED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (status != DOS_VECTOR_OK)
		return DOS_INT21_MACHINE_FAULT;
	/* Ordinary table dispatch initializes the per-call error locus. */
	context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status
dispatch_get_interrupt_vector(struct dos_int21_context *context,
			      struct dos_cpu_state *registers)
{
	struct dos_far_pointer16 address;

	if (dos_vector_get(&context->machine, dos_register_low8(registers->eax),
			   &address) != DOS_VECTOR_OK)
		return DOS_INT21_MACHINE_FAULT;
	dos_register_set_low16(&registers->ebx, address.offset);
	registers->es = address.segment;
	context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status
set_runtime_current_psp(struct dos_int21_context *context, uint16_t current_psp)
{
	enum dos_process_runtime_status status;

	status = dos_process_runtime_set_current_psp(&context->process_runtime,
						     current_psp);
	if (status == DOS_PROCESS_RUNTIME_OK)
		return DOS_INT21_HANDLED;
	if (status == DOS_PROCESS_RUNTIME_POISONED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (status == DOS_PROCESS_RUNTIME_GENERATION_EXHAUSTED) {
		status = dos_process_runtime_poison(&context->process_runtime);
		context->machine_poisoned = true;
		if (status != DOS_PROCESS_RUNTIME_OK &&
		    status != DOS_PROCESS_RUNTIME_POISONED)
			return DOS_INT21_MACHINE_POISONED;
		return DOS_INT21_MACHINE_POISONED;
	}
	return DOS_INT21_INVALID_ARGUMENT;
}

static enum dos_int21_status dispatch_set_dta(
	struct dos_int21_context *context, const struct dos_cpu_state *registers)
{
	struct dos_far_pointer16 dta = {
		.offset = dos_register_low16(registers->edx),
		.segment = registers->ds,
	};
	enum dos_process_runtime_status status =
		dos_process_runtime_set_dta(&context->process_runtime, dta);

	if (status == DOS_PROCESS_RUNTIME_OK)
		return DOS_INT21_HANDLED;
	if (status == DOS_PROCESS_RUNTIME_POISONED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	if (status == DOS_PROCESS_RUNTIME_GENERATION_EXHAUSTED) {
		status = dos_process_runtime_poison(&context->process_runtime);
		context->machine_poisoned = true;
		if (status != DOS_PROCESS_RUNTIME_OK &&
		    status != DOS_PROCESS_RUNTIME_POISONED)
			return DOS_INT21_MACHINE_POISONED;
		return DOS_INT21_MACHINE_POISONED;
	}
	return DOS_INT21_INVALID_ARGUMENT;
}

static void dispatch_get_dta(const struct dos_int21_context *context,
			     struct dos_cpu_state *registers)
{
	dos_register_set_low16(&registers->ebx,
			       context->process_runtime.dta.offset);
	registers->es = context->process_runtime.dta.segment;
}

static enum dos_int21_status publish_find_record(
	struct dos_int21_context *context, const struct dos_find_record *record)
{
	uint8_t replacement[DOS_DTA_FIND_SIZE];
	uint8_t rollback[DOS_DTA_FIND_SIZE];
	enum dos_machine_status status;

	if (dos_find_record_encode(record, replacement) != DOS_FIND_OK)
		return DOS_INT21_MACHINE_FAULT;
	status = dos_machine_replace_far(
		&context->machine, context->process_runtime.dta.segment,
		context->process_runtime.dta.offset, replacement,
		sizeof(replacement), rollback, sizeof(rollback),
		sizeof(replacement));
	if (status == DOS_MACHINE_ROLLBACK_FAILED) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	return status == DOS_MACHINE_OK ? DOS_INT21_HANDLED
					: DOS_INT21_MACHINE_FAULT;
}

static enum dos_int21_status dispatch_find_first(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t path[INT21_PATH_CAPACITY];
	struct dos_find_record record;
	uint16_t offset = dos_register_low16(registers->edx);
	size_t path_length;
	enum dos_error error;
	enum dos_int21_status status;

	for (path_length = 0u; path_length < sizeof(path); ++path_length) {
		if (dos_machine_read_far(&context->machine, registers->ds,
					 (uint16_t)(offset + path_length),
					 &path[path_length], 1u, 1u) !=
		    DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		if (path[path_length] == 0u) {
			++path_length;
			break;
		}
	}
	if (path_length == sizeof(path) && path[path_length - 1u] != 0u) {
		int21_return_error(context, registers, INT21_FIND_FIRST,
				   DOS_ERROR_PATH_NOT_FOUND);
		return DOS_INT21_HANDLED;
	}
	error = context->find_ops->first(
		context->find_context, path, path_length,
		dos_register_low8(registers->ecx), &record);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_FIND_FIRST, error);
		return DOS_INT21_HANDLED;
	}
	status = publish_find_record(context, &record);
	if (status != DOS_INT21_HANDLED)
		return status;
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_find_next(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t encoded[DOS_DTA_FIND_SIZE];
	struct dos_find_record previous;
	struct dos_find_record record;
	enum dos_error error;
	enum dos_int21_status status;

	if (dos_machine_read_far(&context->machine,
				 context->process_runtime.dta.segment,
				 context->process_runtime.dta.offset, encoded,
				 sizeof(encoded), sizeof(encoded)) != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	if (dos_find_record_decode(encoded, &previous) != DOS_FIND_OK)
		return DOS_INT21_MACHINE_FAULT;
	error = context->find_ops->next(context->find_context, &previous, &record);
	if (error != DOS_SUCCESS) {
		int21_return_error(context, registers, INT21_FIND_NEXT, error);
		return DOS_INT21_HANDLED;
	}
	status = publish_find_record(context, &record);
	if (status != DOS_INT21_HANDLED)
		return status;
	int21_return_success(registers);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status
dispatch_set_current_psp(struct dos_int21_context *context,
			 const struct dos_cpu_state *registers)
{
	/* SET_CURRENT_PDB accepts every 16-bit BX value verbatim. */
	return set_runtime_current_psp(context,
				       dos_register_low16(registers->ebx));
}

static enum dos_int21_status
dispatch_allocate(struct dos_int21_context *context,
		  struct dos_cpu_state *registers)
{
	uint16_t requested = dos_register_low16(registers->ebx);
	struct dos_memory_allocation_result result;
	enum dos_memory_status status;

	if (!machine_is_usable(&context->machine))
		return DOS_INT21_MACHINE_FAULT;
	status = dos_memory_allocate_checked(
	    &context->memory_arena, &context->machine,
	    context->process_runtime.current_psp, requested, &result);
	if (status == DOS_MEMORY_OK) {
		context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
		dos_register_set_low16(&registers->eax, result.block_segment);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	/* Allocation failure copies the largest block size to BX. */
	if (status == DOS_MEMORY_NOT_ENOUGH_MEMORY)
		dos_register_set_low16(&registers->ebx,
				       result.maximum_available);
	return dispatch_memory_failure(context, registers,
				       INT21_ALLOCATE_MEMORY, status,
				       DOS_ERROR_INVALID_PARAMETER);
}

static enum dos_int21_status dispatch_free(struct dos_int21_context *context,
					   struct dos_cpu_state *registers)
{
	enum dos_memory_status status;

	if (!machine_is_usable(&context->machine))
		return DOS_INT21_MACHINE_FAULT;
	status = dos_memory_free_checked(&context->memory_arena,
					 &context->machine, registers->es);
	if (status == DOS_MEMORY_OK) {
		context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	return dispatch_memory_failure(context, registers, INT21_FREE_MEMORY,
				       status, DOS_ERROR_INVALID_BLOCK);
}

static enum dos_int21_status dispatch_resize(struct dos_int21_context *context,
					     struct dos_cpu_state *registers)
{
	uint16_t maximum_available;
	enum dos_memory_status status;

	if (!machine_is_usable(&context->machine))
		return DOS_INT21_MACHINE_FAULT;
	status = dos_memory_resize_checked(
	    &context->memory_arena, &context->machine, registers->es,
	    context->process_runtime.current_psp,
	    dos_register_low16(registers->ebx), &maximum_available);
	if (status == DOS_MEMORY_OK) {
		/*
		 * SETBLOCK rejoins the size-return path, which leaves
		 * AX equal to the resized data segment. MS-DOS preserves the
		 * same undocumented result because several old runtimes consume it.
		 */
		dos_register_set_low16(&registers->eax, registers->es);
		context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	/* SETBLOCK writes the coalesced maximum only on the grow-failure path.
	 */
	if (status == DOS_MEMORY_NOT_ENOUGH_MEMORY)
		dos_register_set_low16(&registers->ebx, maximum_available);
	return dispatch_memory_failure(context, registers, INT21_RESIZE_MEMORY,
				       status, DOS_ERROR_INVALID_BLOCK);
}

static enum dos_int21_status dispatch_set_handle_count(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	enum dos_jft_status status = dos_jft_resize_checked(
		&context->memory_arena, &context->machine,
		context->process_runtime.current_psp,
		dos_register_low16(registers->ebx));

	switch (status) {
	case DOS_JFT_OK:
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	case DOS_JFT_TOO_MANY_OPEN_FILES:
		int21_return_error(context, registers, INT21_SET_HANDLE_COUNT,
				   DOS_ERROR_TOO_MANY_OPEN_FILES);
		return DOS_INT21_HANDLED;
	case DOS_JFT_NOT_ENOUGH_MEMORY:
		int21_return_error(context, registers, INT21_SET_HANDLE_COUNT,
				   DOS_ERROR_NOT_ENOUGH_MEMORY);
		return DOS_INT21_HANDLED;
	case DOS_JFT_INVALID_FUNCTION:
		int21_return_error(context, registers, INT21_SET_HANDLE_COUNT,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	case DOS_JFT_INVALID_STATE:
	case DOS_JFT_ARENA_FAULT:
		/* A forged external pointer must never release another PSP's MCB.
		 * Preserve AH=67h's legacy AX allowlist while function 59h retains
		 * the stronger arena-corruption diagnosis. */
		int21_return_error(context, registers, INT21_SET_HANDLE_COUNT,
				   DOS_ERROR_ARENA_TRASHED);
		return DOS_INT21_HANDLED;
	case DOS_JFT_MACHINE_FAULT:
	case DOS_JFT_INVALID_ARGUMENT:
		return DOS_INT21_MACHINE_FAULT;
	case DOS_JFT_MACHINE_POISONED:
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	return DOS_INT21_MACHINE_FAULT;
}

static void dispatch_get_current_psp(const struct dos_int21_context *context,
				     struct dos_cpu_state *registers)
{
	dos_register_set_low16(&registers->ebx,
			       context->process_runtime.current_psp);
	/* Functions 51h/62h return directly from the entry stack. */
}

static enum dos_int21_status
dispatch_allocation_operation(struct dos_int21_context *context,
			      struct dos_cpu_state *registers)
{
	uint8_t operation = dos_register_low8(registers->eax);
	enum dos_memory_status status;

	if (operation == INT21_ALLOC_GET) {
		uint8_t strategy;

		status = dos_memory_get_strategy_checked(&context->memory_arena,
							 &strategy);
		if (status == DOS_MEMORY_OK) {
			/* The allocation-strategy get operation explicitly zeroes AH.
			 */
			dos_register_set_low16(&registers->eax,
					       (uint16_t)strategy);
			context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
			int21_return_success(registers);
			return DOS_INT21_HANDLED;
		}
	} else if (operation == INT21_ALLOC_SET) {
		status = dos_memory_set_strategy_checked(
		    &context->memory_arena, dos_register_low8(registers->ebx));
		if (status == DOS_MEMORY_OK) {
			/* The assembly leaves AX=5801h and all data registers
			 * intact. */
			context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
			int21_return_success(registers);
			return DOS_INT21_HANDLED;
		}
	} else {
		/* Invalid AllocOper is the call-specific memory locus. */
		context->extended_error.locus = DOS_ERROR_LOCUS_MEMORY;
		int21_return_error(context, registers,
				   INT21_ALLOCATION_OPERATION,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	}
	return dispatch_memory_failure(context, registers,
				       INT21_ALLOCATION_OPERATION, status,
				       DOS_ERROR_INVALID_FUNCTION);
}

static void dispatch_get_extended_error(const struct dos_int21_context *context,
					struct dos_cpu_state *registers)
{
	uint16_t error_metadata =
	    (uint16_t)context->extended_error.action |
	    ((uint16_t)context->extended_error.error_class << 8);

	dos_register_set_low16(&registers->eax, context->extended_error.code);
	dos_register_set_low16(&registers->ebx, error_metadata);
	/* This service writes CH only; CL remains a caller value. */
	dos_register_set_high8(&registers->ecx, context->extended_error.locus);
	dos_register_set_low16(&registers->edi, context->extended_error_offset);
	registers->es = context->extended_error_segment;
	int21_return_success(registers);
}

static void dispatch_get_child_return(struct dos_int21_context *context,
				      struct dos_cpu_state *registers)
{
	/* WAIT returns the termination tuple once, then clears it. */
	dos_register_set_low16(&registers->eax, context->child_return_code);
	context->child_return_code = 0u;
	int21_return_success(registers);
}

static enum dos_int21_status dispatch_get_set_code_page(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t operation = dos_register_low8(registers->eax);
	struct dos_nls_switch transaction;
	uint8_t replacement[INT21_NLS_STORAGE_BYTES];
	uint8_t rollback[INT21_NLS_STORAGE_BYTES];
	enum dos_machine_status machine_status;

	if (operation == INT21_NLS_GET_CODE_PAGE) {
		/* The code-page service publishes only BX and DX, then uses the
		 * ordinary carry-only success return. */
		dos_register_set_low16(&registers->ebx,
				       context->nls.active->code_page);
		dos_register_set_low16(&registers->edx,
				       context->nls.system_code_page);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (operation == INT21_NLS_SET_CODE_PAGE &&
	    dos_nls_prepare_switch(&context->nls,
				   dos_register_low16(registers->ebx),
				   &transaction)) {
		if (transaction.target == context->nls.active) {
			if (!dos_nls_commit_switch(&context->nls, &transaction))
				return DOS_INT21_MACHINE_FAULT;
			int21_return_success(registers);
			return DOS_INT21_HANDLED;
		}
		if (!build_nls_storage(transaction.target, replacement,
				       sizeof(replacement))) {
			dos_nls_abort_switch(&transaction);
			return DOS_INT21_MACHINE_FAULT;
		}
		machine_status = dos_machine_replace_far(
			&context->machine, INT21_NLS_SEGMENT, 0u, replacement,
			sizeof(replacement), rollback, sizeof(rollback),
			sizeof(replacement));
		if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
			dos_nls_abort_switch(&transaction);
			context->machine_poisoned = true;
			return DOS_INT21_MACHINE_POISONED;
		}
		if (machine_status != DOS_MACHINE_OK) {
			dos_nls_abort_switch(&transaction);
			return DOS_INT21_MACHINE_FAULT;
		}
		if (!dos_nls_commit_switch(&context->nls, &transaction)) {
			if (dos_machine_write_far(&context->machine,
						  INT21_NLS_SEGMENT, 0u,
						  rollback, sizeof(rollback),
						  sizeof(rollback)) !=
			    DOS_MACHINE_OK) {
				context->machine_poisoned = true;
				return DOS_INT21_MACHINE_POISONED;
			}
			return DOS_INT21_MACHINE_FAULT;
		}
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	int21_return_error(context, registers, INT21_GET_SET_CODE_PAGE,
			   DOS_ERROR_INVALID_FUNCTION);
	return DOS_INT21_HANDLED;
}

static enum dos_int21_status dispatch_extended_code_system(
	struct dos_int21_context *context, struct dos_cpu_state *registers)
{
	uint8_t operation = dos_register_low8(registers->eax);

	if (operation == 0u) {
		/* The ECS service returns the bytes after the DBCS table's
		 * length word.  Keep AH=65h/AL=07h pointing at the full structure
		 * while AX=6300h exposes the range-pair data expected by callers. */
		registers->ds = INT21_NLS_SEGMENT;
		dos_register_set_low16(&registers->esi,
				       INT21_NLS_DBCS_DATA_OFFSET);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (operation == 1u) {
		context->interim_console_mode =
			(dos_register_low8(registers->edx) & 1u) != 0u;
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	if (operation == 2u) {
		dos_register_set_low8(&registers->edx,
				context->interim_console_mode ? 1u : 0u);
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	int21_return_error(context, registers, INT21_EXTENDED_CODE_SYSTEM,
			   DOS_ERROR_INVALID_FUNCTION);
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_context_initialize(
    struct dos_int21_context *context, const struct dos_machine *machine,
    const struct dos_memory_arena *memory_arena,
    kernel_object_handle_t runtime_identity, uint16_t current_psp,
    const struct dos_int21_drive_config *drive_config)
{
	struct dos_int21_context initialized;
	struct dos_far_pointer16 initial_dta = {
	    .offset = DOS_PSP_COMMAND_TAIL_OFFSET,
	    .segment = current_psp,
	};
	uint8_t first_mcb[2];
	uint8_t indos = 0u;
	uint8_t nls_storage[INT21_NLS_STORAGE_BYTES];

	if (context == NULL || memory_arena == NULL ||
	    !dos_int21_drive_config_is_valid(drive_config) ||
	    !memory_arena->constructed || !memory_arena->initialized)
		return DOS_INT21_INVALID_ARGUMENT;
	if (memory_arena->machine_poisoned)
		return DOS_INT21_MACHINE_POISONED;
	if (!machine_is_usable(machine))
		return DOS_INT21_MACHINE_FAULT;
	initialized = (struct dos_int21_context){
	    .machine = *machine,
	    .memory_arena = *memory_arena,
	    .process_runtime = DOS_PROCESS_RUNTIME_INITIALIZER,
	    .extended_error = {0u, 0u, 0u, 0u},
	    .user_number = 0u,
	    .extended_error_segment = 0u,
	    .extended_error_offset = 0u,
	    .child_return_code = 0u,
	    .input_status = NULL,
	    .input_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .input_character = NULL,
	    .input_character_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .input_flush = NULL,
	    .input_flush_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .file_attributes = NULL,
	    .file_attributes_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .file_ops = NULL,
	    .file_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .change_directory = NULL,
	    .directory_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .create_directory = NULL,
	    .create_directory_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .get_current_directory = NULL,
	    .current_directory_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .get_dpb = NULL,
	    .dpb_context = KERNEL_OBJECT_HANDLE_INVALID,
		    .find_ops = NULL,
		    .find_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .sft_context = KERNEL_OBJECT_HANDLE_INVALID,
	    .available_drive_mask = drive_config->available_drive_mask,
	    .oem_number = DOS_INT21_DEFAULT_OEM_NUMBER,
	    .current_drive = drive_config->current_drive,
	    .boot_drive = drive_config->boot_drive,
	    .last_drive = drive_config->last_drive,
	    .sft_services_bound = false,
	    .interim_console_mode = false,
	    .break_enabled = false,
	    .machine_poisoned = false,
		    .initialized = true,
	};
	if (!dos_nls_runtime_initialize(&initialized.nls,
					INT21_NLS_SYSTEM_CODE_PAGE,
					INT21_NLS_SYSTEM_CODE_PAGE))
		return DOS_INT21_INVALID_ARGUMENT;
	if (dos_process_runtime_construct(&initialized.process_runtime) !=
		DOS_PROCESS_RUNTIME_OK ||
	    dos_process_runtime_initialize(
		&initialized.process_runtime, runtime_identity, current_psp,
		initial_dta) != DOS_PROCESS_RUNTIME_OK)
		return DOS_INT21_INVALID_ARGUMENT;
	first_mcb[0] = (uint8_t)memory_arena->head_segment;
	first_mcb[1] = (uint8_t)(memory_arena->head_segment >> 8u);
	if (dos_machine_write_far(&initialized.machine,
				  INT21_LIST_OF_LISTS_SEGMENT,
				  INT21_LIST_OF_LISTS_OFFSET - sizeof(first_mcb),
				  first_mcb, sizeof(first_mcb),
				  sizeof(first_mcb)) != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	/* Service delivery is currently non-reentrant and hardware IRQs are
	 * injected only between service steps, so the externally observable
	 * count is idle.  Retain a stable byte for later reentrancy bracketing. */
	if (dos_machine_write_far(&initialized.machine, INT21_INDOS_SEGMENT,
				  INT21_INDOS_OFFSET, &indos, sizeof(indos),
				  sizeof(indos)) != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	if (!build_nls_storage(initialized.nls.active, nls_storage,
			       sizeof(nls_storage)))
		return DOS_INT21_INVALID_ARGUMENT;
	if (dos_machine_write_far(&initialized.machine, INT21_NLS_SEGMENT, 0u,
				  nls_storage, sizeof(nls_storage),
				  sizeof(nls_storage)) != DOS_MACHINE_OK)
		return DOS_INT21_MACHINE_FAULT;
	*context = initialized;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status
dos_int21_set_current_psp(struct dos_int21_context *context,
			  uint16_t current_psp)
{
	if (context == NULL || !context->initialized)
		return DOS_INT21_INVALID_ARGUMENT;
	return set_runtime_current_psp(context, current_psp);
}

enum dos_int21_status dos_int21_set_console_output(
	struct dos_int21_context *context,
	dos_int21_output_character_fn output_character,
	kernel_object_handle_t output_context)
{
	if (!context_is_initialized(context) || output_character == NULL ||
	    output_context == 0u ||
	    output_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->output_character = output_character;
	context->output_context = output_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_console_input_status(
	struct dos_int21_context *context,
	dos_int21_input_status_fn input_status,
	kernel_object_handle_t input_context)
{
	if (!context_is_initialized(context) || input_status == NULL ||
	    input_context == 0u ||
	    input_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->input_status = input_status;
	context->input_context = input_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_console_input(
	struct dos_int21_context *context,
	dos_int21_input_character_fn input_character,
	kernel_object_handle_t input_context)
{
	if (!context_is_initialized(context) || input_character == NULL ||
	    input_context == 0u ||
	    input_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->input_character = input_character;
	context->input_character_context = input_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_console_input_flush(
	struct dos_int21_context *context,
	dos_int21_input_flush_fn input_flush,
	kernel_object_handle_t input_context)
{
	if (!context_is_initialized(context) || input_flush == NULL ||
	    input_context == 0u ||
	    input_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->input_flush = input_flush;
	context->input_flush_context = input_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_file_attributes_query(
	struct dos_int21_context *context,
	dos_int21_file_attributes_fn file_attributes,
	kernel_object_handle_t file_attributes_context)
{
	if (!context_is_initialized(context) || file_attributes == NULL ||
	    file_attributes_context == 0u ||
	    file_attributes_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->file_attributes = file_attributes;
	context->file_attributes_context = file_attributes_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_file_services(
	struct dos_int21_context *context,
	const struct dos_int21_file_ops *file_ops,
	kernel_object_handle_t file_context)
{
	if (!context_is_initialized(context) || file_ops == NULL ||
	    file_ops->open == NULL || file_ops->create == NULL ||
	    file_ops->read == NULL || file_ops->write == NULL ||
	    file_ops->get_time == NULL || file_ops->set_time == NULL ||
	    file_ops->rename == NULL || file_ops->close == NULL ||
	    file_context == 0u ||
	    file_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->file_ops = file_ops;
	context->file_context = file_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_bind_sft_services(
	struct dos_int21_context *context, kernel_object_handle_t sft_context)
{
	if (!context_is_initialized(context) || sft_context == 0u ||
	    sft_context == KERNEL_OBJECT_HANDLE_INVALID ||
	    dos_sft_registry_context() != sft_context)
		return DOS_INT21_INVALID_ARGUMENT;
	context->sft_context = sft_context;
	context->sft_services_bound = true;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_directory_change(
	struct dos_int21_context *context,
	dos_int21_change_directory_fn change_directory,
	kernel_object_handle_t directory_context)
{
	if (!context_is_initialized(context) || change_directory == NULL ||
	    directory_context == 0u ||
	    directory_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->change_directory = change_directory;
	context->directory_context = directory_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_directory_create(
	struct dos_int21_context *context,
	dos_int21_create_directory_fn create_directory,
	kernel_object_handle_t directory_context)
{
	if (!context_is_initialized(context) || create_directory == NULL ||
	    directory_context == 0u ||
	    directory_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->create_directory = create_directory;
	context->create_directory_context = directory_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_current_directory_query(
	struct dos_int21_context *context,
	dos_int21_get_current_directory_fn get_current_directory,
	kernel_object_handle_t directory_context)
{
	if (!context_is_initialized(context) || get_current_directory == NULL ||
	    directory_context == 0u ||
	    directory_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->get_current_directory = get_current_directory;
	context->current_directory_context = directory_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_dpb_query(
	struct dos_int21_context *context, dos_int21_get_dpb_fn get_dpb,
	kernel_object_handle_t dpb_context)
{
	if (!context_is_initialized(context) || get_dpb == NULL ||
	    dpb_context == 0u || dpb_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->get_dpb = get_dpb;
	context->dpb_context = dpb_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_disk_space_query(
	struct dos_int21_context *context,
	dos_int21_get_disk_space_fn get_disk_space,
	kernel_object_handle_t disk_space_context)
{
	if (!context_is_initialized(context) || get_disk_space == NULL ||
	    disk_space_context == 0u ||
	    disk_space_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->get_disk_space = get_disk_space;
	context->disk_space_context = disk_space_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_set_find_services(
	struct dos_int21_context *context,
	const struct dos_int21_find_ops *find_ops,
	kernel_object_handle_t find_context)
{
	if (!context_is_initialized(context) || find_ops == NULL ||
	    find_ops->first == NULL || find_ops->next == NULL ||
	    find_context == 0u ||
	    find_context == KERNEL_OBJECT_HANDLE_INVALID)
		return DOS_INT21_INVALID_ARGUMENT;
	context->find_ops = find_ops;
	context->find_context = find_context;
	return DOS_INT21_HANDLED;
}

enum dos_int21_status
dos_int21_set_version_identity(struct dos_int21_context *context,
			       uint8_t oem_number, uint32_t user_number)
{
	if (context == NULL || !context->initialized ||
	    user_number > DOS_INT21_USER_NUMBER_MAXIMUM)
		return DOS_INT21_INVALID_ARGUMENT;
	context->oem_number = oem_number;
	context->user_number = user_number;
	return DOS_INT21_HANDLED;
}

void dos_int21_set_extended_error_pointer(struct dos_int21_context *context,
					  uint16_t segment, uint16_t offset)
{
	if (context == NULL || !context->initialized)
		return;
	context->extended_error_segment = segment;
	context->extended_error_offset = offset;
}

enum dos_int21_status dos_int21_publish_child_return(
	struct dos_int21_context *context, uint8_t exit_type, uint8_t exit_code)
{
	if (!context_is_initialized(context) || context->machine_poisoned)
		return DOS_INT21_INVALID_ARGUMENT;
	context->child_return_code =
		(uint16_t)exit_code | ((uint16_t)exit_type << 8);
	return DOS_INT21_HANDLED;
}

enum dos_int21_status dos_int21_dispatch(struct dos_int21_context *context,
					 struct dos_cpu_state *registers)
{
	uint8_t function;

	if (!context_is_initialized(context) || registers == NULL)
		return DOS_INT21_INVALID_ARGUMENT;
	if (context->machine_poisoned ||
	    context->memory_arena.machine_poisoned ||
	    context->process_runtime.poisoned) {
		context->machine_poisoned = true;
		return DOS_INT21_MACHINE_POISONED;
	}
	function = dos_register_high8(registers->eax);
	/*
	 * BadCall handles AH > MAXCOM before saving
	 * DOS state: only AL becomes zero; caller CF and every other register
	 * remain untouched.
	 */
	if (function > INT21_MAXIMUM_FUNCTION) {
		dos_register_set_low8(&registers->eax, 0u);
		return DOS_INT21_HANDLED;
	}
	/* CP/M compatibility holes have the same guest register result. */
	if (function_is_cpm_hole(function)) {
		context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;
		dos_register_set_low8(&registers->eax, 0u);
		return DOS_INT21_HANDLED;
	}
	/* A real table entry must be handled later, never forged as BadCall. */
	if (!function_is_implemented(function))
		return DOS_INT21_UNIMPLEMENTED;
	if ((function == INT21_STANDARD_CONSOLE_OUTPUT ||
	     function == INT21_STANDARD_STRING_OUTPUT) &&
	    context->output_character == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if ((function == INT21_CREATE_FILE || function == INT21_OPEN_FILE ||
	     function == INT21_CLOSE_FILE || function == INT21_READ_FILE ||
	     function == INT21_WRITE_HANDLE || function == INT21_SEEK_FILE ||
	     function == INT21_FILE_DATE_TIME ||
	     (function == INT21_DEVICE_CONTROL &&
	      (dos_register_low8(registers->eax) ==
		       INT21_IOCTL_GET_DEVICE_INFO ||
	       dos_register_low8(registers->eax) ==
		       INT21_IOCTL_CONTROL_READ ||
	       dos_register_low8(registers->eax) ==
		       INT21_IOCTL_CONTROL_WRITE))) &&
	    !context->sft_services_bound)
		return DOS_INT21_UNIMPLEMENTED;
	if (function == INT21_FILE_ATTRIBUTES &&
	    context->file_attributes == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (function == INT21_CHANGE_DIRECTORY &&
	    context->change_directory == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (function == INT21_CREATE_DIRECTORY &&
	    context->create_directory == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (function == INT21_GET_CURRENT_DIRECTORY &&
	    context->get_current_directory == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if ((function == INT21_GET_DEFAULT_DPB || function == INT21_GET_DPB) &&
	    context->get_dpb == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (function == INT21_GET_DISK_FREE_SPACE &&
	    context->get_disk_space == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if ((function == INT21_FIND_FIRST || function == INT21_FIND_NEXT) &&
	    context->find_ops == NULL)
		return DOS_INT21_UNIMPLEMENTED;
	if (function == INT21_DEVICE_CONTROL &&
	    dos_register_low8(registers->eax) != INT21_IOCTL_GET_DEVICE_INFO &&
	    dos_register_low8(registers->eax) != INT21_IOCTL_CONTROL_READ &&
	    dos_register_low8(registers->eax) != INT21_IOCTL_CONTROL_WRITE &&
	    dos_register_low8(registers->eax) != INT21_IOCTL_CHECK_REMOVABLE)
		return DOS_INT21_UNIMPLEMENTED;
	/* Reject a broken native boundary before changing registers or DOS
	 * state. */
	if ((function == INT21_SET_INTERRUPT_VECTOR ||
	     function == INT21_GET_INTERRUPT_VECTOR ||
	     function == INT21_STANDARD_STRING_OUTPUT ||
	     function == INT21_OPEN_FILE || function == INT21_READ_FILE ||
	     function == INT21_WRITE_HANDLE ||
	     (function == INT21_DEVICE_CONTROL &&
	      (dos_register_low8(registers->eax) ==
		       INT21_IOCTL_CONTROL_READ ||
	       dos_register_low8(registers->eax) ==
		       INT21_IOCTL_CONTROL_WRITE)) ||
	     function == INT21_ALLOCATE_MEMORY ||
	     function == INT21_FREE_MEMORY ||
	     function == INT21_RESIZE_MEMORY ||
	     function == INT21_SET_HANDLE_COUNT) &&
	    !machine_is_usable(&context->machine))
		return DOS_INT21_MACHINE_FAULT;
	/* Ordinary dispatch defaults the per-call error locus. */
	if (function != INT21_GET_EXTENDED_ERROR &&
	    function != INT21_SET_INTERRUPT_VECTOR &&
	    function != INT21_GET_INTERRUPT_VECTOR &&
	    function != INT21_SET_CURRENT_PDB &&
	    function != INT21_GET_CURRENT_PDB &&
	    function != INT21_GET_CURRENT_PSP &&
	    function != INT21_ALLOCATE_MEMORY &&
	    function != INT21_FREE_MEMORY && function != INT21_RESIZE_MEMORY &&
	    function != INT21_ALLOCATION_OPERATION)
		context->extended_error.locus = DOS_ERROR_LOCUS_UNKNOWN;

	switch (function) {
	case INT21_TERMINATE_COMPATIBLE:
	case INT21_TERMINATE:
		return DOS_INT21_PROCESS_EXITED;
	case INT21_STANDARD_INPUT_ECHO:
		return dispatch_console_input(context, registers, true);
	case INT21_STANDARD_CONSOLE_OUTPUT:
		return dispatch_console_output(context, registers);
	case INT21_RAW_CONSOLE_IO:
		return dispatch_raw_console_io(context, registers);
	case INT21_RAW_CONSOLE_INPUT:
	case INT21_STANDARD_INPUT_DIRECT:
		return dispatch_console_input(context, registers, false);
	case INT21_STANDARD_STRING_OUTPUT:
		return dispatch_string_output(context, registers);
	case INT21_BUFFERED_CONSOLE_INPUT:
		return dispatch_buffered_console_input(context, registers);
	case INT21_STANDARD_INPUT_STATUS:
		if (context->input_status == NULL)
			return DOS_INT21_UNIMPLEMENTED;
		dos_register_set_low8(
			&registers->eax,
			context->input_status(context->input_context) ? 0xffu : 0u);
		return DOS_INT21_HANDLED;
	case INT21_STANDARD_INPUT_FLUSH:
		return dispatch_console_input_flush(context, registers);
	case INT21_DISK_RESET:
		/* DISK_RESET reports no result. The current FAT layer
		 * has no guest-dirty cache, so there is nothing to flush or
		 * invalidate; preserve the complete caller register image. */
		return DOS_INT21_HANDLED;
	case INT21_SELECT_DEFAULT_DRIVE:
		if (drive_index_is_available(
			    context, dos_register_low8(registers->edx)))
			context->current_drive =
				dos_register_low8(registers->edx);
		dos_register_set_low8(&registers->eax, context->last_drive);
		return DOS_INT21_HANDLED;
	case INT21_GET_CURRENT_DRIVE:
		/* DOS drive numbers are zero-based for AH=19h. */
		dos_register_set_low8(&registers->eax, context->current_drive);
		return DOS_INT21_HANDLED;
	case INT21_SET_DTA:
		return dispatch_set_dta(context, registers);
	case INT21_GET_DEFAULT_DPB:
		return dispatch_get_dpb(context, registers, true);
	case INT21_PARSE_FILE_DESCRIPTOR:
		return dispatch_parse_file_descriptor(context, registers);
	case INT21_WRITE_HANDLE:
		return dispatch_write_handle(context, registers);
	case INT21_DEVICE_CONTROL:
		return dispatch_device_control(context, registers);
	case INT21_SET_INTERRUPT_VECTOR:
		return dispatch_set_interrupt_vector(context, registers);
	case INT21_GET_DTA:
		dispatch_get_dta(context, registers);
		return DOS_INT21_HANDLED;
	case INT21_GET_VERSION:
		dispatch_get_version(context, registers);
		return DOS_INT21_HANDLED;
	case INT21_GET_DPB:
		return dispatch_get_dpb(context, registers, false);
	case INT21_DOS_VARIABLES:
		return dispatch_dos_variables(context, registers);
	case INT21_GET_INDOS_ADDRESS:
		dispatch_get_indos_address(registers);
		return DOS_INT21_HANDLED;
	case INT21_GET_INTERRUPT_VECTOR:
		return dispatch_get_interrupt_vector(context, registers);
	case INT21_GET_DISK_FREE_SPACE:
		return dispatch_get_disk_free_space(context, registers);
	case INT21_CREATE_DIRECTORY:
		return dispatch_create_directory(context, registers);
	case INT21_CHANGE_DIRECTORY:
		return dispatch_change_directory(context, registers);
	case INT21_CREATE_FILE:
		return dispatch_create_file(context, registers);
	case INT21_OPEN_FILE:
		return dispatch_open_file(context, registers);
	case INT21_CLOSE_FILE:
		return dispatch_close_file(context, registers);
	case INT21_READ_FILE:
		return dispatch_read_file(context, registers);
	case INT21_DELETE_FILE:
		/* The mounted FAT volume has no write transaction yet. */
		int21_return_error(context, registers, INT21_DELETE_FILE,
				   DOS_ERROR_ACCESS_DENIED);
		return DOS_INT21_HANDLED;
	case INT21_SEEK_FILE:
		return dispatch_seek_file(context, registers);
	case INT21_RENAME_FILE:
		return dispatch_rename_file(context, registers);
	case INT21_FILE_DATE_TIME:
		return dispatch_file_date_time(context, registers);
	case INT21_FILE_ATTRIBUTES:
		return dispatch_file_attributes(context, registers);
	case INT21_GET_CURRENT_DIRECTORY: {
		uint8_t path[INT21_PATH_CAPACITY] = {0u};
		uint8_t rollback[INT21_PATH_CAPACITY];
		size_t path_length = 0u;
		enum dos_error error;
		enum dos_machine_status machine_status;
		uint8_t drive = dos_register_low8(registers->edx);

		if (drive == 0u)
			drive = (uint8_t)(context->current_drive + 1u);
		if (!drive_abi_is_available(context, drive)) {
			int21_return_error(context, registers,
					   INT21_GET_CURRENT_DIRECTORY,
					   DOS_ERROR_INVALID_DRIVE);
			return DOS_INT21_HANDLED;
		}
		if (context->get_current_directory != NULL) {
			error = context->get_current_directory(
				context->current_directory_context, drive, path,
				sizeof(path), &path_length);
			if (error != DOS_SUCCESS) {
				int21_return_error(context, registers,
						   INT21_GET_CURRENT_DIRECTORY,
						   error);
				return DOS_INT21_HANDLED;
			}
			if (path_length >= sizeof(path))
				return DOS_INT21_MACHINE_FAULT;
			path[path_length] = 0u;
		}
		machine_status = dos_machine_replace_far(
			&context->machine, registers->ds,
			dos_register_low16(registers->esi), path, sizeof(path),
			rollback, sizeof(rollback), path_length + 1u);

		if (machine_status == DOS_MACHINE_ROLLBACK_FAILED) {
			context->machine_poisoned = true;
			return DOS_INT21_MACHINE_POISONED;
		}
		if (machine_status != DOS_MACHINE_OK)
			return DOS_INT21_MACHINE_FAULT;
		int21_return_success(registers);
		return DOS_INT21_HANDLED;
	}
	case INT21_ALLOCATE_MEMORY:
		return dispatch_allocate(context, registers);
	case INT21_FREE_MEMORY:
		return dispatch_free(context, registers);
	case INT21_RESIZE_MEMORY:
		return dispatch_resize(context, registers);
	case INT21_EXECUTE:
		return dispatch_execute(context, registers);
	case INT21_GET_CHILD_RETURN:
		dispatch_get_child_return(context, registers);
		return DOS_INT21_HANDLED;
	case INT21_FIND_FIRST:
		return dispatch_find_first(context, registers);
	case INT21_FIND_NEXT:
		return dispatch_find_next(context, registers);
	case INT21_SET_CURRENT_PDB:
		return dispatch_set_current_psp(context, registers);
	case INT21_GET_CURRENT_PDB:
	case INT21_GET_CURRENT_PSP:
		dispatch_get_current_psp(context, registers);
		return DOS_INT21_HANDLED;
	case INT21_EXTENDED_CODE_SYSTEM:
		return dispatch_extended_code_system(context, registers);
	case INT21_GET_LIST_OF_LISTS:
		dispatch_get_list_of_lists(registers);
		return DOS_INT21_HANDLED;
	case INT21_GET_EXTENDED_COUNTRY:
		return dispatch_extended_country(context, registers);
	case INT21_GET_SET_CODE_PAGE:
		return dispatch_get_set_code_page(context, registers);
	case INT21_SET_HANDLE_COUNT:
		return dispatch_set_handle_count(context, registers);
	case INT21_ALLOCATION_OPERATION:
		return dispatch_allocation_operation(context, registers);
	case INT21_GET_EXTENDED_ERROR:
		dispatch_get_extended_error(context, registers);
		return DOS_INT21_HANDLED;
	case INT21_NETWORK_REDIRECTION:
		/* No redirector is installed.  MS-DOS routes AH=5Fh through
		 * INT 2Fh; the 5F44h fallback returns the normal
		 * DOS "invalid function" result.  Capability probing must not stop
		 * the guest process. */
		int21_return_error(context, registers,
				   INT21_NETWORK_REDIRECTION,
				   DOS_ERROR_INVALID_FUNCTION);
		return DOS_INT21_HANDLED;
	default:
		return DOS_INT21_UNIMPLEMENTED;
	}
}
