#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

typedef int S32;

typedef void (*con_addMethodS_t)(const char *nsName, const char *name,
    const char *(*cb)(void *obj, S32 argc, const char **argv),
    const char *usage, S32 minArgs, S32 maxArgs);

static con_addMethodS_t Con__addMethodS = NULL;
static int registered = 0;

#define CONSOLE_GLOBAL 0x8493dec
#define CONSOLE_TABLE_SIZE_OFFS 4

/* ── SHA1 ────────────────────────────────────────────────────────────── */

static uint32_t rol32(uint32_t v, int n) { return n ? (v << n) | (v >> (32 - n)) : v; }

static void sha1(const uint8_t *data, int len, char out[41]) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    uint8_t blk[64];
    int pos = 0;
    uint64_t bits = (uint64_t)len * 8;

    while (len - pos >= 64) {
        uint32_t w[80], a, b, c, d, e, f, k, tmp;
        int t;
        for (t = 0; t < 16; t++)
            w[t] = ((uint32_t)blk[t*4]<<24)|((uint32_t)data[t*4+1]<<16)|((uint32_t)data[t*4+2]<<8)|data[t*4+3];
        for (t = 16; t < 80; t++) w[t] = rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16], 1);
        a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4];
        for (t = 0; t < 80; t++) {
            if (t<20)      { f=(b&c)|((~b)&d); k=0x5A827999; }
            else if (t<40) { f=b^c^d; k=0x6ED9EBA1; }
            else if (t<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else           { f=b^c^d; k=0xCA62C1D6; }
            tmp = rol32(a,5)+f+e+k+w[t];
            e=d; d=c; c=rol32(b,30); b=a; a=tmp;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
        pos += 64;
    }

    /* Final block(s) */
    memset(blk, 0, 64);
    memcpy(blk, data + pos, len - pos);
    blk[len - pos] = 0x80;
    if (len - pos >= 56) {
        /* Need two blocks */
        uint32_t w[80], a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
        int t;
        for (t = 0; t < 16; t++)
            w[t] = ((uint32_t)blk[t*4]<<24)|((uint32_t)blk[t*4+1]<<16)|((uint32_t)blk[t*4+2]<<8)|blk[t*4+3];
        for (t = 16; t < 80; t++) w[t] = rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16], 1);
        for (t = 0; t < 80; t++) {
            uint32_t f, k;
            if (t<20)      { f=(b&c)|((~b)&d); k=0x5A827999; }
            else if (t<40) { f=b^c^d; k=0x6ED9EBA1; }
            else if (t<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else           { f=b^c^d; k=0xCA62C1D6; }
            uint32_t tmp = rol32(a,5)+f+e+k+w[t];
            e=d; d=c; c=rol32(b,30); b=a; a=tmp;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
        memset(blk, 0, 64);
    }
    for (int i = 0; i < 8; i++) blk[56+i] = (bits >> (56 - i*8)) & 0xFF;
    {
        uint32_t w[80], a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
        int t;
        for (t = 0; t < 16; t++)
            w[t] = ((uint32_t)blk[t*4]<<24)|((uint32_t)blk[t*4+1]<<16)|((uint32_t)blk[t*4+2]<<8)|blk[t*4+3];
        for (t = 16; t < 80; t++) w[t] = rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16], 1);
        for (t = 0; t < 80; t++) {
            uint32_t f, k;
            if (t<20)      { f=(b&c)|((~b)&d); k=0x5A827999; }
            else if (t<40) { f=b^c^d; k=0x6ED9EBA1; }
            else if (t<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else           { f=b^c^d; k=0xCA62C1D6; }
            uint32_t tmp = rol32(a,5)+f+e+k+w[t];
            e=d; d=c; c=rol32(b,30); b=a; a=tmp;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (int i = 0; i < 5; i++) sprintf(out + i*8, "%08x", h[i]);
}

/* ── Callbacks ───────────────────────────────────────────────────────── */

static const char* hook_test_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    return "ok_from_c";
}

static const char* hook_sha1_cb(void *obj, S32 argc, const char **argv) {
    static char hex[41];
    (void)obj; (void)argc;
    if (argc < 2) return "usage: hook_sha1 <string>";
    sha1((const uint8_t *)argv[1], strlen(argv[1]), hex);
    return hex;
}

/* ── Registration thread ─────────────────────────────────────────────── */

static void* poll_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "[t2_hook] Poll thread started\n");

    for (int i = 0; i < 100; i++) {
        usleep(500000);  /* 500ms */

        uint32_t console_ptr = *(volatile uint32_t *)CONSOLE_GLOBAL;
        if (console_ptr != 0) {
            uint32_t table_size = *(volatile uint32_t *)(console_ptr + CONSOLE_TABLE_SIZE_OFFS);
            if (table_size > 0 && !registered) {
                registered = 1;
                Con__addMethodS = (con_addMethodS_t)0x80E85C0;
                fprintf(stderr, "[t2_hook] Console ready (table_size=%d) at ~%ds\n", table_size, i/2);
                Con__addMethodS(NULL, "hook_test", hook_test_cb, "hook_test - test command", 1, 1);
                Con__addMethodS(NULL, "hook_sha1", hook_sha1_cb, "hook_sha1 <string> - SHA1 hash", 2, 2);
                fprintf(stderr, "[t2_hook] Registered hook_test, hook_sha1\n");
                break;
            }
        }
    }
    if (!registered) {
        fprintf(stderr, "[t2_hook] TIMEOUT: console hash table never initialized\n");
    }
    return NULL;
}

static void __attribute__((constructor)) init(void) {
    fprintf(stderr, "[t2_hook] Constructor: starting poll thread\n");
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, poll_thread, NULL);
    pthread_attr_destroy(&attr);
}
