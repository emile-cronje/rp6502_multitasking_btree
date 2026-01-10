#include "string_helpers.h"
#include <string.h>

char* itoa_new(unsigned int value, char *buffer, size_t buf_size)
{
    unsigned int temp;
    int digit_count;
    int pos;
    
    if (buf_size == 0) return buffer;
    
    if (buf_size == 1) {
        buffer[0] = '\0';
        return buffer;
    }
    
    /* Handle zero specially */
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return buffer;
    }
    
    /* Count digits */
    temp = value;
    digit_count = 0;
    while (temp > 0) {
        digit_count++;
        temp /= 10;
    }
    
    /* Check if buffer is large enough (digits + null terminator) */
    if ((size_t)(digit_count + 1) > buf_size) {
        digit_count = (int)buf_size - 2;
        if (digit_count < 0) digit_count = 0;
    }
    
    /* Write digits in reverse order, then reverse the string */
    pos = digit_count;
    temp = value;
    
    while (temp > 0 && pos > 0) {
        buffer[pos - 1] = '0' + (temp % 10);
        temp /= 10;
        pos--;
    }
    
    /* Null terminate */
    buffer[digit_count] = '\0';
    
    return buffer;
}
