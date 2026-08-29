// SPDX-License-Identifier: GPL-2.0-only
/*
 * MS-DOS external-command search policy.
 * Within one directory COM outranks EXE, which outranks BAT.  The first
 * directory with any match wins, even when that match is EXE or BAT.
 */
#include "shell_external_command.h"

#include "string.h"

struct extension_rule {
	const char *suffix;
	size_t suffix_length;
	enum shell_external_file_type type;
};

static const struct extension_rule extension_rules[] = {
	{ ".COM", sizeof(".COM") - 1u, SHELL_EXTERNAL_COM },
	{ ".EXE", sizeof(".EXE") - 1u, SHELL_EXTERNAL_EXE },
	{ ".BAT", sizeof(".BAT") - 1u, SHELL_EXTERNAL_BAT },
};

static bool ascii_span_equal(const char *left, const char *right,
			     size_t length)
{
	size_t index;

	for (index = 0u; index < length; ++index) {
		if (ascii_toupper(left[index]) != ascii_toupper(right[index]))
			return false;
	}
	return true;
}

static bool classify_explicit_extension(
	const char *path, size_t path_length,
	enum shell_external_file_type *type)
{
	size_t index = path_length;
	size_t rule_index;

	while (index != 0u && path[index - 1u] != '\\' &&
	       path[index - 1u] != '/') {
		--index;
	}
	for (; index < path_length; ++index) {
		if (path[index] != '.')
			continue;
		for (rule_index = 0u; rule_index < ARRAY_SIZE(extension_rules);
		     ++rule_index) {
			const struct extension_rule *rule =
				&extension_rules[rule_index];

			if (path_length - index == rule->suffix_length &&
			    ascii_span_equal(path + index, rule->suffix,
					     rule->suffix_length)) {
				*type = rule->type;
				return true;
			}
		}
		return false;
	}
	return false;
}

static enum shell_external_status probe_candidate(
	const struct shell_external_request *request, const char *base,
	size_t base_length, const struct extension_rule *rule,
	struct shell_external_result *result)
{
	struct shell_external_result prepared = {0};
	enum shell_external_probe_status probe_status;
	size_t index;

	if (base_length + rule->suffix_length >= DOS_PATH_CAPACITY)
		return SHELL_EXTERNAL_PATH_ERROR;
	for (index = 0u; index < base_length; ++index)
		prepared.absolute_path[index] = base[index];
	for (index = 0u; index < rule->suffix_length; ++index)
		prepared.absolute_path[base_length + index] = rule->suffix[index];
	prepared.path_length = base_length + rule->suffix_length;
	prepared.absolute_path[prepared.path_length] = '\0';
	prepared.type = rule->type;
	probe_status = request->probe(prepared.absolute_path,
				      prepared.path_length,
				      request->probe_context);
	if (probe_status == SHELL_EXTERNAL_PROBE_FOUND) {
		*result = prepared;
		return SHELL_EXTERNAL_FOUND;
	}
	return probe_status == SHELL_EXTERNAL_PROBE_NOT_FOUND
		       ? SHELL_EXTERNAL_NOT_FOUND
		       : SHELL_EXTERNAL_STORAGE_ERROR;
}

