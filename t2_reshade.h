/* ReShade FX runner for the hook (included by t2_hook.c, single translation unit).
 *
 * Runs ReShade .fx effects (compiled to GLSL by t2_fx.so) and ReShade preset
 * files on the game's finished frame:
 *
 *   video.reshade          1 enables the runner
 *   video.reshade_preset   preset .ini path (a bare name is looked up in <shaders>/Presets)
 *   video.reshade_shaders  directory with Shaders/ and Textures/ (the reshade-shaders layout)
 *
 * Effect space follows ReShade: texture row 0 is the top. The game's frame
 * (OpenGL, row 0 at the bottom) is flipped into the first buffer and the
 * result is flipped back, so effects never see an upside-down image.
 *
 * Everything here runs on the game thread with the game's GL context current,
 * inside post_begin()/post_end(), which already save the fixed-function state.
 * Framebuffer bindings, programs, samplers and buffer bindings are restored by
 * rs_run itself.
 */
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "t2_fx.h"

#define RS_TEXTURE_2D 0x0DE1
#define RS_RGBA8 0x8058
#define RS_RGBA 0x1908
#define RS_UNSIGNED_BYTE 0x1401
#define RS_FLOAT 0x1406
#define RS_FRAMEBUFFER 0x8D40
#define RS_READ_FRAMEBUFFER 0x8CA8
#define RS_DRAW_FRAMEBUFFER 0x8CA9
#define RS_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define RS_READ_FRAMEBUFFER_BINDING 0x8CAA
#define RS_COLOR_ATTACHMENT0 0x8CE0
#define RS_FRAMEBUFFER_COMPLETE 0x8CD5
#define RS_COLOR_BUFFER_BIT 0x4000
#define RS_NEAREST 0x2600
#define RS_LINEAR 0x2601
#define RS_NEAREST_MIPMAP_NEAREST 0x2700
#define RS_LINEAR_MIPMAP_NEAREST 0x2701
#define RS_NEAREST_MIPMAP_LINEAR 0x2702
#define RS_LINEAR_MIPMAP_LINEAR 0x2703
#define RS_TEXTURE_MIN_FILTER 0x2801
#define RS_TEXTURE_MAG_FILTER 0x2800
#define RS_TEXTURE_WRAP_S 0x2802
#define RS_TEXTURE_WRAP_T 0x2803
#define RS_TEXTURE_MIN_LOD 0x813A
#define RS_TEXTURE_MAX_LOD 0x813B
#define RS_TEXTURE_LOD_BIAS 0x8501
#define RS_REPEAT 0x2901
#define RS_MIRRORED_REPEAT 0x8370
#define RS_CLAMP_TO_EDGE 0x812F
#define RS_CLAMP_TO_BORDER 0x812D
#define RS_UNIFORM_BUFFER 0x8A11
#define RS_DYNAMIC_DRAW 0x88E8
#define RS_VERTEX_SHADER 0x8B31
#define RS_FRAGMENT_SHADER 0x8B30
#define RS_COMPILE_STATUS 0x8B81
#define RS_LINK_STATUS 0x8B82
#define RS_INFO_LOG_LENGTH 0x8B84
#define RS_BLEND 0x0BE2
#define RS_FRAMEBUFFER_SRGB 0x8DB9
#define RS_CURRENT_PROGRAM 0x8B8D
#define RS_ACTIVE_TEXTURE 0x84E0
#define RS_TEXTURE0 0x84C0
#define RS_MAX_COLOR_ATTACHMENTS_USED 8

static struct {
    int state;  /* 0 unresolved, 1 ready, -1 unavailable */
    void (*GenTextures)(int, unsigned *);
    void (*DeleteTextures)(int, const unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*ActiveTexture)(unsigned);
    void (*GenerateMipmap)(unsigned);
    void (*GenFramebuffers)(int, unsigned *);
    void (*DeleteFramebuffers)(int, const unsigned *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*CheckFramebufferStatus)(unsigned);
    void (*DrawBuffers)(int, const unsigned *);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    void (*Viewport)(int, int, int, int);
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(unsigned);
    void (*Enable)(unsigned);
    void (*Disable)(unsigned);
    void (*DrawArrays)(unsigned, int, int);
    unsigned (*CreateShader)(unsigned);
    void (*ShaderSource)(unsigned, int, const char *const *, const int *);
    void (*CompileShader)(unsigned);
    void (*GetShaderiv)(unsigned, unsigned, int *);
    void (*GetShaderInfoLog)(unsigned, int, int *, char *);
    void (*DeleteShader)(unsigned);
    unsigned (*CreateProgram)(void);
    void (*AttachShader)(unsigned, unsigned);
    void (*LinkProgram)(unsigned);
    void (*GetProgramiv)(unsigned, unsigned, int *);
    void (*GetProgramInfoLog)(unsigned, int, int *, char *);
    void (*DeleteProgram)(unsigned);
    void (*UseProgram)(unsigned);
    void (*GenBuffers)(int, unsigned *);
    void (*DeleteBuffers)(int, const unsigned *);
    void (*BindBuffer)(unsigned, unsigned);
    void (*BufferData)(unsigned, long, const void *, unsigned);
    void (*BindBufferBase)(unsigned, unsigned, unsigned);
    void (*GenSamplers)(int, unsigned *);
    void (*DeleteSamplers)(int, const unsigned *);
    void (*BindSampler)(unsigned, unsigned);
    void (*SamplerParameteri)(unsigned, unsigned, int);
    void (*SamplerParameterf)(unsigned, unsigned, float);
    void (*GetIntegerv)(unsigned, int *);
    void (*BlendFuncSeparate)(unsigned, unsigned, unsigned, unsigned);
    void (*BlendEquationSeparate)(unsigned, unsigned);
    void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*PixelStorei)(unsigned, int);
    void (*CopyTexSubImage2D)(unsigned, int, int, int, int, int, int, int);
    int (*GetUniformLocation)(unsigned, const char *);
    void (*Uniform1i)(int, int);
} rs_gl;

static int rs_resolve_gl(void) {
    if (rs_gl.state) return rs_gl.state > 0;
    rs_gl.state = -1;
    void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    void *scope = gl ? gl : RTLD_DEFAULT;
    void *(*get_proc)(const unsigned char *) =
        (void *(*)(const unsigned char *))dlsym(scope, "glXGetProcAddressARB");
#define RS_FN(field, name) \
    do { \
        *(void **)&rs_gl.field = get_proc ? get_proc((const unsigned char *)name) : NULL; \
        if (!rs_gl.field) *(void **)&rs_gl.field = dlsym(scope, name); \
        if (!rs_gl.field) { fprintf(stderr, "[t2_hook] ReShade unavailable: no %s\n", name); return 0; } \
    } while (0)
    RS_FN(GenTextures, "glGenTextures"); RS_FN(DeleteTextures, "glDeleteTextures");
    RS_FN(BindTexture, "glBindTexture"); RS_FN(TexImage2D, "glTexImage2D");
    RS_FN(TexParameteri, "glTexParameteri"); RS_FN(ActiveTexture, "glActiveTexture");
    RS_FN(GenerateMipmap, "glGenerateMipmap"); RS_FN(GenFramebuffers, "glGenFramebuffers");
    RS_FN(DeleteFramebuffers, "glDeleteFramebuffers"); RS_FN(BindFramebuffer, "glBindFramebuffer");
    RS_FN(FramebufferTexture2D, "glFramebufferTexture2D");
    RS_FN(CheckFramebufferStatus, "glCheckFramebufferStatus"); RS_FN(DrawBuffers, "glDrawBuffers");
    RS_FN(BlitFramebuffer, "glBlitFramebuffer"); RS_FN(Viewport, "glViewport");
    RS_FN(ClearColor, "glClearColor"); RS_FN(Clear, "glClear"); RS_FN(Enable, "glEnable");
    RS_FN(Disable, "glDisable"); RS_FN(DrawArrays, "glDrawArrays");
    RS_FN(CreateShader, "glCreateShader"); RS_FN(ShaderSource, "glShaderSource");
    RS_FN(CompileShader, "glCompileShader"); RS_FN(GetShaderiv, "glGetShaderiv");
    RS_FN(GetShaderInfoLog, "glGetShaderInfoLog"); RS_FN(DeleteShader, "glDeleteShader");
    RS_FN(CreateProgram, "glCreateProgram"); RS_FN(AttachShader, "glAttachShader");
    RS_FN(LinkProgram, "glLinkProgram"); RS_FN(GetProgramiv, "glGetProgramiv");
    RS_FN(GetProgramInfoLog, "glGetProgramInfoLog"); RS_FN(DeleteProgram, "glDeleteProgram");
    RS_FN(UseProgram, "glUseProgram"); RS_FN(GenBuffers, "glGenBuffers");
    RS_FN(DeleteBuffers, "glDeleteBuffers"); RS_FN(BindBuffer, "glBindBuffer");
    RS_FN(BufferData, "glBufferData"); RS_FN(BindBufferBase, "glBindBufferBase");
    RS_FN(GenSamplers, "glGenSamplers"); RS_FN(DeleteSamplers, "glDeleteSamplers");
    RS_FN(BindSampler, "glBindSampler"); RS_FN(SamplerParameteri, "glSamplerParameteri");
    RS_FN(SamplerParameterf, "glSamplerParameterf"); RS_FN(GetIntegerv, "glGetIntegerv");
    RS_FN(BlendFuncSeparate, "glBlendFuncSeparate"); RS_FN(BlendEquationSeparate, "glBlendEquationSeparate");
    RS_FN(ColorMask, "glColorMask"); RS_FN(PixelStorei, "glPixelStorei");
    RS_FN(CopyTexSubImage2D, "glCopyTexSubImage2D"); RS_FN(GetUniformLocation, "glGetUniformLocation");
    RS_FN(Uniform1i, "glUniform1i");
#undef RS_FN
    rs_gl.state = 1;
    return 1;
}

