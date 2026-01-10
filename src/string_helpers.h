#ifndef STRING_HELPERS_H
#define STRING_HELPERS_H

#include <stddef.h>
#include <stdint.h>

/**
 * Convert an unsigned integer to a null-terminated string in decimal format.
 * 
 * @param value The unsigned integer to convert
 * @param buffer The buffer to write the string to
 * @param buf_size The size of the buffer
 * @return Pointer to the buffer
 */
char* itoa_new(unsigned int value, char *buffer, size_t buf_size);

#endif // STRING_HELPERS_H
