// SPDX-License-Identifier: GPL-2.0-only
/*
 * DOS-C32 bounded formatter
 *
 * Compatibility contract: render command messages and their character, ASCIIZ, and
 *                 signed/unsigned numeric substitutions
 * Safety changes: destination and format capacities are mandatory; string
 *                 substitutions require precision; failure is transactional
 */
#include "format.h"

struct format_writer {
	char *destination;
	size_t capacity;
	size_t length;
};

static enum format_status writer_put(struct format_writer *writer,
				     char character)
{
	if (writer->length == (size_t)-1)
		return FORMAT_RANGE_ERROR;
	if (writer->destination != NULL) {
		if (writer->length >= writer->capacity)
			return FORMAT_TRUNCATED;
		writer->destination[writer->length] = character;
	}
	writer->length++;
	return FORMAT_OK;
}

static enum format_status writer_repeat(struct format_writer *writer,
					char character, size_t count)
{
	size_t index;

	for (index = 0; index < count; index++) {
		enum format_status status = writer_put(writer, character);

		if (status != FORMAT_OK)
			return status;
	}
	return FORMAT_OK;
}

static enum format_status write_unsigned(struct format_writer *writer,
					 uint32_t value, uint32_t base,
					 bool uppercase, size_t minimum_digits)
{
	const char *digits = uppercase ? "0123456789ABCDEF" :
					    "0123456789abcdef";
	char reversed[32];
	size_t digit_count = 0;
	enum format_status status;

	do {
		reversed[digit_count++] = digits[value % base];
		value /= base;
	} while (value != 0);

	if (minimum_digits > digit_count) {
		status = writer_repeat(writer, '0',
				       minimum_digits - digit_count);
		if (status != FORMAT_OK)
			return status;
	}
	while (digit_count != 0) {
		status = writer_put(writer, reversed[--digit_count]);
		if (status != FORMAT_OK)
			return status;
	}
	return FORMAT_OK;
}

static enum format_status write_pointer(struct format_writer *writer,
					 uintptr_t value)
{
	const char digits[] = "0123456789abcdef";
	size_t remaining = sizeof(value) * 2u;
	enum format_status status;

	status = writer_put(writer, '0');
	if (status != FORMAT_OK)
		return status;
	status = writer_put(writer, 'x');
	if (status != FORMAT_OK)
		return status;
	while (remaining != 0u) {
		size_t shift;
		uint8_t nibble;

		--remaining;
		shift = remaining * 4u;
		nibble = (uint8_t)((value >> shift) & (uintptr_t)0x0fu);
		status = writer_put(writer, digits[nibble]);
		if (status != FORMAT_OK)
			return status;
	}
	return FORMAT_OK;
}

static enum format_status write_string(struct format_writer *writer,
				       const char *source, size_t limit)
{
	size_t index;

	if (limit != 0 && source == NULL)
		return FORMAT_INVALID_ARGUMENT;
	for (index = 0; index < limit; index++) {
		enum format_status status;

		if (source[index] == '\0')
			break;
		status = writer_put(writer, source[index]);
		if (status != FORMAT_OK)
			return status;
	}
	return FORMAT_OK;
}

static enum format_status parse_precision(const char *format,
					  size_t format_length,
					  size_t *position,
					  bool *present,
					  bool *dynamic,
					  size_t *precision)
{
	size_t value = 0;
	size_t index = *position;

	*present = false;
	*dynamic = false;
	*precision = 0;
	if (index >= format_length || format[index] != '.')
		return FORMAT_OK;

	*present = true;
	index++;
	if (index < format_length && format[index] == '*') {
		*dynamic = true;
		*position = index + 1;
		return FORMAT_OK;
	}

	while (index < format_length && format[index] >= '0' &&
	       format[index] <= '9') {
		size_t digit = (size_t)(format[index] - '0');

		if (value > ((size_t)-1 - digit) / 10u)
			return FORMAT_RANGE_ERROR;
		value = value * 10u + digit;
		index++;
	}
	*precision = value;
	*position = index;
	return FORMAT_OK;
}