/* ── Small helpers ─────────────────────────────────────────────────── */

/* Value of "key" in a "key=value\n..." annotation list. */
static int rs_annotation(const char *list, const char *key, char *out, size_t size) {
    size_t length = strlen(key);
    for (const char *line = list; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t line_length = end ? (size_t)(end - line) : strlen(line);
        if (line_length > length && !strncmp(line, key, length) && line[length] == '=') {
            size_t value = line_length - length - 1;
            if (value >= size) value = size - 1;
            memcpy(out, line + length + 1, value);
            out[value] = '\0';
            return 1;
        }
        line = end ? end + 1 : NULL;
    }
    return 0;
}

static char *rs_trim(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' ||
                      text[length - 1] == '\n')) text[--length] = '\0';
    return text;
}

static int rs_text_equal(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

static void rs_make_depth_resources(int width, int height);

/* ── State ─────────────────────────────────────────────────────────── */

#define RS_MAX_EFFECTS 64
#define RS_MAX_TECHNIQUES 128
#define RS_MAX_FILES 4096
#define RS_MAX_DEFINES 128

struct rs_file { char *name; char *path; };

struct rs_pass {
    unsigned program;
    t2fx_pass info;
    unsigned vertex_count;
};

struct rs_texture {
    unsigned id;
    t2fx_texture info;
    int kind;  /* 0 own render target / image, 1 COLOR (backbuffer), 2 DEPTH */
    int width, height;
};

struct rs_uniform_value {
    t2fx_uniform info;
    char source[32];
    float minimum, maximum;
};

struct rs_effect {
    char file[256];
    char path[1024];
    t2fx_effect *fx;
    unsigned ubo;
    unsigned char *ubo_data;
    unsigned ubo_size;
    struct rs_texture *textures; int texture_count;
    unsigned *samplers; int sampler_count;
    t2fx_sampler *sampler_info; char **sampler_annotations;
    struct rs_uniform_value *uniforms; int uniform_count;
    struct rs_pass **passes;     /* [technique][pass] flattened with technique_offset */
    int *technique_offset, *technique_passes; int technique_count;
    char **technique_names;
    char defines[RS_MAX_DEFINES][160]; int define_count;
    int ok;
};

struct rs_technique { struct rs_effect *effect; int index; };

static struct {
    int enabled, initialised, failed;
    void *lib;
    t2fx_effect *(*compile)(const char *, const char *const *, int, const char *const *, int, int, int, char *, int);
    void (*free_effect)(t2fx_effect *);
    const char *(*log)(const t2fx_effect *);
    unsigned (*buffer_size)(const t2fx_effect *);
    int (*texture_count)(const t2fx_effect *);
    int (*texture_get)(const t2fx_effect *, int, t2fx_texture *);
    int (*sampler_count)(const t2fx_effect *);
    int (*sampler_get)(const t2fx_effect *, int, t2fx_sampler *);
    int (*uniform_count)(const t2fx_effect *);
    int (*uniform_get)(const t2fx_effect *, int, t2fx_uniform *);
    int (*technique_count)(const t2fx_effect *);
    const char *(*technique_name)(const t2fx_effect *, int);
    int (*pass_count)(const t2fx_effect *, int);
    int (*pass_get)(const t2fx_effect *, int, int, t2fx_pass *);
    const char *(*shader_source)(t2fx_effect *, const char *);
    unsigned char *(*load_image)(const char *, int *, int *);
    void (*free_image)(unsigned char *);

    char shaders_dir[1024], preset_path[1024];
    struct rs_file shader_files[RS_MAX_FILES]; int shader_file_count;
    struct rs_file texture_files[RS_MAX_FILES]; int texture_file_count;
    char **include_dirs; int include_count;
    time_t preset_mtime; int reload_check;

    struct rs_effect effects[RS_MAX_EFFECTS]; int effect_count;
    struct rs_technique chain[RS_MAX_TECHNIQUES]; int chain_count;

    int width, height;           /* buffer size the effects were compiled for */
    unsigned buffer[2], output, fbo, read_fbo, dummy_depth, white;
    unsigned depth_copy, depth_flipped, depth_program;   /* hardware depth, then top-row-first R32F */
    int need_depth, depth_taken, paused, reload_now;
    double frustum_near, frustum_far;       /* committed from the last frame's glFrustum calls */
    double frame_near, frame_far;           /* being collected this frame */
    double depth_ratio_used;                /* far / near the effects were compiled for */
    unsigned long frame;
    double start_time, last_time, frame_time_ms;
    char global_defines[RS_MAX_DEFINES][160]; int global_define_count;
} rs;

static double rs_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── File discovery ────────────────────────────────────────────────── */

static void rs_scan(const char *dir, struct rs_file *files, int *count, int fx_only, int depth) {
    DIR *handle = opendir(dir);
    if (!handle || depth > 8) { if (handle) closedir(handle); return; }
    struct dirent *entry;
    while ((entry = readdir(handle))) {
        if (entry->d_name[0] == '.') continue;
        char path[1280];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        struct stat info;
        if (stat(path, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            rs_scan(path, files, count, fx_only, depth + 1);
        } else if (*count < RS_MAX_FILES) {
            size_t length = strlen(entry->d_name);
            if (fx_only && (length < 3 || strcasecmp(entry->d_name + length - 3, ".fx") != 0)) continue;
            files[*count].name = strdup(entry->d_name);
            files[*count].path = strdup(path);
            (*count)++;
        }
    }
    closedir(handle);
}

static void rs_collect_dirs(const char *dir, char ***list, int *count, int depth) {
    if (depth > 8) return;
    *list = realloc(*list, sizeof(char *) * (size_t)(*count + 1));
    (*list)[(*count)++] = strdup(dir);
    DIR *handle = opendir(dir);
    if (!handle) return;
    struct dirent *entry;
    while ((entry = readdir(handle))) {
        if (entry->d_name[0] == '.') continue;
        char path[1280];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        struct stat info;
        if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) rs_collect_dirs(path, list, count, depth + 1);
    }
    closedir(handle);
}

static const char *rs_find_file(struct rs_file *files, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (rs_text_equal(files[i].name, name)) return files[i].path;
    return NULL;
}

/* ── Library ───────────────────────────────────────────────────────── */

static int rs_load_library(void) {
    if (rs.lib) return 1;
    char path[1024] = "";
    Dl_info where;
    if (dladdr((void *)rs_load_library, &where) && where.dli_fname) {
        snprintf(path, sizeof path, "%s", where.dli_fname);
        char *slash = strrchr(path, '/');
        if (slash) { slash[1] = '\0'; strncat(path, "t2_fx.so", sizeof path - strlen(path) - 1); }
        else snprintf(path, sizeof path, "t2_fx.so");
    }
    const char *override = getenv("T2LAUNCH_FX_LIB");
    rs.lib = dlopen(override && *override ? override : path, RTLD_NOW | RTLD_LOCAL);
    if (!rs.lib) rs.lib = dlopen("./t2_fx.so", RTLD_NOW | RTLD_LOCAL);
    if (!rs.lib) {
        fprintf(stderr, "[t2_hook] ReShade unavailable: t2_fx.so not found (%s)\n", dlerror());
        return 0;
    }
#define RS_SYM(field, name) \
    do { *(void **)&rs.field = dlsym(rs.lib, name); \
         if (!rs.field) { fprintf(stderr, "[t2_hook] ReShade unavailable: no %s in t2_fx.so\n", name); return 0; } } while (0)
    RS_SYM(compile, "t2fx_compile"); RS_SYM(free_effect, "t2fx_free"); RS_SYM(log, "t2fx_log");
    RS_SYM(buffer_size, "t2fx_uniform_buffer_size");
    RS_SYM(texture_count, "t2fx_texture_count"); RS_SYM(texture_get, "t2fx_texture_get");
    RS_SYM(sampler_count, "t2fx_sampler_count"); RS_SYM(sampler_get, "t2fx_sampler_get");
    RS_SYM(uniform_count, "t2fx_uniform_count"); RS_SYM(uniform_get, "t2fx_uniform_get");
    RS_SYM(technique_count, "t2fx_technique_count"); RS_SYM(technique_name, "t2fx_technique_name");
    RS_SYM(pass_count, "t2fx_pass_count"); RS_SYM(pass_get, "t2fx_pass_get");
    RS_SYM(shader_source, "t2fx_shader_source");
    RS_SYM(load_image, "t2fx_load_image"); RS_SYM(free_image, "t2fx_free_image");
#undef RS_SYM
    return 1;
}

/* ── GL objects ────────────────────────────────────────────────────── */

static int rs_internal_format(int format, unsigned *base, unsigned *type) {
    *base = RS_RGBA; *type = RS_UNSIGNED_BYTE;
    switch (format) {
    case 61: *base = 0x1903; return 0x8229;            /* r8 */
    case 56: *base = 0x1903; *type = 0x1403; return 0x822A;   /* r16 */
    case 54: *base = 0x1903; *type = RS_FLOAT; return 0x822D; /* r16f */
    case 41: *base = 0x1903; *type = RS_FLOAT; return 0x822E; /* r32f */
    case 49: *base = 0x8227; return 0x822B;            /* rg8 */
    case 35: *base = 0x8227; *type = 0x1403; return 0x822C;   /* rg16 */
    case 34: *base = 0x8227; *type = RS_FLOAT; return 0x822F; /* rg16f */
    case 16: *base = 0x8227; *type = RS_FLOAT; return 0x8230; /* rg32f */
    case 11: *type = 0x1403; return 0x805B;            /* rgba16 */
    case 10: *type = RS_FLOAT; return 0x881A;          /* rgba16f */
    case 2: *type = RS_FLOAT; return 0x8814;           /* rgba32f */
    case 24: *type = 0x8368; return 0x8059;            /* rgb10a2 */
    case 26: *base = 0x1907; *type = RS_FLOAT; return 0x8C3A; /* rg11b10f */
    default: return RS_RGBA8;
    }
}

static unsigned rs_make_texture(int width, int height, int levels, int format, const void *pixels, int is_image) {
    unsigned id = 0, base, type;
    int internal = is_image ? RS_RGBA8 : rs_internal_format(format, &base, &type);
    if (is_image) { base = RS_RGBA; type = RS_UNSIGNED_BYTE; }
    rs_gl.GenTextures(1, &id);
    rs_gl.BindTexture(RS_TEXTURE_2D, id);
    for (int level = 0, w = width, h = height; level < (levels > 0 ? levels : 1); level++) {
        rs_gl.TexImage2D(RS_TEXTURE_2D, level, internal, w, h, 0, base, type, level == 0 ? pixels : NULL);
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MIN_FILTER, levels > 1 ? RS_LINEAR_MIPMAP_LINEAR : RS_LINEAR);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MAG_FILTER, RS_LINEAR);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_WRAP_S, RS_CLAMP_TO_EDGE);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_WRAP_T, RS_CLAMP_TO_EDGE);
    if (pixels && levels > 1) rs_gl.GenerateMipmap(RS_TEXTURE_2D);
    return id;
}

