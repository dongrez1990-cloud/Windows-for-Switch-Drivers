/* SPDX-License-Identifier: ISC */
/* x64 stack probe for the SwitchWifi host tests.
 *
 * The compiler emits `mov eax, <frame size>; call __chkstk` before any frame
 * larger than one page. A hosted build gets this from the CRT; the test harness
 * is freestanding (-nostdlib), so it must be supplied here. Without it the
 * prologue subtracts the whole frame from RSP and the first store lands past
 * the guard page instead of growing the stack: test_control carries a ~90 KB
 * `model` struct on the stack and takes an access violation immediately.
 *
 * Contract (MSVC x64): RAX = bytes to probe, touches one byte per page going
 * down from the caller's frame base, preserving RAX/RCX and all other registers.
 */
__attribute__((naked, used)) void __chkstk(void)
{
    __asm__ volatile(
        "push %rcx\n\t"
        "push %rax\n\t"
        "cmp $0x1000, %rax\n\t"
        "lea 0x18(%rsp), %rcx\n\t"   /* return address + the two pushes above */
        "jb 2f\n\t"
        "1:\n\t"
        "sub $0x1000, %rcx\n\t"
        "orq $0, (%rcx)\n\t"
        "sub $0x1000, %rax\n\t"
        "cmp $0x1000, %rax\n\t"
        "ja 1b\n\t"
        "2:\n\t"
        "sub %rax, %rcx\n\t"
        "orq $0, (%rcx)\n\t"
        "pop %rax\n\t"
        "pop %rcx\n\t"
        "ret\n\t");
}