static enum shell_external_status search_base(
	const struct shell_external_request *request, const char *base,
	size_t base_length, struct shell_external_result *result)
{
	enum shell_external_file_type explicit_type;
	enum shell_external_probe_status probe_status;
	size_t rule_index;

	if (classify_explicit_extension(base, base_length, &explicit_type)) {
		probe_status = request->probe(base, base_length,
					      request->probe_context);
		if (probe_status == SHELL_EXTERNAL_PROBE_FOUND) {
			struct shell_external_result prepared = {0};

			if (memcpy_s(prepared.absolute_path,
				     sizeof(prepared.absolute_path), base,
				     base_length + 1u, base_length + 1u) != MEMORY_OK)
				return SHELL_EXTERNAL_PATH_ERROR;
			prepared.path_length = base_length;
			prepared.type = explicit_type;
			*result = prepared;
			return SHELL_EXTERNAL_FOUND;
		}
		return probe_status == SHELL_EXTERNAL_PROBE_NOT_FOUND
			       ? SHELL_EXTERNAL_NOT_FOUND
			       : SHELL_EXTERNAL_STORAGE_ERROR;
	}
	/* A user-supplied, unsupported extension is not executable. */
	for (rule_index = base_length; rule_index != 0u; --rule_index) {
		char character = base[rule_index - 1u];

		if (character == '\\' || character == '/')
			break;
		if (character == '.')
			return SHELL_EXTERNAL_NOT_FOUND;
	}
	for (rule_index = 0u; rule_index < ARRAY_SIZE(extension_rules);
	     ++rule_index) {
		enum shell_external_status status = probe_candidate(
			request, base, base_length, &extension_rules[rule_index],
			result);

		if (status != SHELL_EXTERNAL_NOT_FOUND)
			return status;
	}
	return SHELL_EXTERNAL_NOT_FOUND;
}

static enum shell_external_status canonical_search(
	const struct shell_external_request *request, const char *input,
	size_t input_length, struct shell_external_result *result)
{
	char base[DOS_PATH_CAPACITY];
	enum dos_path_status path_status;
	size_t base_length;

	path_status = dos_path_canonicalize(
		request->current_path, request->current_path_capacity, input,
		input_length, base);
	if (path_status != DOS_PATH_OK)
		return SHELL_EXTERNAL_PATH_ERROR;
	base_length = strnlen(base, sizeof(base));
	if (base_length == sizeof(base))
		return SHELL_EXTERNAL_PATH_ERROR;
	return search_base(request, base, base_length, result);
}

enum shell_external_status shell_external_resolve(
	const struct shell_external_request *request,
	struct shell_external_result *result)
{
	char joined[DOS_PATH_CAPACITY];
	size_t entry_start;
	size_t entry_end;
	size_t joined_length;
	size_t index;
	enum shell_external_status status;

	if (request == NULL || result == NULL || request->command == NULL ||
	    request->current_path == NULL || request->probe == NULL ||
	    request->command_length == 0u ||
	    request->command_length >= DOS_PATH_CAPACITY ||
	    (request->search_path == NULL && request->search_path_length != 0u))
		return SHELL_EXTERNAL_INVALID_COMMAND;
	for (index = 0u; index < request->command_length; ++index) {
		char character = request->command[index];

		if (character == '\0' || character == '*' || character == '?' ||
		    character == ';')
			return SHELL_EXTERNAL_INVALID_COMMAND;
	}
	status = canonical_search(request, request->command,
				  request->command_length, result);
	if (status != SHELL_EXTERNAL_NOT_FOUND)
		return status;
	if (dos_path_is_explicit(request->command, request->command_length))
		return SHELL_EXTERNAL_NOT_FOUND;

	entry_start = 0u;
	while (entry_start <= request->search_path_length) {
		entry_end = entry_start;
		while (entry_end < request->search_path_length &&
		       request->search_path[entry_end] != ';')
			++entry_end;
		/* Empty PATH elements are skipped. */
		if (entry_end != entry_start) {
			joined_length = entry_end - entry_start;
			if (joined_length + 1u + request->command_length >=
			    sizeof(joined))
				return SHELL_EXTERNAL_PATH_ERROR;
			for (index = 0u; index < joined_length; ++index)
				joined[index] = request->search_path[entry_start + index];
			if (joined[joined_length - 1u] != '\\' &&
			    joined[joined_length - 1u] != '/')
				joined[joined_length++] = '\\';
			for (index = 0u; index < request->command_length; ++index)
				joined[joined_length++] = request->command[index];
			status = canonical_search(request, joined, joined_length,
						  result);
			if (status != SHELL_EXTERNAL_NOT_FOUND)
				return status;
		}
		if (entry_end == request->search_path_length)
			break;
		entry_start = entry_end + 1u;
	}
	return SHELL_EXTERNAL_NOT_FOUND;
}