static unsigned rs_wrap_mode(int mode) {
    switch (mode) {
    case 1: return RS_REPEAT;
    case 2: return RS_MIRRORED_REPEAT;
    case 4: return RS_CLAMP_TO_BORDER;
    default: return RS_CLAMP_TO_EDGE;
    }
}

static unsigned rs_compile_shader(unsigned type, const char *source, const char *label) {
    unsigned shader = rs_gl.CreateShader(type);
    int ok = 0;
    rs_gl.ShaderSource(shader, 1, &source, NULL);
    rs_gl.CompileShader(shader);
    rs_gl.GetShaderiv(shader, RS_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        rs_gl.GetShaderInfoLog(shader, sizeof log - 1, NULL, log);
        fprintf(stderr, "[t2_hook] ReShade: %s shader failed: %s\n", label, log);
        rs_gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

static unsigned rs_make_program(struct rs_effect *effect, const t2fx_pass *pass) {
    const char *vertex = rs.shader_source(effect->fx, pass->vertex_entry);
    const char *pixel = rs.shader_source(effect->fx, pass->pixel_entry);
    if (!vertex || !pixel) {
        fprintf(stderr, "[t2_hook] ReShade: %s: no shader code for pass %s: %s\n", effect->file, pass->name,
                rs.log(effect->fx));
        return 0;
    }
    unsigned vs = rs_compile_shader(RS_VERTEX_SHADER, vertex, effect->file);
    unsigned fs = vs ? rs_compile_shader(RS_FRAGMENT_SHADER, pixel, effect->file) : 0;
    if (!vs || !fs) { if (vs) rs_gl.DeleteShader(vs); return 0; }
    unsigned program = rs_gl.CreateProgram();
    rs_gl.AttachShader(program, vs);
    rs_gl.AttachShader(program, fs);
    rs_gl.LinkProgram(program);
    rs_gl.DeleteShader(vs);
    rs_gl.DeleteShader(fs);
    int linked = 0;
    rs_gl.GetProgramiv(program, RS_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = "";
        rs_gl.GetProgramInfoLog(program, sizeof log - 1, NULL, log);
        fprintf(stderr, "[t2_hook] ReShade: %s: link failed: %s\n", effect->file, log);
        rs_gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

/* ── Uniform values ────────────────────────────────────────────────── */

static void rs_store_uniform(struct rs_effect *effect, const struct rs_uniform_value *uniform, const float *values,
                             const int *ints) {
    unsigned components = (unsigned)(uniform->info.rows * uniform->info.cols);
    if (components > 16) components = 16;
    if (uniform->info.cols > 1) return;  /* matrices keep their compiled-in initialiser */
    for (unsigned i = 0; i < components; i++) {
        unsigned offset = uniform->info.offset + i * 4;
        if (offset + 4 > effect->ubo_size) break;
        if (uniform->info.base == 3) memcpy(effect->ubo_data + offset, &values[i], 4);
        else memcpy(effect->ubo_data + offset, &ints[i], 4);
    }
}

static void rs_set_uniform_text(struct rs_effect *effect, struct rs_uniform_value *uniform, const char *text) {
    float values[16] = {0};
    int ints[16] = {0};
    int count = 0;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", text);
    for (char *token = strtok(copy, ","); token && count < 16; token = strtok(NULL, ","), count++) {
        token = rs_trim(token);
        if (uniform->info.base == 3) values[count] = (float)atof(token);
        else if (uniform->info.base == 0) ints[count] = (rs_text_equal(token, "true") || atoi(token) != 0) ? 1 : 0;
        else ints[count] = (int)strtol(token, NULL, 10);
    }
    rs_store_uniform(effect, uniform, values, ints);
}

/* Per-frame values for uniforms with a "source" annotation. */
static void rs_update_builtin(struct rs_effect *effect, struct rs_uniform_value *uniform, double now) {
    const char *source = uniform->source;
    float values[16] = {0};
    int ints[16] = {0};
    if (!strcmp(source, "frametime")) values[0] = (float)rs.frame_time_ms;
    else if (!strcmp(source, "framecount")) ints[0] = (int)rs.frame;
    else if (!strcmp(source, "timer")) { double ms = (now - rs.start_time) * 1000.0; values[0] = (float)ms; ints[0] = (int)(unsigned)ms; }
    else if (!strcmp(source, "pingpong")) {
        float range = uniform->maximum - uniform->minimum;
        if (range <= 0.0f) range = 1.0f;
        double period = 2.0 * (double)range, elapsed = now - rs.start_time;
        double position = elapsed - (double)(long)(elapsed / period) * period;
        values[0] = uniform->minimum + (position < range ? (float)position : 2.0f * range - (float)position);
        values[1] = position < range ? 1.0f : -1.0f;
    } else if (!strcmp(source, "random")) {
        float range = uniform->maximum - uniform->minimum;
        float unit = (float)(rand() & 0xFFFF) / 65535.0f;
        values[0] = uniform->minimum + unit * range;
        ints[0] = (int)uniform->minimum + (rand() % ((int)range + 1));
    } else if (!strcmp(source, "date")) {
        time_t t = time(NULL);
        struct tm local;
        localtime_r(&t, &local);
        values[0] = (float)(local.tm_year + 1900); values[1] = (float)(local.tm_mon + 1); values[2] = (float)local.tm_mday;
        values[3] = (float)(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    } else {
        return;  /* mouse, key, overlay...: left at zero */
    }
    rs_store_uniform(effect, uniform, values, ints);
}

/* ── Effect loading ────────────────────────────────────────────────── */

static void rs_free_effect(struct rs_effect *effect) {
    if (effect->fx) {
        for (int i = 0; i < effect->texture_count; i++)
            if (effect->textures[i].kind == 0 && effect->textures[i].id) rs_gl.DeleteTextures(1, &effect->textures[i].id);
        for (int i = 0; i < effect->sampler_count; i++)
            if (effect->samplers[i]) rs_gl.DeleteSamplers(1, &effect->samplers[i]);
        for (int t = 0; t < effect->technique_count; t++)
            for (int p = 0; p < effect->technique_passes[t]; p++) {
                struct rs_pass *pass = effect->passes[effect->technique_offset[t] + p];
                if (pass && pass->program) rs_gl.DeleteProgram(pass->program);
                free(pass);
            }
        if (effect->ubo) rs_gl.DeleteBuffers(1, &effect->ubo);
        rs.free_effect(effect->fx);
    }
    free(effect->textures); free(effect->samplers); free(effect->sampler_info);
    free(effect->uniforms); free(effect->passes); free(effect->technique_offset);
    free(effect->technique_passes); free(effect->ubo_data);
    if (effect->sampler_annotations) {
        for (int i = 0; i < effect->sampler_count; i++) free(effect->sampler_annotations[i]);
        free(effect->sampler_annotations);
    }
    if (effect->technique_names) {
        for (int i = 0; i < effect->technique_count; i++) free(effect->technique_names[i]);
        free(effect->technique_names);
    }
    memset(effect, 0, sizeof *effect);
}

static int rs_load_effect(struct rs_effect *effect, const char *file, const char *path) {
    snprintf(effect->file, sizeof effect->file, "%s", file);
    snprintf(effect->path, sizeof effect->path, "%s", path);
    const char *definitions[2 * RS_MAX_DEFINES + 2];
    int definition_count = 0;
    /* The engine's depth buffer is an ordinary (not reversed) perspective depth. The linearisation
       far plane is far / near of the scene's frustum, so DepthBuffer reads like ReShade expects. */
    char far_define[80];
    double ratio = rs.frustum_near > 0.0 && rs.frustum_far > rs.frustum_near ? rs.frustum_far / rs.frustum_near : 1000.0;
    snprintf(far_define, sizeof far_define, "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=%.1f", ratio);
    rs.depth_ratio_used = ratio;
    definitions[definition_count++] = "RESHADE_DEPTH_INPUT_IS_REVERSED=0";
    definitions[definition_count++] = far_define;
    for (int i = 0; i < rs.global_define_count && definition_count < 2 * RS_MAX_DEFINES; i++)
        definitions[definition_count++] = rs.global_defines[i];
    for (int i = 0; i < effect->define_count && definition_count < 2 * RS_MAX_DEFINES; i++)
        definitions[definition_count++] = effect->defines[i];
    char error[4096] = "";
    effect->fx = rs.compile(path, (const char *const *)rs.include_dirs, rs.include_count, definitions,
                            definition_count, rs.width, rs.height, error, sizeof error);
    if (!effect->fx) {
        fprintf(stderr, "[t2_hook] ReShade: %s did not compile: %s\n", file, error);
        return 0;
    }
    const char *log = rs.log(effect->fx);
    if (log && *log) fprintf(stderr, "[t2_hook] ReShade: %s: %s\n", file, log);

    effect->ubo_size = rs.buffer_size(effect->fx);
    if (effect->ubo_size) {
        effect->ubo_data = calloc(1, effect->ubo_size + 16);
        rs_gl.GenBuffers(1, &effect->ubo);
    }
    effect->texture_count = rs.texture_count(effect->fx);
    effect->textures = calloc((size_t)(effect->texture_count + 1), sizeof(struct rs_texture));
    for (int i = 0; i < effect->texture_count; i++) {
        struct rs_texture *texture = &effect->textures[i];
        rs.texture_get(effect->fx, i, &texture->info);
        texture->width = (int)texture->info.width;
        texture->height = (int)texture->info.height;
        if (!strcmp(texture->info.semantic, "COLOR")) { texture->kind = 1; continue; }
        if (!strcmp(texture->info.semantic, "DEPTH")) { texture->kind = 2; rs.need_depth = 1; continue; }
        char image[512];
        if (rs_annotation(texture->info.annotations, "source", image, sizeof image)) {
            const char *base = strrchr(image, '/');
            const char *found = rs_find_file(rs.texture_files, rs.texture_file_count, base ? base + 1 : image);
            int w = 0, h = 0;
            unsigned char *pixels = found ? rs.load_image(found, &w, &h) : NULL;
            if (!pixels) {
                fprintf(stderr, "[t2_hook] ReShade: %s: texture image %s not found or unreadable\n", file, image);
                texture->id = rs_make_texture(texture->width > 0 ? texture->width : 1,
                                              texture->height > 0 ? texture->height : 1, 1, 28, NULL, 0);
            } else {
                texture->width = w; texture->height = h;
                texture->id = rs_make_texture(w, h, (int)texture->info.levels, 28, pixels, 1);
                rs.free_image(pixels);
            }
        } else {
            texture->id = rs_make_texture(texture->width, texture->height, (int)texture->info.levels,
                                          texture->info.format, NULL, 0);
        }
    }
    effect->sampler_count = rs.sampler_count(effect->fx);
    effect->samplers = calloc((size_t)(effect->sampler_count + 1), sizeof(unsigned));
    effect->sampler_info = calloc((size_t)(effect->sampler_count + 1), sizeof(t2fx_sampler));
    effect->sampler_annotations = calloc((size_t)(effect->sampler_count + 1), sizeof(char *));
    for (int i = 0; i < effect->sampler_count; i++) {
        t2fx_sampler *info = &effect->sampler_info[i];
        rs.sampler_get(effect->fx, i, info);
        unsigned id = 0;
        rs_gl.GenSamplers(1, &id);
        effect->samplers[i] = id;
        int minimum_linear = info->filter & 0x10, magnify_linear = info->filter & 0x04, mip_linear = info->filter & 0x01;
        int levels = 1;
        for (int t = 0; t < effect->texture_count; t++)
            if (!strcmp(effect->textures[t].info.unique_name, info->texture_name)) levels = (int)effect->textures[t].info.levels;
        int min_filter = levels > 1
            ? (minimum_linear ? (mip_linear ? RS_LINEAR_MIPMAP_LINEAR : RS_LINEAR_MIPMAP_NEAREST)
                              : (mip_linear ? RS_NEAREST_MIPMAP_LINEAR : RS_NEAREST_MIPMAP_NEAREST))
            : (minimum_linear ? RS_LINEAR : RS_NEAREST);
        rs_gl.SamplerParameteri(id, RS_TEXTURE_MIN_FILTER, min_filter);
        rs_gl.SamplerParameteri(id, RS_TEXTURE_MAG_FILTER, magnify_linear ? RS_LINEAR : RS_NEAREST);
        rs_gl.SamplerParameteri(id, RS_TEXTURE_WRAP_S, (int)rs_wrap_mode(info->address_u));
        rs_gl.SamplerParameteri(id, RS_TEXTURE_WRAP_T, (int)rs_wrap_mode(info->address_v));
        rs_gl.SamplerParameterf(id, RS_TEXTURE_LOD_BIAS, info->lod_bias);
        if (info->min_lod > -1000.0f) rs_gl.SamplerParameterf(id, RS_TEXTURE_MIN_LOD, info->min_lod);
        if (info->max_lod < 1000.0f) rs_gl.SamplerParameterf(id, RS_TEXTURE_MAX_LOD, info->max_lod);
        if (info->srgb) fprintf(stderr, "[t2_hook] ReShade: %s: sRGB sampling of %s is not supported; reading linear\n",
                                file, info->name);
    }
    effect->uniform_count = rs.uniform_count(effect->fx);
    effect->uniforms = calloc((size_t)(effect->uniform_count + 1), sizeof(struct rs_uniform_value));
    for (int i = 0; i < effect->uniform_count; i++) {
        struct rs_uniform_value *uniform = &effect->uniforms[i];
        rs.uniform_get(effect->fx, i, &uniform->info);
        char value[64];
        uniform->minimum = rs_annotation(uniform->info.annotations, "min", value, sizeof value) ? (float)atof(value) : 0.0f;
        uniform->maximum = rs_annotation(uniform->info.annotations, "max", value, sizeof value) ? (float)atof(value) : 1.0f;
        rs_annotation(uniform->info.annotations, "source", uniform->source, sizeof uniform->source);
        if (uniform->info.has_initializer)
            rs_store_uniform(effect, uniform, uniform->info.initializer, uniform->info.initializer_int);
    }
    effect->technique_count = rs.technique_count(effect->fx);
    effect->technique_offset = calloc((size_t)(effect->technique_count + 1), sizeof(int));
    effect->technique_passes = calloc((size_t)(effect->technique_count + 1), sizeof(int));
    effect->technique_names = calloc((size_t)(effect->technique_count + 1), sizeof(char *));
    int total = 0;
    for (int t = 0; t < effect->technique_count; t++) {
        effect->technique_offset[t] = total;
        effect->technique_passes[t] = rs.pass_count(effect->fx, t);
        effect->technique_names[t] = strdup(rs.technique_name(effect->fx, t));
        total += effect->technique_passes[t];
    }
    effect->passes = calloc((size_t)(total + 1), sizeof(struct rs_pass *));
    for (int t = 0; t < effect->technique_count; t++)
        for (int p = 0; p < effect->technique_passes[t]; p++) {
            struct rs_pass *pass = calloc(1, sizeof *pass);
            rs.pass_get(effect->fx, t, p, &pass->info);
            pass->program = rs_make_program(effect, &pass->info);
            effect->passes[effect->technique_offset[t] + p] = pass;
        }
    effect->ok = 1;
    return 1;
}

/* ── Preset ────────────────────────────────────────────────────────── */

static void rs_split_list(char *text, char **items, int *count, int limit) {
    for (char *token = strtok(text, ","); token && *count < limit; token = strtok(NULL, ",")) {
        token = rs_trim(token);
        if (*token) items[(*count)++] = token;
    }
}

static struct rs_effect *rs_effect_for(const char *file, const char *path) {
    for (int i = 0; i < rs.effect_count; i++)
        if (rs_text_equal(rs.effects[i].file, file)) return &rs.effects[i];
    if (rs.effect_count >= RS_MAX_EFFECTS) return NULL;
    struct rs_effect *effect = &rs.effects[rs.effect_count++];
    memset(effect, 0, sizeof *effect);
    snprintf(effect->file, sizeof effect->file, "%s", file);
    snprintf(effect->path, sizeof effect->path, "%s", path);
    return effect;
}

static void rs_unload(void) {
    if (rs_gl.state > 0 && rs.initialised)
        for (int i = 0; i < rs.effect_count; i++) rs_free_effect(&rs.effects[i]);
    rs.effect_count = rs.chain_count = 0;
    rs.need_depth = 0;
}

/* Load the preset: which techniques run, in what order, with which values. */
static int rs_load_preset(void) {
    FILE *file = fopen(rs.preset_path, "r");
    if (!file) {
        fprintf(stderr, "[t2_hook] ReShade: cannot read preset %s\n", rs.preset_path);
        return 0;
    }
    struct stat info;
    if (stat(rs.preset_path, &info) == 0) rs.preset_mtime = info.st_mtime;

    char *techniques = NULL, *sorting = NULL;
    char section[256] = "";
    /* Keep the file's lines so the per-effect sections can be applied after the effects load. */
    char (*lines)[1024] = malloc(sizeof(char[1024]) * 8192);
    int line_count = 0;
    char buffer[1024];
    while (line_count < 8192 && fgets(buffer, sizeof buffer, file)) {
        char *text = rs_trim(buffer);
        if (!*text || *text == ';' || *text == '#') continue;
        snprintf(lines[line_count++], sizeof lines[0], "%s", text);
    }
    fclose(file);

    rs.global_define_count = 0;
    for (int i = 0; i < line_count; i++) {
        char *text = lines[i];
        if (*text == '[') { snprintf(section, sizeof section, "%s", text); continue; }
        if (section[0]) continue;
        char *equals = strchr(text, '=');
        if (!equals) continue;
        *equals = '\0';
        char *key = rs_trim(text), *value = rs_trim(equals + 1);
        if (rs_text_equal(key, "Techniques")) { free(techniques); techniques = strdup(value); }
        else if (rs_text_equal(key, "TechniqueSorting")) { free(sorting); sorting = strdup(value); }
        else if (rs_text_equal(key, "PreprocessorDefinitions")) {
            char copy[1024]; char *items[RS_MAX_DEFINES]; int count = 0;
            snprintf(copy, sizeof copy, "%s", value);
            rs_split_list(copy, items, &count, RS_MAX_DEFINES);
            for (int j = 0; j < count && rs.global_define_count < RS_MAX_DEFINES; j++)
                snprintf(rs.global_defines[rs.global_define_count++], 160, "%s", items[j]);
        }
    }
    /* Order: TechniqueSorting when present, restricted to the enabled Techniques. */
    char *enabled[RS_MAX_TECHNIQUES]; int enabled_count = 0;
    char *enabled_copy = techniques ? strdup(techniques) : NULL;
    if (enabled_copy) rs_split_list(enabled_copy, enabled, &enabled_count, RS_MAX_TECHNIQUES);
    char *ordered[RS_MAX_TECHNIQUES]; int ordered_count = 0;
    char *sorting_copy = sorting ? strdup(sorting) : NULL;
    if (sorting_copy) {
        char *all[RS_MAX_TECHNIQUES * 2]; int all_count = 0;
        rs_split_list(sorting_copy, all, &all_count, RS_MAX_TECHNIQUES * 2);
        for (int i = 0; i < all_count && ordered_count < RS_MAX_TECHNIQUES; i++)
            for (int j = 0; j < enabled_count; j++)
                if (!strcmp(all[i], enabled[j])) { ordered[ordered_count++] = enabled[j]; break; }
    }
    for (int j = 0; j < enabled_count && ordered_count < RS_MAX_TECHNIQUES; j++) {
        int present = 0;
        for (int i = 0; i < ordered_count; i++) if (!strcmp(ordered[i], enabled[j])) present = 1;
        if (!present) ordered[ordered_count++] = enabled[j];
    }

    /* Load each effect named in the list and build the chain. */
    for (int i = 0; i < ordered_count; i++) {
        char *at = strchr(ordered[i], '@');
        if (!at) continue;
        *at = '\0';
        const char *name = ordered[i], *file_name = at + 1;
        const char *path = rs_find_file(rs.shader_files, rs.shader_file_count, file_name);
        if (!path) { fprintf(stderr, "[t2_hook] ReShade: effect %s (technique %s) not found in %s\n", file_name, name, rs.shaders_dir); continue; }
        struct rs_effect *effect = rs_effect_for(file_name, path);
        if (!effect) continue;
        if (!effect->fx && !effect->ok) {
            /* per-effect preprocessor definitions from its section */
            char header[300];
            snprintf(header, sizeof header, "[%s]", file_name);
            int inside = 0;
            for (int l = 0; l < line_count; l++) {
                if (lines[l][0] == '[') { inside = rs_text_equal(lines[l], header); continue; }
                if (!inside) continue;
                if (!strncasecmp(lines[l], "PreprocessorDefinitions=", 24)) {
                    char copy[1024]; char *items[RS_MAX_DEFINES]; int count = 0;
                    snprintf(copy, sizeof copy, "%s", lines[l] + 24);
                    rs_split_list(copy, items, &count, RS_MAX_DEFINES);
                    for (int j = 0; j < count && effect->define_count < RS_MAX_DEFINES; j++)
                        snprintf(effect->defines[effect->define_count++], 160, "%s", items[j]);
                }
            }
            if (!rs_load_effect(effect, file_name, path)) { effect->ok = -1; continue; }
            /* apply the saved values */
            inside = 0;
            for (int l = 0; l < line_count; l++) {
                if (lines[l][0] == '[') { inside = rs_text_equal(lines[l], header); continue; }
                if (!inside) continue;
                char text[1024];
                snprintf(text, sizeof text, "%s", lines[l]);
                char *equals = strchr(text, '=');
                if (!equals) continue;
                *equals = '\0';
                char *key = rs_trim(text), *value = rs_trim(equals + 1);
                for (int u = 0; u < effect->uniform_count; u++)
                    if (!strcmp(effect->uniforms[u].info.name, key)) rs_set_uniform_text(effect, &effect->uniforms[u], value);
            }
        }
        if (effect->ok <= 0) continue;
        for (int t = 0; t < effect->technique_count; t++)
            if (!strcmp(effect->technique_names[t], name) && rs.chain_count < RS_MAX_TECHNIQUES) {
                rs.chain[rs.chain_count].effect = effect;
                rs.chain[rs.chain_count].index = t;
                rs.chain_count++;
            }
    }
    free(techniques); free(sorting); free(enabled_copy); free(sorting_copy); free(lines);
    fprintf(stderr, "[t2_hook] ReShade: preset %s: %d technique(s) in the chain\n", rs.preset_path, rs.chain_count);
    return 1;
}

static void rs_build_paths(void) {
    const char *shaders = hook_option_str("video.reshade_shaders");
    char fallback[1024];
    if (!shaders || !*shaders) {
        const char *home = getenv("HOME");
        snprintf(fallback, sizeof fallback, "%s/.local/share/t2launch/reshade", home ? home : ".");
        shaders = fallback;
    }
    snprintf(rs.shaders_dir, sizeof rs.shaders_dir, "%s", shaders);
    const char *preset = hook_option_str("video.reshade_preset");
    if (!rs.preset_path[0] && preset && *preset) {  /* kept once set, so a preset picked with the keys sticks */
        if (strchr(preset, '/')) snprintf(rs.preset_path, sizeof rs.preset_path, "%s", preset);
        else snprintf(rs.preset_path, sizeof rs.preset_path, "%s/Presets/%s", rs.shaders_dir, preset);
    }
    for (int i = 0; i < rs.shader_file_count; i++) { free(rs.shader_files[i].name); free(rs.shader_files[i].path); }
    for (int i = 0; i < rs.texture_file_count; i++) { free(rs.texture_files[i].name); free(rs.texture_files[i].path); }
    for (int i = 0; i < rs.include_count; i++) free(rs.include_dirs[i]);
    rs.shader_file_count = rs.texture_file_count = 0;
    char path[1280];
    snprintf(path, sizeof path, "%s/Shaders", rs.shaders_dir);
    rs_scan(path, rs.shader_files, &rs.shader_file_count, 0, 0);
    snprintf(path, sizeof path, "%s/Textures", rs.shaders_dir);
    rs_scan(path, rs.texture_files, &rs.texture_file_count, 0, 0);
    rs.include_count = 0;
    snprintf(path, sizeof path, "%s/Shaders", rs.shaders_dir);
    rs_collect_dirs(path, &rs.include_dirs, &rs.include_count, 0);
}

/* Compile everything for a buffer of width x height. */
static int rs_setup(int width, int height) {
    rs.width = width; rs.height = height;
    if (!rs.initialised) {
        rs.start_time = rs.last_time = rs_now();
        srand((unsigned)time(NULL));
    }
    rs.initialised = 1;
    rs_unload();
    rs_build_paths();  /* every load: files may have been added since the last one */
    if (!rs.preset_path[0]) {
        fprintf(stderr, "[t2_hook] ReShade: no preset set (video.reshade_preset)\n");
        return 0;
    }
    if (!rs_load_preset()) return 0;
    if (!rs.buffer[0]) {
        rs_gl.GenTextures(2, rs.buffer);
        rs_gl.GenTextures(1, &rs.output);
        rs_gl.GenFramebuffers(1, &rs.fbo);
        rs_gl.GenFramebuffers(1, &rs.read_fbo);
        unsigned white = 0xFFFFFFFFu;
        rs.white = rs_make_texture(1, 1, 1, 28, &white, 1);
    }
    rs_make_depth_resources(width, height);
    unsigned targets[3] = {rs.buffer[0], rs.buffer[1], rs.output};
    for (int i = 0; i < 3; i++) {
        rs_gl.BindTexture(RS_TEXTURE_2D, targets[i]);
        rs_gl.TexImage2D(RS_TEXTURE_2D, 0, RS_RGBA8, width, height, 0, RS_RGBA, RS_UNSIGNED_BYTE, NULL);
        rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MIN_FILTER, RS_LINEAR);
        rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MAG_FILTER, RS_LINEAR);
        rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_WRAP_S, RS_CLAMP_TO_EDGE);
        rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_WRAP_T, RS_CLAMP_TO_EDGE);
    }
    return 1;
}

/* ── Rendering ─────────────────────────────────────────────────────── */

/* ── Depth ─────────────────────────────────────────────────────────── */

static void (*rs_real_frustum)(double, double, double, double, double, double);

/* The scene is the glFrustum call with the farthest far plane in a frame (the HUD only uses glOrtho). */
static void rs_frustum_wrapper(double left, double right, double bottom, double top, double near_plane, double far_plane) {
    if (near_plane > 0.0 && far_plane > rs.frame_far) { rs.frame_near = near_plane; rs.frame_far = far_plane; }
    if (rs_real_frustum) rs_real_frustum(left, right, bottom, top, near_plane, far_plane);
}

/* Defined by the hook: the framebuffer to read the scene's depth from (0 = the window). With a
   multisampled render target it resolves the depth first. */
static unsigned rt_depth_read_source(void);

static void (*rs_real_clear)(unsigned);

/* The game clears the depth buffer once per frame right after the 3D scene (the HUD is drawn
   without it), so that is where the scene's depth still exists: copy it just before. */
static void rs_clear_wrapper(unsigned mask) {
    if ((mask & 0x100) && rs.need_depth && rs.depth_copy && rs.frame_far > 0.0 && !rs.depth_taken &&
        rs_gl.state > 0) {
        int old_read = 0, old_draw = 0, old_texture = 0;
        rs_gl.GetIntegerv(RS_READ_FRAMEBUFFER_BINDING, &old_read);
        rs_gl.GetIntegerv(RS_DRAW_FRAMEBUFFER_BINDING, &old_draw);
        rs_gl.GetIntegerv(0x8069, &old_texture);
        unsigned source = rt_depth_read_source();
        rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, source);
        rs_gl.BindTexture(RS_TEXTURE_2D, rs.depth_copy);
        rs_gl.CopyTexSubImage2D(RS_TEXTURE_2D, 0, 0, 0, 0, 0, rs.width, rs.height);
        rs_gl.BindTexture(RS_TEXTURE_2D, (unsigned)old_texture);
        rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, (unsigned)old_read);
        rs_gl.BindFramebuffer(RS_DRAW_FRAMEBUFFER, (unsigned)old_draw);
        rs.depth_taken = 1;
    }
    if (rs_real_clear) rs_real_clear(mask);
}

