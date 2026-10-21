/* t2_realign: an LD_AUDIT module that lets Tribes 2 run on modern 32-bit
 * libraries.
 *
 * Tribes 2 and its Loki-era libraries were built when the i386 stack only had
 * to be 4-byte aligned. Current distributions build their 32-bit libraries
 * assuming the 16-byte alignment of the modern i386 ABI and use SSE stores
 * such as movaps on stack slots, which fault when an old caller arrives with a
 * misaligned stack (for example _XSend called from the bundled SDL's XSync).
 *
 * When an old object (anything outside the system library directories) binds
 * a function defined in a modern system library, this module returns a thunk
 * that realigns the stack, copies the arguments, and calls the real function.
 * glibc's core libraries are left alone: their entry points include setjmp and
 * other functions that must not run inside an extra frame, and they tolerate
 * the old alignment.
 *
 * Usage: LD_AUDIT=/path/to/t2_realign.so ./tribes2.dynamic
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { OBJECT_OTHER = 1, OBJECT_OLD = 2, OBJECT_MODERN = 3 };

/* Arguments copied to the realigned frame. cdecl callers clean up their own
   arguments, so copying more than a function takes is harmless: the extra
   words are read from the caller's frame and ignored. Sixteen words covers
   every X11, GL, and freetype entry point the game uses. */
#define THUNK_ARG_WORDS 16
#define THUNK_SIZE 48

static const unsigned char thunk_template[] = {
    0x55,                         /* push %ebp                      */
    0x89, 0xe5,                   /* mov  %esp,%ebp                 */
    0x83, 0xe4, 0xf0,             /* and  $-16,%esp                 */
    0x83, 0xec, THUNK_ARG_WORDS * 4, /* sub $64,%esp (stays aligned) */
    0xb9, THUNK_ARG_WORDS, 0, 0, 0, /* mov $16,%ecx                 */
    0x8b, 0x44, 0x8d, 0x04,       /* 1: mov 4(%ebp,%ecx,4),%eax     */
    0x89, 0x44, 0x8c, 0xfc,       /*    mov %eax,-4(%esp,%ecx,4)    */
    0x49,                         /*    dec  %ecx                   */
    0x75, 0xf5,                   /*    jnz  1b                     */
    0xb8, 0, 0, 0, 0,             /* mov  $target,%eax              */
    0xff, 0xd0,                   /* call *%eax                     */
    0xc9,                         /* leave                          */
    0xc3,                         /* ret                            */
};
#define THUNK_TARGET_OFFSET 26

struct thunk_entry {
    uintptr_t target;
    uintptr_t thunk;
};

static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;
static struct thunk_entry *entries;
static size_t entry_count, entry_capacity;
/* Thunks live in one memfd mapped twice: written through a writable view and
   run from an executable view, so no page ever changes protection while
   another thread may be running a thunk on it. */
#define THUNK_ARENA_SIZE (1024 * 1024)
static unsigned char *arena_write, *arena_exec;
static size_t arena_used;
static int debug;
static unsigned long thunked_bindings;

