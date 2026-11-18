/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_HOST_STDLIB_H
#define SWIFI_HOST_STDLIB_H
#include <stddef.h>

void *malloc(size_t Size);
void *calloc(size_t Count, size_t Size);
void free(void *Block);
void exit(int Code);
#endif