static enum format_status render_format(struct format_writer *writer,
					const char *format,
					size_t format_length,
					va_list arguments)
{
	size_t position = 0;

	while (position < format_length) {
		bool has_precision;
		bool dynamic_precision;
		size_t precision;
		char conversion;
		enum format_status status;

		if (format[position] != '%') {
			status = writer_put(writer, format[position++]);
			if (status != FORMAT_OK)
				return status;
			continue;
		}

		position++;
		if (position >= format_length)
			return FORMAT_INVALID_FORMAT;
		if (format[position] == '%') {
			position++;
			status = writer_put(writer, '%');
			if (status != FORMAT_OK)
				return status;
			continue;
		}

		status = parse_precision(format, format_length, &position,
					 &has_precision, &dynamic_precision,
					 &precision);
		if (status != FORMAT_OK)
			return status;
		if (dynamic_precision) {
			int argument = va_arg(arguments, int);

			if (argument < 0)
				return FORMAT_INVALID_ARGUMENT;
			precision = (size_t)argument;
		}
		if (position >= format_length)
			return FORMAT_INVALID_FORMAT;
		conversion = format[position++];

		if (conversion != 's' && has_precision)
			return FORMAT_INVALID_FORMAT;
		switch (conversion) {
		case 'c':
			status = writer_put(writer, (char)va_arg(arguments, int));
			break;
		case 's':
			if (!has_precision)
				return FORMAT_INVALID_FORMAT;
			status = write_string(writer,
					      va_arg(arguments, const char *),
					      precision);
			break;
		case 'u':
			status = write_unsigned(writer,
						va_arg(arguments, unsigned int),
						10, false, 1);
			break;
		case 'd': {
			int32_t signed_value = va_arg(arguments, int);
			uint32_t magnitude = (uint32_t)signed_value;

			if (signed_value < 0) {
				status = writer_put(writer, '-');
				if (status != FORMAT_OK)
					return status;
				magnitude = 0u - magnitude;
			}
			status = write_unsigned(writer, magnitude, 10, false, 1);
			break;
		}
		case 'x':
			status = write_unsigned(writer,
						va_arg(arguments, unsigned int),
						16, false, 1);
			break;
		case 'X':
			status = write_unsigned(writer,
						va_arg(arguments, unsigned int),
						16, true, 1);
			break;
		case 'p':
			status = write_pointer(writer,
					       (uintptr_t)va_arg(arguments, void *));
			break;
		default:
			return FORMAT_INVALID_FORMAT;
		}
		if (status != FORMAT_OK)
			return status;
	}
	return FORMAT_OK;
}

static enum format_status bounded_string_length(const char *text,
						 size_t capacity,
						 size_t *length)
{
	size_t index;

	if (text == NULL || capacity == 0 || length == NULL)
		return FORMAT_INVALID_ARGUMENT;
	for (index = 0; index < capacity; index++) {
		if (text[index] == '\0') {
			*length = index;
			return FORMAT_OK;
		}
	}
	return FORMAT_INVALID_FORMAT;
}

enum format_status vsnprintf_s(char *destination,
			       size_t destination_capacity,
			       size_t *required_length,
			       const char *format, size_t format_capacity,
			       va_list arguments)
{
	struct format_writer count_writer = { NULL, 0, 0 };
	struct format_writer output_writer;
	size_t format_length;
	va_list count_arguments;
	va_list output_arguments;
	enum format_status status;

	if (required_length != NULL)
		*required_length = 0;
	if (destination != NULL && destination_capacity != 0)
		destination[0] = '\0';
	if (required_length == NULL || destination == NULL ||
	    destination_capacity == 0)
		return FORMAT_INVALID_ARGUMENT;

	status = bounded_string_length(format, format_capacity, &format_length);
	if (status != FORMAT_OK)
		return status;

	va_copy(count_arguments, arguments);
	status = render_format(&count_writer, format, format_length,
			       count_arguments);
	va_end(count_arguments);
	if (status != FORMAT_OK)
		return status;
	*required_length = count_writer.length;
	if (count_writer.length >= destination_capacity)
		return FORMAT_TRUNCATED;

	output_writer.destination = destination;
	output_writer.capacity = destination_capacity - 1u;
	output_writer.length = 0;
	va_copy(output_arguments, arguments);
	status = render_format(&output_writer, format, format_length,
			       output_arguments);
	va_end(output_arguments);
	if (status != FORMAT_OK || output_writer.length != count_writer.length) {
		destination[0] = '\0';
		*required_length = 0;
		return status == FORMAT_OK ? FORMAT_RANGE_ERROR : status;
	}
	destination[output_writer.length] = '\0';
	return FORMAT_OK;
}

enum format_status snprintf_s(char *destination,
			      size_t destination_capacity,
			      size_t *required_length,
			      const char *format, size_t format_capacity,
			      ...)
{
	va_list arguments;
	enum format_status status;

	va_start(arguments, format_capacity);
	status = vsnprintf_s(destination, destination_capacity, required_length,
			     format, format_capacity, arguments);
	va_end(arguments);
	return status;
}
