/* C interface of t2_fx.so, the ReShade FX (.fx) compiler used by the hook.
 *
 * t2_fx.so wraps the BSD-licensed reshadefx front end (third_party/reshadefx)
 * and generates GLSL (OpenGL semantics). The hook loads it with dlopen so the C
 * hook has no C++ runtime dependency. Strings returned by the accessors stay
 * valid until t2fx_free. Annotation lists are "name=value" lines joined by '\n'.
 */
#ifndef T2_FX_H
#define T2_FX_H

#ifdef __cplusplus
extern "C" {
#endif

#define T2FX_VERSION 1
#define T2FX_MAX_TARGETS 8

typedef struct t2fx_effect t2fx_effect;

typedef struct {
    const char *name;
    const char *unique_name;
    const char *semantic;       /* "COLOR"/"DEPTH" for built-in textures, else "" */
    const char *annotations;
    unsigned width, height, depth, levels;
    int dimensions;             /* 1, 2 or 3 */
    int format;                 /* reshadefx texture_format value (28 = RGBA8, 10 = RGBA16F, ...) */
    int render_target;
} t2fx_texture;

typedef struct {
    const char *name;
    const char *unique_name;
    const char *texture_name;   /* unique_name of the texture this sampler reads */
    const char *annotations;
    int filter;                 /* reshadefx filter_mode */
    int address_u, address_v, address_w;  /* 1 wrap, 2 mirror, 3 clamp, 4 border */
    float min_lod, max_lod, lod_bias;
    int srgb;
} t2fx_sampler;

typedef struct {
    const char *name;
    const char *unique_name;
    const char *annotations;    /* ui_type, ui_min, source, ... */
    int base;                   /* 0 bool, 1 int, 2 uint, 3 float */
    int rows, cols, array_length;
    unsigned offset, size;      /* byte offset and size inside the uniform buffer */
    int has_initializer;
    float initializer[16];      /* raw bits of ints are kept: use initializer_int for int/uint/bool */
    int initializer_int[16];
} t2fx_uniform;

typedef struct {
    const char *name;
    const char *vertex_entry;
    const char *pixel_entry;
    const char *render_target[T2FX_MAX_TARGETS];  /* texture unique_name; "" = back buffer */
    int clear;
    int generate_mipmaps;
    int srgb_write;
    int blend_enable;
    int blend_op, blend_src, blend_dst;
    int blend_alpha_op, blend_alpha_src, blend_alpha_dst;
    int write_mask;
    int stencil_enable, stencil_func, stencil_pass, stencil_fail, stencil_depth_fail;
    int stencil_read_mask, stencil_write_mask, stencil_ref;
    int topology;               /* 1 points, 2 lines, 3 line strip, 4 triangles, 5 triangle strip */
    unsigned num_vertices;
    unsigned viewport_width, viewport_height;  /* 0 = the buffer size */
    int sampler_count;
    const int *sampler_index;       /* effect sampler index per binding */
    const int *sampler_binding;     /* GLSL binding point per entry */
    int texture_count;
    const int *texture_index;
    const int *texture_binding;
} t2fx_pass;

/* Compile `path`. include_dirs/definitions may be NULL. definitions are "NAME=value".
 * Returns NULL and fills error on failure; warnings are available via t2fx_log. */
t2fx_effect *t2fx_compile(const char *path, const char *const *include_dirs, int include_count,
                          const char *const *definitions, int definition_count,
                          int width, int height, char *error, int error_size);
void t2fx_free(t2fx_effect *effect);

const char *t2fx_log(const t2fx_effect *effect);            /* preprocessor/parser warnings */
unsigned t2fx_uniform_buffer_size(const t2fx_effect *effect);

int t2fx_texture_count(const t2fx_effect *effect);
int t2fx_texture_get(const t2fx_effect *effect, int index, t2fx_texture *out);
int t2fx_sampler_count(const t2fx_effect *effect);
int t2fx_sampler_get(const t2fx_effect *effect, int index, t2fx_sampler *out);
int t2fx_uniform_count(const t2fx_effect *effect);
int t2fx_uniform_get(const t2fx_effect *effect, int index, t2fx_uniform *out);

int t2fx_technique_count(const t2fx_effect *effect);
const char *t2fx_technique_name(const t2fx_effect *effect, int technique);
const char *t2fx_technique_annotations(const t2fx_effect *effect, int technique);
int t2fx_pass_count(const t2fx_effect *effect, int technique);
int t2fx_pass_get(const t2fx_effect *effect, int technique, int pass, t2fx_pass *out);

/* GLSL source (#version 430, OpenGL semantics) for one entry point, or NULL. */
const char *t2fx_shader_source(t2fx_effect *effect, const char *entry_point);

#ifdef __cplusplus
}
#endif
#endif