/* A wrapper for a GL entry point the game resolves through SDL_GL_GetProcAddress, or NULL. */
static void *reshade_gl_proc(const char *proc, void *address) {
    if (!address || !hook_option_bool("video.reshade")) return NULL;
    if (!strcmp(proc, "glFrustum")) {
        rs_real_frustum = (void (*)(double, double, double, double, double, double))address;
        return (void *)rs_frustum_wrapper;
    }
    if (!strcmp(proc, "glClear")) {
        rs_real_clear = (void (*)(unsigned))address;
        return (void *)rs_clear_wrapper;
    }
    return NULL;
}

static const char rs_depth_vertex[] =
    "#version 330 compatibility\n"
    "out vec2 uv;\n"
    "void main() { vec2 p = vec2((gl_VertexID == 2) ? 2.0 : 0.0, (gl_VertexID == 1) ? 2.0 : 0.0);\n"
    "  uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char rs_depth_fragment[] =
    "#version 330 compatibility\n"
    "uniform sampler2D source; in vec2 uv; layout(location = 0) out float result;\n"
    "void main() { result = texture(source, vec2(uv.x, 1.0 - uv.y)).r; }\n";

static void rs_make_depth_resources(int width, int height) {
    if (!rs.depth_copy) {
        rs_gl.GenTextures(1, &rs.depth_copy);
        rs_gl.GenTextures(1, &rs.depth_flipped);
        unsigned vs = rs_compile_shader(RS_VERTEX_SHADER, rs_depth_vertex, "depth");
        unsigned fs = vs ? rs_compile_shader(RS_FRAGMENT_SHADER, rs_depth_fragment, "depth") : 0;
        if (vs && fs) {
            rs.depth_program = rs_gl.CreateProgram();
            rs_gl.AttachShader(rs.depth_program, vs);
            rs_gl.AttachShader(rs.depth_program, fs);
            rs_gl.LinkProgram(rs.depth_program);
            int linked = 0;
            rs_gl.GetProgramiv(rs.depth_program, RS_LINK_STATUS, &linked);
            if (!linked) {
                char log[1024] = "";
                rs_gl.GetProgramInfoLog(rs.depth_program, sizeof log - 1, NULL, log);
                fprintf(stderr, "[t2_hook] ReShade: depth program did not link: %s\n", log);
                rs_gl.DeleteProgram(rs.depth_program);
                rs.depth_program = 0;
            }
        }
        if (vs) rs_gl.DeleteShader(vs);
        if (fs) rs_gl.DeleteShader(fs);
    }
    rs_gl.BindTexture(RS_TEXTURE_2D, rs.depth_copy);
    rs_gl.TexImage2D(RS_TEXTURE_2D, 0, 0x81A6, width, height, 0, 0x1902, 0x1405, NULL);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MIN_FILTER, RS_NEAREST);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MAG_FILTER, RS_NEAREST);
    rs_gl.BindTexture(RS_TEXTURE_2D, rs.depth_flipped);
    rs_gl.TexImage2D(RS_TEXTURE_2D, 0, 0x822E, width, height, 0, 0x1903, RS_FLOAT, NULL);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MIN_FILTER, RS_NEAREST);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_MAG_FILTER, RS_NEAREST);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_WRAP_S, RS_CLAMP_TO_EDGE);
    rs_gl.TexParameteri(RS_TEXTURE_2D, RS_TEXTURE_WRAP_T, RS_CLAMP_TO_EDGE);
}

