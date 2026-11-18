/* SPDX-License-Identifier: ISC */
/* Freestanding assert for the SwitchWifi host tests.
 *
 * The build host has the WDK + LLVM but no MSVC CRT, so the tests cannot use a
 * normal hosted toolchain. tools/Test-SwitchWifi.ps1 compiles them freestanding
 * against this header set plus tools/HostTests/shim.c, which supplies exactly
 * the handful of C library calls the tests use. */
#ifndef SWIFI_HOST_ASSERT_H
#define SWIFI_HOST_ASSERT_H

void swifi_host_assert_fail(const char *Expression, const char *File, int Line);

#ifdef NDEBUG
#define assert(Expression) ((void)0)
#else
#define assert(Expression) ((Expression) ? (void)0 : swifi_host_assert_fail(#Expression, __FILE__, __LINE__))
#endif

#endif
