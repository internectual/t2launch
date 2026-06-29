#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

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
    int pos = 0, t;
    uint64_t bits = (uint64_t)len * 8;
    uint32_t w[80],a,b,c,d,e,f,k,tmp;

    while (len - pos >= 64) {
        for (t=0;t<16;t++) w[t]=((uint32_t)data[pos+t*4]<<24)|((uint32_t)data[pos+t*4+1]<<16)|((uint32_t)data[pos+t*4+2]<<8)|data[pos+t*4+3];
        for (t=16;t<80;t++) w[t]=rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];
        for (t=0;t<80;t++){
            if(t<20){f=(b&c)|((~b)&d);k=0x5A827999;}
            else if(t<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(t<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else{f=b^c^d;k=0xCA62C1D6;}
            tmp=rol32(a,5)+f+e+k+w[t];e=d;d=c;c=rol32(b,30);b=a;a=tmp;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;pos+=64;
    }
    memset(blk,0,64);memcpy(blk,data+pos,len-pos);blk[len-pos]=0x80;
    if(len-pos>=56){
        for (t=0;t<16;t++) w[t]=((uint32_t)blk[t*4]<<24)|((uint32_t)blk[t*4+1]<<16)|((uint32_t)blk[t*4+2]<<8)|blk[t*4+3];
        for (t=16;t<80;t++) w[t]=rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];
        for (t=0;t<80;t++){
            if(t<20){f=(b&c)|((~b)&d);k=0x5A827999;}
            else if(t<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(t<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else{f=b^c^d;k=0xCA62C1D6;}
            tmp=rol32(a,5)+f+e+k+w[t];e=d;d=c;c=rol32(b,30);b=a;a=tmp;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
        memset(blk,0,64);
    }
    for(t=0;t<8;t++) blk[56+t]=(bits>>(56-t*8))&0xFF;
    for (t=0;t<16;t++) w[t]=((uint32_t)blk[t*4]<<24)|((uint32_t)blk[t*4+1]<<16)|((uint32_t)blk[t*4+2]<<8)|blk[t*4+3];
    for (t=16;t<80;t++) w[t]=rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
    a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];
    for (t=0;t<80;t++){
        if(t<20){f=(b&c)|((~b)&d);k=0x5A827999;}
        else if(t<40){f=b^c^d;k=0x6ED9EBA1;}
        else if(t<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
        else{f=b^c^d;k=0xCA62C1D6;}
        tmp=rol32(a,5)+f+e+k+w[t];e=d;d=c;c=rol32(b,30);b=a;a=tmp;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    for(t=0;t<5;t++) sprintf(out+t*8,"%08x",h[t]);
}

/* ── Big Number ops for RSA (kept minimal to avoid stack issues) ────── */
/* Full bn_ functions removed - using inline code in callback */

/* ── Console Callbacks ──────────────────────────────────────────────── */

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

static const char* hook_rsa_mod_exp_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 4) return "usage";
    /* All bignum state on heap to avoid stack overflow.
     * Using 32-bit words, little-endian byte order per word. */
    typedef struct { uint32_t w[64]; int nw; } bn;
    #define BN_MAX 64
    bn *ba = calloc(1, sizeof(bn));
    bn *be = calloc(1, sizeof(bn));
    bn *bm = calloc(1, sizeof(bn));
    bn *br = calloc(1, sizeof(bn));
    if (!ba||!be||!bm||!br) { free(ba);free(be);free(bm);free(br); return "oom"; }

    /* Parse hex to bn (little-endian bytes into little-endian words) */
    for (int p = 1; p <= 3; p++) {
        const char *hex = argv[p];
        bn *bnn = (p==1)?ba:(p==2)?be:bm;
        int hlen = strlen(hex);
        if (hlen % 2) hlen++; /* pad */
        int nbytes = hlen / 2;
        bnn->nw = (nbytes + 3) / 4;
        if (bnn->nw > BN_MAX) bnn->nw = BN_MAX;
        for (int i = 0; i < nbytes && i < BN_MAX*4; i++) {
            int hi = (hlen % 2) ? (hlen-1-i*2) : (hlen-2-i*2);
            if (hi < 0) break;
            char h[3] = {hex[hi], hex[hi+1], 0};
            unsigned int b; sscanf(h, "%02x", &b);
            bnn->w[i/4] |= b << ((i%4)*8);
        }
        while (bnn->nw > 1 && bnn->w[bnn->nw-1] == 0) bnn->nw--;
    }

    /* bn helpers as macros using heap temp */
    #define BNCPY(dst, src) memcpy((dst), (src), sizeof(bn))
    #define BNCMP(a, b) ({ int _m=(a)->nw>(b)->nw?(a)->nw:(b)->nw,_r=0; for(int _i=_m-1;_i>=0;_i--){uint32_t _a=(_i<(a)->nw)?(a)->w[_i]:0,_b=(_i<(b)->nw)?(b)->w[_i]:0;if(_a<_b){_r=-1;break;}if(_a>_b){_r=1;break;}} _r; })

    /* ba = ba mod bm */
    while (BNCMP(ba, bm) >= 0) {
        bn *sh = calloc(1, sizeof(bn));
        BNCPY(sh, bm);
        while (BNCMP(sh, ba) <= 0 && sh->nw < BN_MAX) {
            uint32_t c = 0;
            for (int i = 0; i < sh->nw; i++) {
                uint32_t nc = sh->w[i] >> 31;
                sh->w[i] = (sh->w[i] << 1) | c;
                c = nc;
            }
            if (c && sh->nw < BN_MAX) sh->w[sh->nw++] = c;
        }
        /* shift right 1 */
        uint32_t c = 0;
        for (int i = sh->nw-1; i >= 0; i--) {
            uint32_t nc = sh->w[i] & 1;
            sh->w[i] = (sh->w[i] >> 1) | (c << 31);
            c = nc;
        }
        while (sh->nw > 1 && sh->w[sh->nw-1] == 0) sh->nw--;
        /* ba -= sh */
        int64_t borrow = 0;
        for (int i = 0; i < ba->nw; i++) {
            int64_t diff = (int64_t)ba->w[i] - (i < sh->nw ? sh->w[i] : 0) - borrow;
            if (diff < 0) { diff += 0x100000000LL; borrow = 1; } else borrow = 0;
            ba->w[i] = (uint32_t)diff;
        }
        while (ba->nw > 1 && ba->w[ba->nw-1] == 0) ba->nw--;
        free(sh);
    }

    /* br = ba^be mod bm (square-and-multiply) */
    br->w[0] = 1; br->nw = 1;
    for (int i = be->nw-1; i >= 0; i--) {
        for (int bit = 31; bit >= 0; bit--) {
            /* br = br*br mod bm */
            bn *tmp = calloc(1, sizeof(bn));
            for (int j = 0; j < br->nw; j++) {
                uint64_t carry = 0;
                for (int k = 0; k < br->nw; k++) {
                    if (j+k >= BN_MAX) break;
                    uint64_t prod = (uint64_t)br->w[j]*br->w[k]+tmp->w[j+k]+carry;
                    tmp->w[j+k] = (uint32_t)prod;
                    carry = prod >> 32;
                }
                int kk = j+br->nw;
                while (carry && kk < BN_MAX) {
                    uint64_t s = tmp->w[kk]+carry;
                    tmp->w[kk] = (uint32_t)s; carry = s>>32; kk++;
                }
            }
            tmp->nw = br->nw*2; if (tmp->nw > BN_MAX) tmp->nw = BN_MAX;
            while (tmp->nw > 1 && tmp->w[tmp->nw-1] == 0) tmp->nw--;
            /* tmp = tmp mod bm */
            while (BNCMP(tmp, bm) >= 0) {
                bn *sh2 = calloc(1, sizeof(bn));
                BNCPY(sh2, bm);
                while (BNCMP(sh2, tmp) <= 0 && sh2->nw < BN_MAX) {
                    uint32_t c2 = 0;
                    for (int ii = 0; ii < sh2->nw; ii++) {
                        uint32_t nc2 = sh2->w[ii] >> 31;
                        sh2->w[ii] = (sh2->w[ii] << 1) | c2;
                        c2 = nc2;
                    }
                    if (c2 && sh2->nw < BN_MAX) sh2->w[sh2->nw++] = c2;
                }
                uint32_t c2 = 0;
                for (int ii = sh2->nw-1; ii >= 0; ii--) {
                    uint32_t nc2 = sh2->w[ii] & 1;
                    sh2->w[ii] = (sh2->w[ii] >> 1) | (c2 << 31);
                    c2 = nc2;
                }
                while (sh2->nw > 1 && sh2->w[sh2->nw-1] == 0) sh2->nw--;
                int64_t bor = 0;
                for (int ii = 0; ii < tmp->nw; ii++) {
                    int64_t dif = (int64_t)tmp->w[ii]-(ii<sh2->nw?sh2->w[ii]:0)-bor;
                    if (dif < 0) { dif += 0x100000000LL; bor = 1; } else bor = 0;
                    tmp->w[ii] = (uint32_t)dif;
                }
                while (tmp->nw > 1 && tmp->w[tmp->nw-1] == 0) tmp->nw--;
                free(sh2);
            }
            BNCPY(br, tmp);
            free(tmp);

            if ((be->w[i] >> bit) & 1) {
                /* br = br * ba mod bm */
                tmp = calloc(1, sizeof(bn));
                for (int j = 0; j < br->nw; j++) {
                    uint64_t carry = 0;
                    for (int k = 0; k < ba->nw; k++) {
                        if (j+k >= BN_MAX) break;
                        uint64_t prod = (uint64_t)br->w[j]*ba->w[k]+tmp->w[j+k]+carry;
                        tmp->w[j+k] = (uint32_t)prod;
                        carry = prod >> 32;
                    }
                    int kk = j+ba->nw;
                    while (carry && kk < BN_MAX) {
                        uint64_t s = tmp->w[kk]+carry;
                        tmp->w[kk] = (uint32_t)s; carry = s>>32; kk++;
                    }
                }
                tmp->nw = br->nw+ba->nw; if (tmp->nw > BN_MAX) tmp->nw = BN_MAX;
                while (tmp->nw > 1 && tmp->w[tmp->nw-1] == 0) tmp->nw--;
                while (BNCMP(tmp, bm) >= 0) {
                    bn *sh3 = calloc(1, sizeof(bn));
                    BNCPY(sh3, bm);
                    while (BNCMP(sh3, tmp) <= 0 && sh3->nw < BN_MAX) {
                        uint32_t c3 = 0;
                        for (int ii = 0; ii < sh3->nw; ii++) {
                            uint32_t nc3 = sh3->w[ii] >> 31;
                            sh3->w[ii] = (sh3->w[ii] << 1) | c3;
                            c3 = nc3;
                        }
                        if (c3 && sh3->nw < BN_MAX) sh3->w[sh3->nw++] = c3;
                    }
                    uint32_t c3 = 0;
                    for (int ii = sh3->nw-1; ii >= 0; ii--) {
                        uint32_t nc3 = sh3->w[ii] & 1;
                        sh3->w[ii] = (sh3->w[ii] >> 1) | (c3 << 31);
                        c3 = nc3;
                    }
                    while (sh3->nw > 1 && sh3->w[sh3->nw-1] == 0) sh3->nw--;
                    int64_t bor2 = 0;
                    for (int ii = 0; ii < tmp->nw; ii++) {
                        int64_t dif2 = (int64_t)tmp->w[ii]-(ii<sh3->nw?sh3->w[ii]:0)-bor2;
                        if (dif2 < 0) { dif2 += 0x100000000LL; bor2 = 1; } else bor2 = 0;
                        tmp->w[ii] = (uint32_t)dif2;
                    }
                    while (tmp->nw > 1 && tmp->w[tmp->nw-1] == 0) tmp->nw--;
                    free(sh3);
                }
                BNCPY(br, tmp);
                free(tmp);
            }
        }
    }

    static char result[1024];
    int pos = 0, started = 0;
    for (int i = br->nw-1; i >= 0; i--) {
        for (int byte = 3; byte >= 0; byte--) {
            uint8_t b = (br->w[i] >> (byte*8)) & 0xFF;
            if (!started && b == 0 && (i > 0 || byte > 0)) continue;
            started = 1;
            sprintf(result+pos, "%02x", b);
            pos += 2;
        }
    }
    result[pos] = '\0';
    if (pos == 0) strcpy(result, "00");

    free(ba); free(be); free(bm); free(br);
    #undef BNCPY
    #undef BNCMP
    return result;
}

static const char* hook_rand64_cb(void *obj, S32 argc, const char **argv) {
    static char buf[17];
    (void)obj; (void)argc; (void)argv;
    uint64_t val;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&val, sizeof(val), 1, f) != 1) {
            srand((unsigned)time(NULL) ^ (unsigned)getpid());
            val = ((uint64_t)rand() << 32) | (uint64_t)rand();
        }
        fclose(f);
    } else {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        val = ((uint64_t)rand() << 32) | (uint64_t)rand();
    }
    sprintf(buf, "%016llx", (unsigned long long)val);
    return buf;
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
                Con__addMethodS(NULL, "hook_rsa_mod_exp", hook_rsa_mod_exp_cb, "hook_rsa_mod_exp <base> <exp> <mod>", 4, 4);
                Con__addMethodS(NULL, "hook_rand64", hook_rand64_cb, "hook_rand64 - random 64-bit hex", 1, 1);
                fprintf(stderr, "[t2_hook] Registered 4 commands\n");
                break;
            }
        }
    }
    if (!registered) {
        fprintf(stderr, "[t2_hook] TIMEOUT\n");
    }
    return NULL;
}

static void __attribute__((constructor)) init(void) {
    fprintf(stderr, "[t2_hook] Constructor: starting poll thread\n");
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_create(&tid, &attr, poll_thread, NULL);
    pthread_attr_destroy(&attr);
}
