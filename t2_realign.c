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
#define OBJECT_KIND(cookie) ((cookie) & 0xff)
#define OBJECT_OPENAL 0x100  /* cookie flag: the game's Loki libopenal.so.0 */

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

/* Volume fix for Loki's OpenAL. Torque sets every source's gain with Loki's
 * linear AL_GAIN_LINEAR_LOKI (0x20000) and plays 2D sounds (interface, shell
 * hum, music) as AL_SOURCE_RELATIVE. Loki applies source gain inside its
 * distance-attenuation filter (alf_da), which returns early for relative
 * sources, so their gain is ignored and every volume slider has no effect.
 * Loki's min/max filter still runs for relative sources, so for those the
 * wrappers below also set AL_MAX_GAIN to the value whose clamp produces the
 * requested linear gain. Loki maps AL_MAX_GAIN non-linearly; the table was
 * measured from its output (clean scaling, no clipping).
 *
 * On unless T2LAUNCH_OPENAL_VOLUME_FIX=0. T2LAUNCH_REALIGN_TRACE_AL=1 also
 * logs each gain call. */
typedef int ALenum;
typedef unsigned int ALuint;
typedef float ALfloat;
#define AL_SOURCE_RELATIVE 0x202
#define AL_MAX_GAIN 0x100E
#define AL_GAIN_LINEAR_LOKI 0x20000
typedef void (*al_sourcef_t)(ALuint, ALenum, ALfloat);
typedef void (*al_sourcei_t)(ALuint, ALenum, int);
typedef void *(*alc_create_context_t)(void *, const int *);
static int trace_al, volume_fix;
static al_sourcef_t real_al_sourcef;
static al_sourcei_t real_al_sourcei;
static alc_create_context_t real_alc_create_context;

/* Per-source state; Loki numbers sources from 16385 within each context. */
#define SOURCE_BASE 16384u
#define SOURCE_SLOTS 4096u
static struct { unsigned char relative, capped; float gain; } sources[SOURCE_SLOTS];
static pthread_mutex_t sources_lock = PTHREAD_MUTEX_INITIALIZER;

/* Measured Loki response: AL_MAX_GAIN value -> output amplitude ratio. */
static const float max_gain_curve[][2] = {
    {0.0f, 0.0f}, {0.001f, 0.0078f}, {0.01f, 0.0469f}, {0.02f, 0.1016f}, {0.05f, 0.2266f},
    {0.1f, 0.4140f}, {0.2f, 0.6797f}, {0.3f, 0.8046f}, {0.4f, 0.8906f}, {0.5f, 0.9140f},
    {0.6f, 0.9218f}, {0.7f, 0.9375f}, {0.8f, 0.9531f}, {0.9f, 0.9687f}, {1.0f, 1.0f},
};

static float max_gain_for(float gain) {
    const size_t points = sizeof(max_gain_curve) / sizeof(max_gain_curve[0]);
    if (!(gain > 0.0f)) return 0.0f;
    if (gain >= 1.0f) return 1.0f;
    for (size_t i = 1; i < points; i++) {
        float x0 = max_gain_curve[i - 1][0], y0 = max_gain_curve[i - 1][1];
        float x1 = max_gain_curve[i][0], y1 = max_gain_curve[i][1];
        if (gain <= y1) return x0 + (x1 - x0) * (gain - y0) / (y1 - y0);
    }
    return 1.0f;
}

/* Apply the cap for the slot's current state; call with sources_lock held. */
static void apply_relative_gain(ALuint source, unsigned slot) {
    if (sources[slot].relative) {
        real_al_sourcef(source, AL_MAX_GAIN, max_gain_for(sources[slot].gain));
        sources[slot].capped = 1;
    } else if (sources[slot].capped) {
        real_al_sourcef(source, AL_MAX_GAIN, 1.0f);
        sources[slot].capped = 0;
    }
}

__attribute__((force_align_arg_pointer))
static void wrapped_al_sourcef(ALuint source, ALenum param, ALfloat value) {
    real_al_sourcef(source, param, value);
    if (trace_al && param == AL_GAIN_LINEAR_LOKI)
        fprintf(stderr, "[t2_realign] alSourcef(%u, gain, %g)\n", source, (double)value);
    unsigned slot = source - SOURCE_BASE;
    if (!volume_fix || param != AL_GAIN_LINEAR_LOKI || slot >= SOURCE_SLOTS) return;
    pthread_mutex_lock(&sources_lock);
    sources[slot].gain = value;
    apply_relative_gain(source, slot);
    pthread_mutex_unlock(&sources_lock);
}

__attribute__((force_align_arg_pointer))
static void wrapped_al_sourcei(ALuint source, ALenum param, int value) {
    real_al_sourcei(source, param, value);
    unsigned slot = source - SOURCE_BASE;
    if (!volume_fix || param != AL_SOURCE_RELATIVE || slot >= SOURCE_SLOTS) return;
    pthread_mutex_lock(&sources_lock);
    sources[slot].relative = value != 0;
    apply_relative_gain(source, slot);
    pthread_mutex_unlock(&sources_lock);
}

__attribute__((force_align_arg_pointer))
static void *wrapped_alc_create_context(void *device, const int *attributes) {
    void *context = real_alc_create_context(device, attributes);
    /* A new context starts with default sources; forget the old ones. */
    pthread_mutex_lock(&sources_lock);
    for (unsigned i = 0; i < SOURCE_SLOTS; i++) {
        sources[i].relative = sources[i].capped = 0;
        sources[i].gain = 1.0f;
    }
    pthread_mutex_unlock(&sources_lock);
    return context;
}

static uintptr_t wrap_openal_binding(const char *symname, uintptr_t address) {
    if (strcmp(symname, "alSourcef") == 0) {
        real_al_sourcef = (al_sourcef_t)address;
        return (uintptr_t)wrapped_al_sourcef;
    }
    if (strcmp(symname, "alSourcei") == 0) {
        real_al_sourcei = (al_sourcei_t)address;
        return (uintptr_t)wrapped_al_sourcei;
    }
    if (strcmp(symname, "alcCreateContext") == 0) {
        real_alc_create_context = (alc_create_context_t)address;
        return (uintptr_t)wrapped_alc_create_context;
    }
    return address;
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
    const char *al_flag = getenv("T2LAUNCH_REALIGN_TRACE_AL");
    trace_al = al_flag && strcmp(al_flag, "1") == 0;
    const char *fix_flag = getenv("T2LAUNCH_OPENAL_VOLUME_FIX");
    volume_fix = !fix_flag || strcmp(fix_flag, "0") != 0;
    return version < LAV_CURRENT ? version : LAV_CURRENT;
}

unsigned int la_objopen(struct link_map *map, Lmid_t lmid, uintptr_t *cookie) {
    (void)lmid;
    int kind = classify(map->l_name);
    const char *base = strrchr(map->l_name, '/');
    base = base ? base + 1 : map->l_name;
    *cookie = (uintptr_t)kind | (strcmp(base, "libopenal.so.0") == 0 ? OBJECT_OPENAL : 0);
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
    if ((trace_al || volume_fix) && (*defcook & OBJECT_OPENAL) &&
        OBJECT_KIND(*refcook) == OBJECT_OLD && !(*refcook & OBJECT_OPENAL) && sym->st_value)
        return wrap_openal_binding(symname, sym->st_value);
    if (OBJECT_KIND(*refcook) != OBJECT_OLD || OBJECT_KIND(*defcook) != OBJECT_MODERN ||
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
