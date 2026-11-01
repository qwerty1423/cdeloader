// Test host for scripts/test-modloader.sh.
//
// Stands in for CDE.exe: it loads cdeloader.dll the way the injected import
// would, then calls HostTarget() so the sample plugin's hook is observable.
// HostTarget has a normal multi-instruction prologue (push/mov frame) so the
// loader's trampoline must steal *whole* instructions, not a fixed five bytes.

#include <windows.h>
#include <stdio.h>
#include <string.h>

int HostTarget(int x);

__asm__(
    ".text\n"
    ".globl _HostTarget\n"
    ".def _HostTarget; .scl 2; .type 32; .endef\n"
    "_HostTarget:\n"
    "  push %ebp\n"           // 1 byte
    "  mov %esp, %ebp\n"      // 2 bytes
    "  mov 8(%ebp), %eax\n"   // 3 bytes  (5-byte cut would split this)
    "  add $1, %eax\n"        // 3 bytes
    "  pop %ebp\n"            // 1 byte
    "  ret\n"
);

int main(int argc, char** argv) {
    HMODULE loader = LoadLibraryA("cdeloader.dll");
    if (!loader) {
        printf("HOST: failed to load cdeloader.dll (err %lu)\n",
               (unsigned long)GetLastError());
        return 1;
    }
    printf("HOST: cdeloader.dll loaded\n");

    if (argc > 1 && strcmp(argv[1], "redirect") == 0) {
        /* Ask for a game data file; the loader should serve the mod's copy. */
        Sleep(300);
        HANDLE f = CreateFileA("Resources\\Data\\Modules\\engines.txt", GENERIC_READ,
                               FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            printf("HOST: open failed (%lu)\n", (unsigned long)GetLastError());
            return 3;
        }
        char buf[64] = {0};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(f);
        printf("HOST: read '%s'\n", buf);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "crash") == 0) {
        /* Deliberate fault: proves the loader's crash handler writes a dump. */
        Sleep(200);
        int (*boom)(void) = NULL;
        printf("HOST: crashing on purpose\n");
        fflush(stdout);
        return boom();
    }

    Sleep(300);  /* let the init thread run and the frame callback tick */

    int r = HostTarget(1);
    printf("HOST: HostTarget(1) = %d\n", r);
    Sleep(300);
    return 0;
}
