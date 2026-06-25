/* t2_hook.cpp — Read game memory from constructor */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>

typedef void (*con_printf_t)(const char *fmt, ...);
static con_printf_t Con__printf = NULL;

struct sConsoleConstructor {
    void *sc, *ic, *fc, *vc, *bc;
    int mina, maxa;
    const char *usage, *funcName, *className;
    sConsoleConstructor *next;
};

#define mConsoleConstructor 0x8487434

static void __attribute__((constructor)) init(void) {
    Con__printf = (con_printf_t)dlsym(RTLD_DEFAULT, "_ZN3Con6printfEPKcz");
    if (!Con__printf) {
        Con__printf = (con_printf_t)0x080E61E4;
    }

    // Read game memory safely
    uint32_t value = *(uint32_t *)mConsoleConstructor;
    void **ptr = (void **)mConsoleConstructor;

    // Write results to stdout
    char buf[512];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len, "[t2_hook] Hook loaded!\n");
    len += snprintf(buf + len, sizeof(buf) - len, "[t2_hook] mConsoleConstructor @ 0x%08X\n", mConsoleConstructor);
    len += snprintf(buf + len, sizeof(buf) - len, "[t2_hook] Value: 0x%08X\n", value);
    len += snprintf(buf + len, sizeof(buf) - len, "[t2_hook] Ptr: %p\n", ptr);
    len += snprintf(buf + len, sizeof(buf) - len, "[t2_hook] Con::printf: %p\n", (void*)Con__printf);
    write(1, buf, len);
}
