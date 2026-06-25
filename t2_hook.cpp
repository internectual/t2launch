/* t2_hook.cpp — Read mConsoleConstructor from thread */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <pthread.h>

typedef void (*con_printf_t)(const char *fmt, ...);
static con_printf_t Con__printf = NULL;

/* ConsoleConstructor struct */
struct sConsoleConstructor {
    void *sc, *ic, *fc, *vc, *bc;
    int mina, maxa;
    const char *usage, *funcName, *className;
    sConsoleConstructor *next;
};

/* mConsoleConstructor is a pointer at 0x084921f4 */
#define mConsoleConstructor 0x084921f4

static void *init_thread(void *arg) {
    sleep(3);

    /* Read the mConsoleConstructor pointer from process memory */
    uint32_t *mcc_ptr = (uint32_t *)mConsoleConstructor;
    uint32_t first_addr = *mcc_ptr;

    /* Call Con::printf with the value */
    if (Con__printf) {
        Con__printf("[t2_hook] mConsoleConstructor = 0x%08X", first_addr);
    }

    /* If it's non-zero, try to read the first node */
    if (first_addr != 0) {
        sConsoleConstructor *cc = (sConsoleConstructor *)first_addr;
        /* Read sc and funcName */
        uint32_t sc = (uint32_t)cc->sc;
        uint32_t next = (uint32_t)cc->next;
        if (Con__printf) {
            Con__printf("[t2_hook] First node: sc=0x%08X next=0x%08X mina=%d maxa=%d",
                sc, next, cc->mina, cc->maxa);
        }
        /* Read funcName string (carefully) */
        if (cc->funcName) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.60s", cc->funcName);
            if (Con__printf) Con__printf("[t2_hook] funcName: %s", buf);
        }
    }

    return NULL;
}

static void __attribute__((constructor)) init(void) {
    Con__printf = (con_printf_t)0x080E7350;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, init_thread, NULL);
    pthread_attr_destroy(&attr);
}