static uintptr_t make_thunk(uintptr_t target) {
    uintptr_t result = 0;
    pthread_mutex_lock(&thunk_lock);
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].target == target) {
            result = entries[i].thunk;
            goto done;
        }
    }
    if (entry_count == entry_capacity) {
        size_t capacity = entry_capacity ? entry_capacity * 2 : 256;
        struct thunk_entry *grown = realloc(entries, capacity * sizeof(*grown));
        if (!grown) goto done;
        entries = grown;
        entry_capacity = capacity;
    }
    if (!arena_exec) {
        int fd = memfd_create("t2_realign", MFD_CLOEXEC);
        if (fd < 0) goto done;
        if (ftruncate(fd, THUNK_ARENA_SIZE) == 0) {
            arena_write = mmap(NULL, THUNK_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            arena_exec = mmap(NULL, THUNK_ARENA_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        }
        close(fd);
        if (arena_write == MAP_FAILED || arena_exec == MAP_FAILED || !arena_write || !arena_exec) {
            arena_write = arena_exec = NULL;
            goto done;
        }
    }
    if (arena_used + THUNK_SIZE > THUNK_ARENA_SIZE) goto done;
    unsigned char *thunk = arena_write + arena_used;
    memcpy(thunk, thunk_template, sizeof(thunk_template));
    uint32_t address = (uint32_t)target;
    memcpy(thunk + THUNK_TARGET_OFFSET, &address, sizeof(address));
    __builtin___clear_cache((char *)thunk, (char *)thunk + THUNK_SIZE);
    entries[entry_count].target = target;
    entries[entry_count].thunk = (uintptr_t)(arena_exec + arena_used);
    arena_used += THUNK_SIZE;
    entry_count++;
    result = entries[entry_count - 1].thunk;
done:
    pthread_mutex_unlock(&thunk_lock);
    return result ? result : target;
}

/* glXGetProcAddress hands out extension pointers outside the dynamic linker,
   so the old caller gets a wrapper that thunks each pointer it returns. */
typedef void *(*get_proc_address_t)(const unsigned char *);
static get_proc_address_t real_get_proc_address, real_get_proc_address_arb;

__attribute__((force_align_arg_pointer))
static void *wrapped_get_proc_address(const unsigned char *name) {
    void *pointer = real_get_proc_address ? real_get_proc_address(name) : NULL;
    return pointer ? (void *)make_thunk((uintptr_t)pointer) : NULL;
}

__attribute__((force_align_arg_pointer))
static void *wrapped_get_proc_address_arb(const unsigned char *name) {
    void *pointer = real_get_proc_address_arb ? real_get_proc_address_arb(name) : NULL;
    return pointer ? (void *)make_thunk((uintptr_t)pointer) : NULL;
}

static int has_prefix(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int classify(const char *path) {
    if (!path || !path[0]) return OBJECT_OLD;  /* the main executable */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strstr(base, "t2_realign") || strstr(base, "t2_hook") || has_prefix(base, "linux-gate"))
        return OBJECT_OTHER;  /* built with -mstackrealign, or the vDSO */
    if (has_prefix(path, "/usr/lib32/") || has_prefix(path, "/lib32/") ||
        has_prefix(path, "/usr/lib/i386-linux-gnu/") || has_prefix(path, "/lib/i386-linux-gnu/") ||
        has_prefix(path, "/usr/lib/") || has_prefix(path, "/lib/")) {
        static const char *core[] = {"libc.so", "libpthread.so", "libdl.so", "ld-linux",
                                     "librt.so", "libresolv.so", "libnss_", "libutil.so"};
        for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++)
            if (has_prefix(base, core[i])) return OBJECT_OTHER;
        return OBJECT_MODERN;
    }
    return OBJECT_OLD;
}

unsigned int la_version(unsigned int version) {
    const char *flag = getenv("T2LAUNCH_REALIGN_DEBUG");
    debug = flag && strcmp(flag, "1") == 0;
    return version < LAV_CURRENT ? version : LAV_CURRENT;
}

unsigned int la_objopen(struct link_map *map, Lmid_t lmid, uintptr_t *cookie) {
    (void)lmid;
    int kind = classify(map->l_name);
    *cookie = (uintptr_t)kind;
    if (debug)
        fprintf(stderr, "[t2_realign] %s: %s\n",
                kind == OBJECT_OLD ? "old" : kind == OBJECT_MODERN ? "modern" : "other",
                map->l_name[0] ? map->l_name : "(main program)");
    return LA_FLG_BINDTO | LA_FLG_BINDFROM;
}

uintptr_t la_symbind32(Elf32_Sym *sym, unsigned int ndx, uintptr_t *refcook,
                       uintptr_t *defcook, unsigned int *flags, const char *symname) {
    (void)ndx; (void)flags;
    unsigned int type = ELF32_ST_TYPE(sym->st_info);
    if (*refcook != OBJECT_OLD || *defcook != OBJECT_MODERN ||
        (type != STT_FUNC && type != STT_GNU_IFUNC) || !sym->st_value)
        return sym->st_value;
    if (strcmp(symname, "glXGetProcAddress") == 0) {
        real_get_proc_address = (get_proc_address_t)(uintptr_t)sym->st_value;
        return (uintptr_t)wrapped_get_proc_address;
    }
    if (strcmp(symname, "glXGetProcAddressARB") == 0) {
        real_get_proc_address_arb = (get_proc_address_t)(uintptr_t)sym->st_value;
        return (uintptr_t)wrapped_get_proc_address_arb;
    }
    thunked_bindings++;
    if (debug && thunked_bindings <= 4000)
        fprintf(stderr, "[t2_realign] thunk %s\n", symname);
    return make_thunk(sym->st_value);
}
