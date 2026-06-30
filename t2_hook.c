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
    static char result[2048];
    (void)obj;
    if (argc < 4) return "usage";
    fprintf(stderr, "[t2_hook] rsa_mod_exp b=%.20s... e=%.20s... m=%.20s...\n",
        argv[1], argv[2], argv[3]);
    /* Use precompiled GMP helper for fast modexp */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "/usr/lib/rsa_helper %s %s %s 2>/dev/null",
             argv[1], argv[2], argv[3]);
    FILE *fp = popen(cmd, "r");
    if (!fp) { strcpy(result, "ERR:popen"); return result; }
    if (!fgets(result, sizeof(result), fp)) {
        pclose(fp);
        strcpy(result, "ERR:no_output");
        return result;
    }
    pclose(fp);
    /* strip newline */
    result[strcspn(result, "\r\n")] = 0;
    /* lowercase */
    for (int i = 0; result[i]; i++)
        if (result[i] >= 'A' && result[i] <= 'F')
            result[i] += 32;
    fprintf(stderr, "[t2_hook] rsa_mod_exp result=%.40s...\n", result);
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

/* ── RC4 implementation in C (correct, no TS──────── */

static void rc4_ksa(uint8_t *S, const uint8_t *key, int keylen) {
    for (int i = 0; i < 256; i++) S[i] = i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % keylen]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }
}

static void rc4_drop(uint8_t *S, int n) {
    int i = 0, j = 0;
    for (int k = 0; k < n; k++) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }
}

static void rc4_crypt(uint8_t *S, const uint8_t *in, uint8_t *out, int len) {
    int i = 0, j = 0;
    for (int k = 0; k < len; k++) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
        out[k] = in[k] ^ S[(S[i] + S[j]) & 0xFF];
    }
}

static int hex2nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int max_out) {
    int len = 0;
    for (int i = 0; hex[i] && hex[i+1] && len < max_out; i += 2) {
        int hi = hex2nibble(hex[i]);
        int lo = hex2nibble(hex[i+1]);
        if (hi < 0 || lo < 0) return -1;
        out[len++] = (hi << 4) | lo;
    }
    return len;
}

static void bytes_to_hex(const uint8_t *data, int len, char *out) {
    for (int i = 0; i < len; i++)
        sprintf(out + i*2, "%02x", data[i]);
    out[len*2] = '\0';
}

static const char* hook_login_cb(void *obj, S32 argc, const char **argv) {
    static char result[2048];
    (void)obj; (void)argc;

    const char *username = "LouCypher";
    if (argc >= 2) username = argv[1];

    /* ── Read password from file ── */
    char password[256] = {0};
    FILE *fp = fopen("/home/loki/.loki/tribes2/password", "r");
    if (!fp) { snprintf(result, sizeof(result), "ERR:no_pw_file"); return result; }
    if (!fgets(password, sizeof(password), fp)) { fclose(fp); snprintf(result, sizeof(result), "ERR:empty_pw"); return result; }
    fclose(fp);
    password[strcspn(password, "\r\n")] = 0;
    fprintf(stderr, "[t2_hook] login: pw=%s (len=%zu)\n", password, strlen(password));

    /* ── Read private.store ── */
    char store_path[512];
    snprintf(store_path, sizeof(store_path), "/home/loki/game/base/private.store");
    FILE *fs = fopen(store_path, "r");
    if (!fs) { snprintf(result, sizeof(result), "ERR:no_store"); return result; }
    char line[2048], enc_data[2048] = {0};
    while (fgets(line, sizeof(line), fs)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcmp(line, username) == 0) {
            strncpy(enc_data, tab + 1, sizeof(enc_data) - 1);
            break;
        }
    }
    fclose(fs);
    if (!enc_data[0]) { snprintf(result, sizeof(result), "ERR:no_user"); return result; }

    /* ── Parse nonce:ciphertext ── */
    char *colon = strchr(enc_data, ':');
    if (!colon) { snprintf(result, sizeof(result), "ERR:bad_fmt"); return result; }
    *colon = 0;
    char *nonce_hex = enc_data;
    char *ct_hex = colon + 1;

    /* ── Compute RC4 key = SHA1(password + nonce) ── */
    char pw_nonce[512];
    snprintf(pw_nonce, sizeof(pw_nonce), "%s%s", password, nonce_hex);
    char key_hex[41];
    sha1((const uint8_t *)pw_nonce, strlen(pw_nonce), key_hex);

    uint8_t key_bytes[20];
    for (int i = 0; i < 20; i++) {
        unsigned int b;
        sscanf(key_hex + i*2, "%02x", &b);
        key_bytes[i] = (uint8_t)b;
    }

    /* ── Decode ciphertext hex ── */
    int ct_len = strlen(ct_hex) / 2;
    uint8_t ct[1024], pt[1024];
    if (ct_len > sizeof(ct)) { snprintf(result, sizeof(result), "ERR:ct_too_long"); return result; }
    for (int i = 0; i < ct_len; i++) {
        unsigned int b;
        sscanf(ct_hex + i*2, "%02x", &b);
        ct[i] = (uint8_t)b;
    }

    /* ── RC4 KSA ── */
    uint8_t S[256];
    for (int i = 0; i < 256; i++) S[i] = i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key_bytes[i % 20]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }

    /* ── RC4 drop 2048 ── */
    int ii = 0, jj = 0;
    for (int k = 0; k < 2048; k++) {
        ii = (ii + 1) & 0xFF;
        jj = (jj + S[ii]) & 0xFF;
        uint8_t tmp = S[ii]; S[ii] = S[jj]; S[jj] = tmp;
    }

    /* ── RC4 PRGA + decrypt ── */
    for (int k = 0; k < ct_len; k++) {
        ii = (ii + 1) & 0xFF;
        jj = (jj + S[ii]) & 0xFF;
        uint8_t tmp = S[ii]; S[ii] = S[jj]; S[jj] = tmp;
        pt[k] = ct[k] ^ S[(S[ii] + S[jj]) & 0xFF];
    }

    /* ── Convert plaintext bytes to hex string ── */
    for (int i = 0; i < ct_len; i++)
        sprintf(result + i*2, "%02x", pt[i]);
    result[ct_len*2] = 0;
    fprintf(stderr, "[t2_hook] login: decrypted %d bytes = %.40s...\n", ct_len, result);
    return result;
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
                Con__addMethodS(NULL, "hook_login", hook_login_cb, "hook_login [username] - C-based RC4 decrypt", 1, 2);
                fprintf(stderr, "[t2_hook] Registered 5 commands\n");
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
