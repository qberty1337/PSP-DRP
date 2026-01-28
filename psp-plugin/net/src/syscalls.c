#include <pspkernel.h>

__attribute__((noreturn)) void _exit(int status) {
    (void)status;
    sceKernelExitGame();
    while (1) {
        sceKernelDelayThread(1000000);
    }
}
