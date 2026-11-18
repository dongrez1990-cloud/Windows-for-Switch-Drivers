/* SPDX-License-Identifier: ISC */
/* Minimal freestanding runtime for the SwitchWifi host tests.
 *
 * The build host has the WDK and LLVM but no MSVC CRT (no INCLUDE/LIB, no
 * vcruntime.h), so `clang-cl /I ucrt` cannot link a hosted test binary. These
 * tests only touch a very small slice of the C library - memory functions plus
 * puts() for the pass message - so this file supplies exactly that on top of a
 * few kernel32 imports, and the runner links it with -nostdlib.
 *
 * Compiled with -fno-builtin: the memory routines below must not be rewritten
 * into calls to themselves.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SWIFI_HOST_HANDLE *SWIFI_HOST_HANDLE;

__declspec(dllimport) void *__stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(void *Heap, unsigned long Flags, unsigned long long Bytes);
__declspec(dllimport) int __stdcall HeapFree(void *Heap, unsigned long Flags, void *Block);
__declspec(dllimport) SWIFI_HOST_HANDLE __stdcall GetStdHandle(unsigned long Index);
__declspec(dllimport) int __stdcall WriteFile(SWIFI_HOST_HANDLE File, const void *Buffer,
    unsigned long Bytes, unsigned long *Written, void *Overlapped);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int Code);

#define SWIFI_STD_OUTPUT_HANDLE ((unsigned long)-11)
#define SWIFI_HEAP_ZERO_MEMORY  0x00000008ul

int main(void);

/* ---- string.h ---- */

void *memcpy(void *Destination, const void *Source, size_t Count)
{
    unsigned char *To = Destination;
    const unsigned char *From = Source;
    for (size_t i = 0; i < Count; ++i) To[i] = From[i];
    return Destination;
}

void *memmove(void *Destination, const void *Source, size_t Count)
{
    unsigned char *To = Destination;
    const unsigned char *From = Source;
    if (To < From) for (size_t i = 0; i < Count; ++i) To[i] = From[i];
    else for (size_t i = Count; i; --i) To[i-1] = From[i-1];
    return Destination;
}

void *memset(void *Destination, int Value, size_t Count)
{
    unsigned char *To = Destination;
    for (size_t i = 0; i < Count; ++i) To[i] = (unsigned char)Value;
    return Destination;
}

int memcmp(const void *Left, const void *Right, size_t Count)
{
    const unsigned char *A = Left, *B = Right;
    for (size_t i = 0; i < Count; ++i)
        if (A[i] != B[i]) return A[i] < B[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *Text)
{
    size_t Length = 0;
    while (Text[Length]) ++Length;
    return Length;
}

int strcmp(const char *Left, const char *Right)
{
    while (*Left && *Left == *Right) { ++Left; ++Right; }
    return (unsigned char)*Left < (unsigned char)*Right ? -1 : (unsigned char)*Left > (unsigned char)*Right;
}

/* ---- stdlib.h ---- */

void *malloc(size_t Size)
{
    return HeapAlloc(GetProcessHeap(), 0, Size);
}

void *calloc(size_t Count, size_t Size)
{
    return HeapAlloc(GetProcessHeap(), SWIFI_HEAP_ZERO_MEMORY, (unsigned long long)Count * Size);
}

void free(void *Block)
{
    if (Block) HeapFree(GetProcessHeap(), 0, Block);
}

/* ---- stdio.h ---- */

static void Emit(const char *Text, unsigned long Length)
{
    unsigned long Written = 0;
    SWIFI_HOST_HANDLE Out = GetStdHandle(SWIFI_STD_OUTPUT_HANDLE);
    if (Out) WriteFile(Out, Text, Length, &Written, 0);
}

static unsigned long Append(char *Buffer, unsigned long At, unsigned long Cap, const char *Text, unsigned long Length)
{
    for (unsigned long i = 0; i < Length && At < Cap; ++i) Buffer[At++] = Text[i];
    return At;
}

static unsigned long AppendUnsigned(char *Buffer, unsigned long At, unsigned long Cap,
    unsigned long long Value, unsigned Base, int Upper, int Negative)
{
    char Digits[24];
    unsigned long Count = 0;
    const char *Alphabet = Upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!Value) Digits[Count++] = '0';
    while (Value) { Digits[Count++] = Alphabet[Value % Base]; Value /= Base; }
    if (Negative && At < Cap) Buffer[At++] = '-';
    while (Count && At < Cap) Buffer[At++] = Digits[--Count];
    return At;
}

/* Deliberately small: %s %d %u %x %X %c %% and literals. Most tests only print a
 * fixed pass message through puts(); anything richer is added when needed. */
static int EmitFormatted(const char *Format, __builtin_va_list Arguments)
{
    char Buffer[512];
    unsigned long At = 0;

    for (const char *P = Format; *P && At < sizeof(Buffer); ++P) {
        if (*P != '%') { Buffer[At++] = *P; continue; }
        ++P;
        switch (*P) {
        case 's': {
            const char *Text = __builtin_va_arg(Arguments, const char *);
            At = Append(Buffer, At, sizeof(Buffer), Text ? Text : "(null)", Text ? strlen(Text) : 6);
            break;
        }
        case 'd': {
            int Value = __builtin_va_arg(Arguments, int);
            At = AppendUnsigned(Buffer, At, sizeof(Buffer), (unsigned long long)(Value < 0 ? -Value : Value), 10, 0, Value < 0);
            break;
        }
        case 'u': At = AppendUnsigned(Buffer, At, sizeof(Buffer), __builtin_va_arg(Arguments, unsigned), 10, 0, 0); break;
        case 'x': At = AppendUnsigned(Buffer, At, sizeof(Buffer), __builtin_va_arg(Arguments, unsigned), 16, 0, 0); break;
        case 'X': At = AppendUnsigned(Buffer, At, sizeof(Buffer), __builtin_va_arg(Arguments, unsigned), 16, 1, 0); break;
        case 'c': Buffer[At++] = (char)__builtin_va_arg(Arguments, int); break;
        case '%': Buffer[At++] = '%'; break;
        case '\0': --P; break;
        default: Buffer[At++] = '%'; if (At < sizeof(Buffer)) Buffer[At++] = *P; break;
        }
    }
    Emit(Buffer, At);
    return (int)At;
}

int printf(const char *Format, ...)
{
    __builtin_va_list Arguments;
    __builtin_va_start(Arguments, Format);
    int Written = EmitFormatted(Format, Arguments);
    __builtin_va_end(Arguments);
    return Written;
}

/* Both streams share the console: the tests only use stderr for a FAIL line. */
int fprintf(FILE *Stream, const char *Format, ...)
{
    (void)Stream;
    __builtin_va_list Arguments;
    __builtin_va_start(Arguments, Format);
    int Written = EmitFormatted(Format, Arguments);
    __builtin_va_end(Arguments);
    return Written;
}

void exit(int Code)
{
    ExitProcess((unsigned)Code);
}

int puts(const char *Text)
{
    unsigned long Length = (unsigned long)strlen(Text);
    Emit(Text, Length);
    Emit("\r\n", 2);
    return 0;
}

/* ---- assert.h ---- */

void swifi_host_assert_fail(const char *Expression, const char *File, int Line)
{
    printf("\nASSERT FAILED: %s  at %s:%d\n", Expression, File, Line);
    ExitProcess(3);
}

/* ---- entry ---- */

void swifi_host_entry(void)
{
    ExitProcess((unsigned)main());
}
