/* SPDX-License-Identifier: ISC */
#ifndef SWIFI_HOST_STDIO_H
#define SWIFI_HOST_STDIO_H

/* No real stream objects: both stdout and stderr converge on the console that
 * shim.c writes through WriteFile, which is all the tests need. */
typedef struct SWIFI_HOST_FILE { int Unused; } FILE;
#define stderr ((FILE *)0)
#define stdout ((FILE *)0)

int puts(const char *Text);
int printf(const char *Format, ...);
int fprintf(FILE *Stream, const char *Format, ...);
#endif
