/* Loads a ReShade preset through the hook's FX runner (t2_reshade.h) with GL stubbed out, and checks
 * the technique chain, uniform values, preprocessor definitions, depth defaults and preset cycling. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

typedef const char *(*con_evaluate_t)(const char *, int, const char *, int);
static con_evaluate_t Con__evaluate;
static void Con__addMethodS(const char *ns, const char *name, void *callback, const char *usage, int min, int max) {
    (void)ns; (void)name; (void)callback; (void)usage; (void)min; (void)max;
}
static const char *g_shaders, *g_preset;
static const char *hook_option_str(const char *key) {
    if (!strcmp(key, "video.reshade_shaders")) return g_shaders;
    if (!strcmp(key, "video.reshade_preset")) return g_preset;
    return "";
}
static int hook_option_bool(const char *key) { (void)key; return 1; }

static unsigned rt_depth_read_source(void) { return 0; }
#include "../t2_reshade.h"

static unsigned next_id = 1;
static void stub_gen(int count, unsigned *ids) { for (int i = 0; i < count; i++) ids[i] = next_id++; }
static unsigned stub_create(unsigned type) { (void)type; return next_id++; }
static unsigned stub_create_program(void) { return next_id++; }
static void stub_param(unsigned object, unsigned name, int *value) { (void)object; (void)name; *value = 1; }
static void stub_noop(void) {}
static unsigned stub_status(unsigned target) { (void)target; return RS_FRAMEBUFFER_COMPLETE; }

static void install_gl_stubs(void) {
    void **table = (void **)&rs_gl;
    size_t count = sizeof rs_gl / sizeof(void *);
    for (size_t i = 1; i < count; i++) table[i] = (void *)stub_noop;
    rs_gl.GenTextures = stub_gen; rs_gl.GenBuffers = stub_gen; rs_gl.GenFramebuffers = stub_gen;
    rs_gl.GenSamplers = stub_gen;
    rs_gl.CreateShader = stub_create; rs_gl.CreateProgram = stub_create_program;
    rs_gl.GetShaderiv = stub_param; rs_gl.GetProgramiv = stub_param;
    rs_gl.CheckFramebufferStatus = stub_status;
    rs_gl.state = 1;
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); }
    fputs(text, f); fclose(f);
}

static float ubo_float(struct rs_effect *effect, const char *name) {
    for (int i = 0; i < effect->uniform_count; i++)
        if (!strcmp(effect->uniforms[i].info.name, name)) {
            float value; memcpy(&value, effect->ubo_data + effect->uniforms[i].info.offset, 4); return value;
        }
    return -12345.0f;
}

static int ubo_int(struct rs_effect *effect, const char *name) {
    for (int i = 0; i < effect->uniform_count; i++)
        if (!strcmp(effect->uniforms[i].info.name, name)) {
            int value; memcpy(&value, effect->ubo_data + effect->uniforms[i].info.offset, 4); return value;
        }
    return -12345;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s t2_fx.so scratch-dir\n", argv[0]); return 2; }
    setenv("T2LAUNCH_FX_LIB", argv[1], 1);
    char shaders[600], path[700];
    snprintf(shaders, sizeof shaders, "%s/reshade", argv[2]);
    snprintf(path, sizeof path, "%s", shaders); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Shaders", shaders); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Shaders/sub", shaders); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Presets", shaders); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Textures", shaders); mkdir(path, 0755);

    snprintf(path, sizeof path, "%s/Shaders/sub/fixture.fxh", shaders);
    write_file(path,
        "#pragma once\n"
        "texture BackBufferTex : COLOR;\n"
        "sampler BackBuffer { Texture = BackBufferTex; };\n"
        "void PostProcessVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 tex : TEXCOORD)\n"
        "{ tex.x = (id == 2) ? 2.0 : 0.0; tex.y = (id == 1) ? 2.0 : 0.0;\n"
        "  pos = float4(tex * float2(2, -2) + float2(-1, 1), 0, 1); }\n");
    snprintf(path, sizeof path, "%s/Shaders/fixture.fx", shaders);
    write_file(path,
        "#include \"fixture.fxh\"\n"   /* found through the recursive include path */
        "#ifndef RESHADE_DEPTH_INPUT_IS_REVERSED\n#error depth default missing\n#endif\n"
        "#ifndef GLOBAL_DEF\n#define GLOBAL_DEF 0\n#endif\n#ifndef EXTRA\n#define EXTRA 0\n#endif\n"
        "uniform float Strength = 0.5;\n"
        "uniform float3 Tint = float3(1.0, 1.0, 1.0);\n"
        "uniform bool Invert = false;\n"
        "uniform int Defines = GLOBAL_DEF * 10 + EXTRA;\n"
        "uniform int Reversed = RESHADE_DEPTH_INPUT_IS_REVERSED;\n"
        "uniform float Far = RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;\n"
        "float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target\n"
        "{ return float4(tex2D(BackBuffer, uv).rgb * Tint * Strength, Invert ? 0.0 : 1.0); }\n"
        "technique Fixture { pass { VertexShader = PostProcessVS; PixelShader = PS; } }\n"
        "technique Second { pass { VertexShader = PostProcessVS; PixelShader = PS; } }\n");
    snprintf(path, sizeof path, "%s/Presets/p.ini", shaders);
    write_file(path,
        "; comment\n"
        "PreprocessorDefinitions=GLOBAL_DEF=2\n"
        "Techniques=Second@fixture.fx,Fixture@fixture.fx,Gone@missing.fx\n"
        "TechniqueSorting=Second@fixture.fx,Fixture@fixture.fx,Gone@missing.fx\n"
        "\n[fixture.fx]\n"
        "Strength=1.250000\n"
        "Tint=0.100000,0.200000,0.300000\n"
        "Invert=1\n"
        "PreprocessorDefinitions=EXTRA=7\n");
    snprintf(path, sizeof path, "%s/Presets/q.ini", shaders);
    write_file(path, "Techniques=Fixture@fixture.fx\n");

    g_shaders = shaders;
    g_preset = "p.ini";
    install_gl_stubs();
    rs.enabled = 1;
    CHECK(rs_load_library());
    CHECK(rs_setup(1280, 720));

    CHECK(rs.chain_count == 2);                       /* the missing effect is skipped */
    CHECK(rs.effect_count == 1);                      /* one effect file shared by both techniques */
    struct rs_effect *effect = rs.chain[0].effect;
    CHECK(effect->ok == 1);
    CHECK(!strcmp(effect->technique_names[rs.chain[0].index], "Second"));   /* TechniqueSorting order */
    CHECK(!strcmp(effect->technique_names[rs.chain[1].index], "Fixture"));
    CHECK(ubo_float(effect, "Strength") == 1.25f);
    CHECK(ubo_float(effect, "Tint") == 0.1f);         /* first component of the vector */
    CHECK(ubo_int(effect, "Invert") == 1);
    CHECK(ubo_int(effect, "Defines") == 27);          /* global GLOBAL_DEF=2 and per-effect EXTRA=7 */
    CHECK(ubo_int(effect, "Reversed") == 0);          /* hook default */
    CHECK(ubo_float(effect, "Far") == 1000.0f);       /* no frustum seen yet */
    CHECK(rs.width == 1280 && rs.height == 720);

    /* the in-game dialog's commands */
    {
        const char *noargs[1] = {"cmd"};
        CHECK(!strcmp(rs_effects_cb(NULL, 1, noargs), "fixture.fx"));
        const char *effect_arg[2] = {"cmd", "0"};
        const char *listing = rs_uniforms_cb(NULL, 2, effect_arg);
        CHECK(strstr(listing, "Strength\tf\t0\t1\t1.25") != NULL);      /* label, kind, default range, value */
        CHECK(strstr(listing, "Tint x\tf") != NULL && strstr(listing, "Tint z\tf") != NULL);
        CHECK(strstr(listing, "Invert\tb") != NULL);
        CHECK(strstr(listing, "Far") != NULL);                              /* editable although no ui_ annotations */
        const char *set[5] = {"cmd", "0", "0", "0", "0.75"};   /* Strength is the first uniform */
        CHECK(!strcmp(rs_set_cb(NULL, 5, set), "1"));
        CHECK(ubo_float(effect, "Strength") == 0.75f);
        const char *bad[5] = {"cmd", "0", "99", "0", "1"};
        CHECK(!strcmp(rs_set_cb(NULL, 5, bad), ""));
        const char *tint[5] = {"cmd", "0", "1", "2", "0.5"};   /* Tint.z */
        CHECK(!strcmp(rs_set_cb(NULL, 5, tint), "1"));
        CHECK(!strcmp(rs_toggle_cb(NULL, 1, noargs), "0") && rs.paused);
        CHECK(!strcmp(rs_toggle_cb(NULL, 1, noargs), "1") && !rs.paused);
        CHECK(!strcmp(rs_status_cb(NULL, 1, noargs), "1\tp.ini"));
        const char *presets = rs_presets_cb(NULL, 1, noargs);
        CHECK(!strcmp(presets, "p.ini\nq.ini"));
        const char *traversal[2] = {"cmd", "../evil.ini"};
        CHECK(!strcmp(rs_preset_cb(NULL, 2, traversal), ""));
        CHECK(strstr(rs.preset_path, "/Presets/p.ini") != NULL);
        /* save, then load the saved file back */
        CHECK(!strcmp(rs_save_cb(NULL, 1, noargs), "1"));
        rs_unload();
        CHECK(rs_setup(1280, 720));
        struct rs_effect *again = rs.chain[0].effect;
        CHECK(ubo_float(again, "Strength") == 0.75f);
        CHECK(ubo_int(again, "Invert") == 1);
        CHECK(ubo_int(again, "Defines") == 27);
        CHECK(rs.chain_count == 2);
        effect = again;
    }

    /* a bare preset name is looked up in Presets; cycling wraps and reloads */
    CHECK(strstr(rs.preset_path, "/Presets/p.ini") != NULL);
    rs_cycle_preset(1);
    CHECK(strstr(rs.preset_path, "/Presets/q.ini") != NULL && rs.reload_now);
    rs_cycle_preset(1);
    CHECK(strstr(rs.preset_path, "/Presets/p.ini") != NULL);
    rs_cycle_preset(-1);
    CHECK(strstr(rs.preset_path, "/Presets/q.ini") != NULL);
    CHECK(rs_setup(1280, 720));
    CHECK(rs.chain_count == 1);                       /* q.ini lists one technique */
    CHECK(ubo_float(rs.chain[0].effect, "Strength") == 0.5f);   /* untouched default again */

    /* a frustum known before compiling sets the linearisation far plane to far / near */
    rs.frustum_near = 0.1; rs.frustum_far = 500.0;
    CHECK(rs_setup(1280, 720));
    CHECK(ubo_float(rs.chain[0].effect, "Far") == 5000.0f);
    puts("OK");
    return 0;
}
