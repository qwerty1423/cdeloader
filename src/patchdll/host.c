// Minimal host used by scripts/test-patchdll.sh to prove that cdetool's section
// injection makes the Windows loader load a helper DLL (whose DllMain writes
// cdepatch.log) before the host's entry point runs.
#include <stdio.h>

int main(void) {
    printf("host ran\n");
    return 0;
}
