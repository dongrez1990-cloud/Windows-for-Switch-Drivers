/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_HOST_STRING_H
#define SWIFI_HOST_STRING_H
#include <stddef.h>

void *memcpy(void *Destination, const void *Source, size_t Count);
void *memmove(void *Destination, const void *Source, size_t Count);
void *memset(void *Destination, int Value, size_t Count);
int memcmp(const void *Left, const void *Right, size_t Count);
size_t strlen(const char *Text);
int strcmp(const char *Left, const char *Right);
#endif
