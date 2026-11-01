// test_x86len.cpp — unit test for the hook engine's instruction-length decoder.
// Built and run under wine by scripts/test-x86len.sh.

#include "x86len.h"
#include <stdio.h>
#include <string.h>

namespace {

struct Case {
    const char* name;
    unsigned char bytes[16];
    size_t expect_len;  // 0 means "should refuse"
    int expect_rel;
};

int run(const Case& c) {
    size_t len = x86_instruction_length(c.bytes, sizeof(c.bytes), nullptr);
    int rel = 0;
    x86_instruction_length(c.bytes, sizeof(c.bytes), &rel);
    if (len != c.expect_len || (c.expect_len != 0 && rel != c.expect_rel)) {
        printf("FAIL %-28s got len=%u rel=%d, want len=%u rel=%d\n",
               c.name, (unsigned)len, rel, (unsigned)c.expect_len, c.expect_rel);
        return 1;
    }
    printf("ok   %-28s len=%u rel=%d\n", c.name, (unsigned)len, rel);
    return 0;
}

Case mk(const char* name, const unsigned char* bytes, size_t n, size_t len, int rel) {
    Case c;
    memset(&c, 0, sizeof(c));
    c.name = name;
    memcpy(c.bytes, bytes, n);
    c.expect_len = len;
    c.expect_rel = rel;
    return c;
}

}  // namespace

int main(void) {
    int fails = 0;
    {
        const unsigned char b[] = {0x55};                    fails += run(mk("push ebp", b, 1, 1, 0));
    }
    {
        const unsigned char b[] = {0x89, 0xE5};              fails += run(mk("mov ebp,esp", b, 2, 2, 0));
    }
    {
        const unsigned char b[] = {0x8B, 0x45, 0x08};        fails += run(mk("mov eax,[ebp+8]", b, 3, 3, 0));
    }
    {
        const unsigned char b[] = {0x83, 0xC0, 0x01};        fails += run(mk("add eax,1", b, 3, 3, 0));
    }
    {
        const unsigned char b[] = {0x5D};                    fails += run(mk("pop ebp", b, 1, 1, 0));
    }
    {
        const unsigned char b[] = {0xC3};                    fails += run(mk("ret", b, 1, 1, 0));
    }
    {
        const unsigned char b[] = {0xB8, 0x78, 0x56, 0x34, 0x12};  fails += run(mk("mov eax,imm32", b, 5, 5, 0));
    }
    {
        const unsigned char b[] = {0xE8, 0, 0, 0, 0};        fails += run(mk("call rel32", b, 5, 5, 1));
    }
    {
        const unsigned char b[] = {0x68, 0x78, 0x56, 0x34, 0x12};  fails += run(mk("push imm32", b, 5, 5, 0));
    }
    {
        const unsigned char b[] = {0x6A, 0x10};              fails += run(mk("push imm8", b, 2, 2, 0));
    }
    {
        const unsigned char b[] = {0xC7, 0x45, 0xFC, 0, 0, 0, 0};  fails += run(mk("mov dword [ebp-4],0", b, 7, 7, 0));
    }
    {
        const unsigned char b[] = {0x8B, 0x44, 0x24, 0x04};  fails += run(mk("mov eax,[esp+4]", b, 4, 4, 0));
    }
    {
        const unsigned char b[] = {0x0F, 0x1F, 0x44, 0x00, 0x00};  fails += run(mk("nop dword [eax+0]", b, 5, 5, 0));
    }
    {
        const unsigned char b[] = {0x0F, 0xB6, 0xC0};        fails += run(mk("movzx eax,al", b, 3, 3, 0));
    }
    {
        const unsigned char b[] = {0xFF, 0x75, 0x08};        fails += run(mk("push [ebp+8]", b, 3, 3, 0));
    }
    {
        const unsigned char b[] = {0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};  fails += run(mk("sub esp,0x100", b, 6, 6, 0));
    }
    {
        const unsigned char b[] = {0x64, 0xA1, 0, 0, 0, 0};  fails += run(mk("mov eax,fs:[0]", b, 6, 6, 0));
    }
    {
        const unsigned char b[] = {0x90};                    fails += run(mk("nop", b, 1, 1, 0));
    }
    {
        const unsigned char b[] = {0x0F, 0x84, 0, 0, 0, 0};  fails += run(mk("jz rel32", b, 6, 6, 1));
    }
    {
        const unsigned char b[] = {0xA1, 0x78, 0x56, 0x34, 0x12};  fails += run(mk("mov eax,[moffs32]", b, 5, 5, 0));
    }
    {
        const unsigned char b[] = {0x0F, 0x0B};              fails += run(mk("ud2", b, 2, 2, 0));
    }
    {
        const unsigned char b[] = {0x06};                    fails += run(mk("push es (unhandled)", b, 1, 0, 0));
    }

    if (fails) {
        printf("FAILED: %d case(s)\n", fails);
        return 1;
    }
    printf("OK: all length cases passed\n");
    return 0;
}