/* Turn the depth copied at the clear into depth_flipped, top row first. */
static void rs_capture_depth(int width, int height) {
    if (!rs.depth_program || !rs.depth_taken) return;
    rs_gl.BindFramebuffer(RS_FRAMEBUFFER, rs.fbo);
    rs_gl.FramebufferTexture2D(RS_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, rs.depth_flipped, 0);
    unsigned draw = RS_COLOR_ATTACHMENT0;
    rs_gl.DrawBuffers(1, &draw);
    rs_gl.Viewport(0, 0, width, height);
    rs_gl.Disable(RS_BLEND);
    rs_gl.UseProgram(rs.depth_program);
    rs_gl.ActiveTexture(RS_TEXTURE0);
    rs_gl.BindTexture(RS_TEXTURE_2D, rs.depth_copy);
    rs_gl.Uniform1i(rs_gl.GetUniformLocation(rs.depth_program, "source"), 0);
    rs_gl.DrawArrays(4, 0, 3);
}

static unsigned rs_blend_factor(int factor) {
    static const unsigned table[] = {0, 1, 0x0300, 0x0301, 0x0306, 0x0307, 0x0302, 0x0303, 0x0304, 0x0305};
    return factor >= 0 && factor < 10 ? table[factor] : 1;
}

static unsigned rs_blend_op(int op) {
    static const unsigned table[] = {0x8006, 0x8006, 0x800A, 0x800B, 0x8007, 0x8008};
    return op >= 1 && op <= 5 ? table[op] : 0x8006;
}

