// SPDX-License-Identifier: MIT
#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    for (size_t i = 0u; i < size; ++i) out[i] = in[i];
    return destination;
}

void *memset(void *destination, int value, size_t size) {
    unsigned char *out = (unsigned char *)destination;
    for (size_t i = 0u; i < size; ++i) out[i] = (unsigned char)value;
    return destination;
}
