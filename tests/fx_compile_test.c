/* Compiles a ReShade FX effect through t2_fx.so and checks the reflection data. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../t2_fx.h"

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); }
    fputs(text, f); fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s t2_fx.so scratch-dir [glsl-out]\n", argv[0]); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW);
    CHECK(lib);
#define FN(ret, name, args) ret (*name) args = dlsym(lib, #name); CHECK(name)
    FN(t2fx_effect *, t2fx_compile, (const char *, const char *const *, int, const char *const *, int, int, int, char *, int));
    FN(void, t2fx_free, (t2fx_effect *));
    FN(int, t2fx_texture_count, (const t2fx_effect *));
    FN(int, t2fx_texture_get, (const t2fx_effect *, int, t2fx_texture *));
    FN(int, t2fx_sampler_get, (const t2fx_effect *, int, t2fx_sampler *));
    FN(int, t2fx_uniform_count, (const t2fx_effect *));
    FN(int, t2fx_uniform_get, (const t2fx_effect *, int, t2fx_uniform *));
    FN(int, t2fx_technique_count, (const t2fx_effect *));
    FN(const char *, t2fx_technique_name, (const t2fx_effect *, int));
    FN(int, t2fx_pass_count, (const t2fx_effect *, int));
    FN(int, t2fx_pass_get, (const t2fx_effect *, int, int, t2fx_pass *));
    FN(const char *, t2fx_shader_source, (t2fx_effect *, const char *));
    FN(unsigned, t2fx_uniform_buffer_size, (const t2fx_effect *));

    char path[512], header[512];
    snprintf(header, sizeof header, "%s/fixture.fxh", argv[2]);
    snprintf(path, sizeof path, "%s/fixture.fx", argv[2]);
    {
        FILE *f = fopen(header, "w");
        fputs("#pragma once\n"
              "texture BackBufferTex : COLOR;\n"
              "sampler BackBuffer { Texture = BackBufferTex; };\n"
              "void PostProcessVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 tex : TEXCOORD)\n"
              "{ tex.x = (id == 2) ? 2.0 : 0.0; tex.y = (id == 1) ? 2.0 : 0.0;\n"
              "  pos = float4(tex * float2(2, -2) + float2(-1, 1), 0, 1); }\n", f);
        fclose(f);
    }
    write_file(path,
        "#include \"fixture.fxh\"\n"
        "uniform float Strength < ui_type = \"slider\"; ui_min = 0.0; ui_max = 2.0; ui_label = \"Strength\"; > = 0.75;\n"
        "uniform bool Invert = false;\n"
        "uniform float3 Tint = float3(1.0, 0.5, 0.25);\n"
        "texture Half { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };\n"
        "sampler HalfSampler { Texture = Half; MagFilter = POINT; };\n"
        "float4 Down(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2D(BackBuffer, uv); }\n"
        "float4 Grade(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {\n"
        "  float3 c = tex2D(HalfSampler, uv).rgb * Tint * Strength;\n"
        "  return float4(Invert ? 1.0 - c : c, 1.0); }\n"
        "technique Fixture < ui_label = \"Fixture\"; > {\n"
        "  pass Downsample { VertexShader = PostProcessVS; PixelShader = Down; RenderTarget = Half; }\n"
        "  pass Grade { VertexShader = PostProcessVS; PixelShader = Grade; }\n"
        "}\n");

    char error[2048] = "";
    const char *includes[] = {argv[2]};
    const char *defs[] = {"FIXTURE_DEF=3"};
    t2fx_effect *fx = t2fx_compile(path, includes, 1, defs, 1, 1280, 720, error, sizeof error);
    if (!fx) { fprintf(stderr, "compile failed: %s\n", error); return 1; }
    CHECK(t2fx_technique_count(fx) == 1);
    CHECK(strcmp(t2fx_technique_name(fx, 0), "Fixture") == 0);
    CHECK(t2fx_pass_count(fx, 0) == 2);

    int found_half = 0, found_color = 0;
    for (int i = 0; i < t2fx_texture_count(fx); i++) {
        t2fx_texture tex; CHECK(t2fx_texture_get(fx, i, &tex));
        if (!strcmp(tex.name, "Half")) {
            found_half = 1;
            CHECK(tex.width == 640 && tex.height == 360);   /* BUFFER_WIDTH / 2 resolved by the preprocessor */
            CHECK(tex.format == 10 && tex.render_target);
        }
        if (!strcmp(tex.semantic, "COLOR")) found_color = 1;
    }
    CHECK(found_half && found_color);

    int saw_strength = 0, saw_invert = 0, saw_tint = 0;
    for (int i = 0; i < t2fx_uniform_count(fx); i++) {
        t2fx_uniform u; CHECK(t2fx_uniform_get(fx, i, &u));
        if (!strcmp(u.name, "Strength")) {
            saw_strength = 1;
            CHECK(u.base == 3 && u.rows == 1 && u.has_initializer && u.initializer[0] == 0.75f);
            CHECK(strstr(u.annotations, "ui_type=slider") && strstr(u.annotations, "ui_max=2"));
        }
        if (!strcmp(u.name, "Invert")) { saw_invert = 1; CHECK(u.base == 0); }
        if (!strcmp(u.name, "Tint")) { saw_tint = 1; CHECK(u.base == 3 && u.rows == 3 && u.initializer[1] == 0.5f); }
    }
    CHECK(saw_strength && saw_invert && saw_tint);
    CHECK(t2fx_uniform_buffer_size(fx) >= 4 + 4 + 12);

    t2fx_pass pass;
    CHECK(t2fx_pass_get(fx, 0, 0, &pass));
    {   /* render targets and sampler textures are referenced by texture unique_name */
        t2fx_texture tex; int match = 0;
        for (int i = 0; i < t2fx_texture_count(fx); i++) {
            t2fx_texture_get(fx, i, &tex);
            if (!strcmp(tex.unique_name, pass.render_target[0])) match = !strcmp(tex.name, "Half");
        }
        CHECK(match);
    }
    CHECK(pass.vertex_entry[0] && pass.pixel_entry[0] && pass.num_vertices == 3);
    CHECK(pass.sampler_count >= 1);
    CHECK(t2fx_pass_get(fx, 0, 1, &pass));
    CHECK(pass.render_target[0][0] == '\0');

    const char *vs = t2fx_shader_source(fx, pass.vertex_entry);
    const char *ps = t2fx_shader_source(fx, pass.pixel_entry);
    CHECK(vs && ps && strstr(vs, "#version") && strstr(ps, "#version"));
    CHECK(strstr(ps, "void main()"));
    if (argc > 3) {
        char out[600];
        snprintf(out, sizeof out, "%s.vert", argv[3]); write_file(out, vs);
        snprintf(out, sizeof out, "%s.frag", argv[3]); write_file(out, ps);
    }
    t2fx_free(fx);

    /* errors come back as text instead of a crash */
    write_file(path, "this is not a valid effect\n");
    CHECK(t2fx_compile(path, NULL, 0, NULL, 0, 1280, 720, error, sizeof error) == NULL);
    CHECK(error[0] != '\0');
    puts("OK");
    return 0;
}