static unsigned rs_topology(int topology) {
    static const unsigned table[] = {4, 0, 1, 3, 4, 5};
    return topology >= 1 && topology <= 5 ? table[topology] : 4;
}

static unsigned rs_texture_for(struct rs_effect *effect, const char *unique_name, unsigned current) {
    for (int i = 0; i < effect->texture_count; i++) {
        struct rs_texture *texture = &effect->textures[i];
        if (strcmp(texture->info.unique_name, unique_name)) continue;
        if (texture->kind == 1) return current;
        if (texture->kind == 2) return rs.depth_flipped ? rs.depth_flipped : rs.white;
        return texture->id;
    }
    return rs.white;
}

/* Copy src into dst flipped vertically (both RGBA8 w x h). */
static void rs_flip_copy(unsigned src, unsigned dst, int w, int h) {
    rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, rs.read_fbo);
    rs_gl.FramebufferTexture2D(RS_READ_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, src, 0);
    rs_gl.BindFramebuffer(RS_DRAW_FRAMEBUFFER, rs.fbo);
    rs_gl.FramebufferTexture2D(RS_DRAW_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, dst, 0);
    rs_gl.BlitFramebuffer(0, 0, w, h, 0, h, w, 0, RS_COLOR_BUFFER_BIT, RS_NEAREST);
}

static void rs_plain_copy(unsigned src, unsigned dst, int w, int h) {
    rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, rs.read_fbo);
    rs_gl.FramebufferTexture2D(RS_READ_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, src, 0);
    rs_gl.BindFramebuffer(RS_DRAW_FRAMEBUFFER, rs.fbo);
    rs_gl.FramebufferTexture2D(RS_DRAW_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, dst, 0);
    rs_gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, RS_COLOR_BUFFER_BIT, RS_NEAREST);
}

static int rs_run_pass(struct rs_effect *effect, struct rs_pass *pass, unsigned *current, int *spare) {
    const t2fx_pass *info = &pass->info;
    if (!pass->program) return 0;
    unsigned attachments[T2FX_MAX_TARGETS];
    int target_count = 0, target_w = rs.width, target_h = rs.height;
    int to_backbuffer = info->render_target[0][0] == '\0';
    unsigned destination = rs.buffer[*spare];
    if (to_backbuffer) {
        if (info->blend_enable) rs_plain_copy(*current, destination, rs.width, rs.height);
        attachments[target_count++] = destination;
    } else {
        for (int i = 0; i < T2FX_MAX_TARGETS && info->render_target[i][0]; i++)
            for (int t = 0; t < effect->texture_count; t++)
                if (!strcmp(effect->textures[t].info.unique_name, info->render_target[i])) {
                    attachments[target_count++] = effect->textures[t].id;
                    if (i == 0) { target_w = effect->textures[t].width; target_h = effect->textures[t].height; }
                }
    }
    if (!target_count) return 0;
    rs_gl.BindFramebuffer(RS_FRAMEBUFFER, rs.fbo);
    unsigned draw_buffers[T2FX_MAX_TARGETS];
    for (int i = 0; i < target_count; i++) {
        rs_gl.FramebufferTexture2D(RS_FRAMEBUFFER, RS_COLOR_ATTACHMENT0 + (unsigned)i, RS_TEXTURE_2D, attachments[i], 0);
        draw_buffers[i] = RS_COLOR_ATTACHMENT0 + (unsigned)i;
    }
    rs_gl.DrawBuffers(target_count, draw_buffers);
    if (rs_gl.CheckFramebufferStatus(RS_FRAMEBUFFER) != RS_FRAMEBUFFER_COMPLETE) return 0;
    rs_gl.Viewport(0, 0, info->viewport_width ? (int)info->viewport_width : target_w,
                   info->viewport_height ? (int)info->viewport_height : target_h);
    if (info->clear) { rs_gl.ClearColor(0, 0, 0, 0); rs_gl.Clear(RS_COLOR_BUFFER_BIT); }
    if (info->blend_enable) {
        rs_gl.Enable(RS_BLEND);
        rs_gl.BlendFuncSeparate(rs_blend_factor(info->blend_src), rs_blend_factor(info->blend_dst),
                                rs_blend_factor(info->blend_alpha_src), rs_blend_factor(info->blend_alpha_dst));
        rs_gl.BlendEquationSeparate(rs_blend_op(info->blend_op), rs_blend_op(info->blend_alpha_op));
    } else {
        rs_gl.Disable(RS_BLEND);
    }
    if (info->srgb_write) rs_gl.Enable(RS_FRAMEBUFFER_SRGB); else rs_gl.Disable(RS_FRAMEBUFFER_SRGB);
    unsigned mask = (unsigned)info->write_mask;
    rs_gl.ColorMask(mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
    rs_gl.UseProgram(pass->program);
    for (int i = 0; i < info->sampler_count; i++) {
        int sampler = info->sampler_index[i], binding = info->sampler_binding[i];
        rs_gl.ActiveTexture(RS_TEXTURE0 + (unsigned)binding);
        rs_gl.BindTexture(RS_TEXTURE_2D, rs_texture_for(effect, effect->sampler_info[sampler].texture_name, *current));
        rs_gl.BindSampler((unsigned)binding, effect->samplers[sampler]);
    }
    rs_gl.DrawArrays(rs_topology(info->topology), 0, (int)info->num_vertices);
    for (int i = 0; i < info->sampler_count; i++) rs_gl.BindSampler((unsigned)info->sampler_binding[i], 0);
    rs_gl.ColorMask(1, 1, 1, 1);
    if (info->generate_mipmaps && !to_backbuffer)
        for (int i = 0; i < target_count; i++)
            for (int t = 0; t < effect->texture_count; t++)
                if (effect->textures[t].id == attachments[i] && effect->textures[t].info.levels > 1) {
                    rs_gl.BindTexture(RS_TEXTURE_2D, attachments[i]);
                    rs_gl.GenerateMipmap(RS_TEXTURE_2D);
                }
    if (to_backbuffer) { *current = destination; *spare ^= 1; }
    return 1;
}

/* Run the chain on src_texture (the game's w x h frame, GL orientation).
   Returns the texture holding the result in GL orientation, or 0 when there is
   nothing to draw. */
static unsigned reshade_run(unsigned src_texture, int width, int height) {
    if (!rs.enabled || rs.failed || rs.paused || width <= 0 || height <= 0) return 0;
    if (!rs_resolve_gl() || !rs_load_library()) { rs.failed = 1; return 0; }
    double now = rs_now();
    rs.frame_time_ms = (now - rs.last_time) * 1000.0;
    rs.last_time = now;
    int frustum_changed = 0;
    if (rs.frame_far > 0.0) {
        frustum_changed = rs.frustum_far != rs.frame_far || rs.frustum_near != rs.frame_near;
        rs.frustum_near = rs.frame_near; rs.frustum_far = rs.frame_far;
        rs.frame_near = rs.frame_far = 0.0;
        if (frustum_changed) fprintf(stderr, "[t2_hook] ReShade: scene frustum near %.3f far %.1f\n", rs.frustum_near, rs.frustum_far);
    }
    double ratio = rs.frustum_near > 0.0 ? rs.frustum_far / rs.frustum_near : 0.0;
    int depth_stale = rs.need_depth && frustum_changed && ratio > 0.0 &&
                      (ratio > rs.depth_ratio_used * 1.05 || ratio < rs.depth_ratio_used / 1.05);
    if (!rs.initialised || width != rs.width || height != rs.height || depth_stale || rs.reload_now) {
        rs.reload_now = 0;
        if (!rs_setup(width, height)) { if (!rs.chain_count) { rs.reload_check = 0; } }
    } else if (++rs.reload_check >= 60) {
        rs.reload_check = 0;
        struct stat info;
        if (rs.preset_path[0] && stat(rs.preset_path, &info) == 0 && info.st_mtime != rs.preset_mtime) {
            fprintf(stderr, "[t2_hook] ReShade: preset changed, reloading\n");
            rs_setup(width, height);
        }
    }
    if (!rs.chain_count) return 0;

    int previous_draw = 0, previous_read = 0, previous_unit = 0;
    rs_gl.GetIntegerv(RS_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    rs_gl.GetIntegerv(RS_READ_FRAMEBUFFER_BINDING, &previous_read);
    rs_gl.GetIntegerv(RS_ACTIVE_TEXTURE, &previous_unit);

    if (rs.need_depth) { rs_capture_depth(width, height); rs.depth_taken = 0; }
    rs_flip_copy(src_texture, rs.buffer[0], width, height);
    unsigned current = rs.buffer[0];
    int spare = 1;
    for (int i = 0; i < rs.chain_count; i++) {
        struct rs_effect *effect = rs.chain[i].effect;
        int technique = rs.chain[i].index;
        if (effect->ubo) {
            for (int u = 0; u < effect->uniform_count; u++)
                if (effect->uniforms[u].source[0]) rs_update_builtin(effect, &effect->uniforms[u], now);
            rs_gl.BindBuffer(RS_UNIFORM_BUFFER, effect->ubo);
            rs_gl.BufferData(RS_UNIFORM_BUFFER, (long)effect->ubo_size, effect->ubo_data, RS_DYNAMIC_DRAW);
            rs_gl.BindBufferBase(RS_UNIFORM_BUFFER, 0, effect->ubo);
        }
        for (int p = 0; p < effect->technique_passes[technique]; p++)
            rs_run_pass(effect, effect->passes[effect->technique_offset[technique] + p], &current, &spare);
    }
    rs_gl.BindBuffer(RS_UNIFORM_BUFFER, 0);
    rs_gl.BindBufferBase(RS_UNIFORM_BUFFER, 0, 0);
    rs_gl.Disable(RS_BLEND);
    rs_gl.Disable(RS_FRAMEBUFFER_SRGB);
    rs_flip_copy(current, rs.output, width, height);
    rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, (unsigned)previous_read);
    rs_gl.BindFramebuffer(RS_DRAW_FRAMEBUFFER, (unsigned)previous_draw);
    rs_gl.ActiveTexture((unsigned)previous_unit);
    rs.frame++;
    return rs.output;
}

/* A new GL context: every id from the old one is gone. */
static void reshade_context_lost(void) {
    rs.initialised = 0;
    rs.effect_count = rs.chain_count = 0;
    memset(rs.buffer, 0, sizeof rs.buffer);
    rs.depth_copy = rs.depth_flipped = rs.depth_program = 0;
    rs.output = rs.fbo = rs.read_fbo = rs.white = 0;
    memset(rs.effects, 0, sizeof rs.effects);
    if (rs_gl.state > 0) rs_gl.state = 0;
}

static int reshade_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) {
        wanted = hook_option_bool("video.reshade");
        rs.enabled = wanted;
    }
    return wanted;
}

/* ── Keys ──────────────────────────────────────────────────────────── */

/* Scroll Lock toggles the effects; Ctrl+Scroll Lock loads the next preset from <shaders>/Presets and
   Shift+Scroll Lock the previous one. */
#define RS_KEY_TOGGLE 302   /* SDLK_SCROLLOCK */

static int rs_compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void rs_notify(const char *message) {
    fprintf(stderr, "[t2_hook] ReShade: %s\n", message);
    char script[256];
    if (Con__evaluate && snprintf(script, sizeof script, "echo(\"ReShade: %s\");", message) < (int)sizeof script)
        (void)Con__evaluate(script, 0, "t2launch/reshade.cs", 0);
}

static void rs_cycle_preset(int step) {
    char directory[1100];
    snprintf(directory, sizeof directory, "%s/Presets", rs.shaders_dir);
    DIR *handle = opendir(directory);
    if (!handle) { rs_notify("no Presets folder to cycle through"); return; }
    char *names[256]; int count = 0;
    struct dirent *entry;
    while ((entry = readdir(handle)) && count < 256) {
        size_t length = strlen(entry->d_name);
        if (entry->d_name[0] != '.' && length > 4 && !strcasecmp(entry->d_name + length - 4, ".ini"))
            names[count++] = strdup(entry->d_name);
    }
    closedir(handle);
    if (!count) { rs_notify("no presets found"); return; }
    qsort(names, (size_t)count, sizeof names[0], rs_compare_names);
    const char *current = strrchr(rs.preset_path, '/');
    current = current ? current + 1 : rs.preset_path;
    int index = -1;
    for (int i = 0; i < count; i++) if (!strcmp(names[i], current)) index = i;
    index = (index + step + count) % count;
    snprintf(rs.preset_path, sizeof rs.preset_path, "%s/%s", directory, names[index]);
    rs.reload_now = 1;
    rs.paused = 0;
    char message[300];
    snprintf(message, sizeof message, "preset %s", names[index]);
    rs_notify(message);
    for (int i = 0; i < count; i++) free(names[i]);
}

/* An SDL 1.2 key event as the game's SDL lays it out: type, which, state at 0..2, then the 32-bit
   scancode at 4, sym at 8 and mod at 12 (read from the live events; not the header's packed guess). */
static void reshade_key_event(const unsigned char *event) {
    if (event[0] != 2 || !rs.enabled || rs.failed) return;  /* SDL_KEYDOWN */
    unsigned sym, mod;
    memcpy(&sym, event + 8, sizeof sym);
    memcpy(&mod, event + 12, sizeof mod);
    if (sym != RS_KEY_TOGGLE) return;
    if (mod & 0x00C0) rs_cycle_preset(1);          /* KMOD_CTRL */
    else if (mod & 0x0003) rs_cycle_preset(-1);    /* KMOD_SHIFT */
    else {
        rs.paused = !rs.paused;
        rs_notify(rs.paused ? "effects off" : "effects on");
    }
}
