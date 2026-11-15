// ReShade FX compiler wrapper: see t2_fx.h. Built into t2_fx.so by runtime/build.
#include "t2_fx.h"
#include "effect_codegen.hpp"
#include "effect_module.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct t2fx_effect {
    std::unique_ptr<reshadefx::codegen> backend;
    std::string log;
    std::map<std::string, std::string> shaders;
    struct pass_data {
        std::vector<int> samplers, sampler_bindings, textures, texture_bindings;
    };
    std::vector<std::vector<pass_data>> passes;
    std::vector<std::string> technique_annotations;
    std::vector<std::string> texture_annotations, sampler_annotations, uniform_annotations;
    const reshadefx::effect_module &module() const { return backend->module(); }
};

namespace {

std::string format_annotations(const std::vector<reshadefx::annotation> &list) {
    std::string out;
    for (const auto &item : list) {
        std::string value;
        char number[48];
        const auto &v = item.value;
        if (item.type.is_numeric()) {
            for (unsigned i = 0; i < item.type.components() && i < 16; i++) {
                if (i) value += ',';
                if (item.type.is_floating_point()) snprintf(number, sizeof number, "%.9g", v.as_float[i]);
                else if (item.type.is_unsigned()) snprintf(number, sizeof number, "%u", v.as_uint[i]);
                else snprintf(number, sizeof number, "%d", v.as_int[i]);
                value += number;
            }
        } else {
            value = v.string_data;
        }
        for (char &c : value) if (c == '\n') c = ' ';
        out += item.name + "=" + value + "\n";
    }
    if (!out.empty()) out.pop_back();
    return out;
}

void copy_error(char *error, int size, const std::string &text) {
    if (!error || size <= 0) return;
    snprintf(error, (size_t)size, "%s", text.c_str());
}

int base_of(const reshadefx::type &type) {
    if (type.is_boolean()) return 0;
    if (type.is_floating_point()) return 3;
    return type.is_unsigned() ? 2 : 1;
}

}  // namespace

extern "C" {

t2fx_effect *t2fx_compile(const char *path, const char *const *include_dirs, int include_count,
                          const char *const *definitions, int definition_count,
                          int width, int height, char *error, int error_size) {
    if (!path) { copy_error(error, error_size, "no effect path"); return nullptr; }
    auto effect = std::make_unique<t2fx_effect>();
    reshadefx::preprocessor pp;
    pp.add_macro_definition("__RESHADE__", "60800");
    pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    pp.add_macro_definition("__VENDOR__", "0");
    pp.add_macro_definition("__DEVICE__", "0");
    pp.add_macro_definition("__RENDERER__", "0x14300");  // desktop OpenGL 4.3
    pp.add_macro_definition("__APPLICATION__", "0");
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(width > 0 ? width : 1280));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(height > 0 ? height : 720));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");
    pp.add_macro_definition("BUFFER_COLOR_FORMAT", "1");
    pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
    for (int i = 0; i < definition_count; i++) {
        if (!definitions || !definitions[i] || !*definitions[i]) continue;
        std::string entry = definitions[i];
        size_t eq = entry.find('=');
        std::string name = entry.substr(0, eq);
        std::string value = eq == std::string::npos || eq + 1 >= entry.size() ? "1" : entry.substr(eq + 1);
        if (!name.empty()) pp.add_macro_definition(name, value);
    }
    for (int i = 0; i < include_count; i++)
        if (include_dirs && include_dirs[i]) pp.add_include_path(include_dirs[i]);
    pp.append_string(
        "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
        "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
        "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
        "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
        "#define tex2Dgather0 tex2DgatherR\n"
        "#define tex2Dgather1 tex2DgatherG\n"
        "#define tex2Dgather2 tex2DgatherB\n"
        "#define tex2Dgather3 tex2DgatherA\n");
    if (!pp.append_file(path)) { copy_error(error, error_size, pp.errors()); return nullptr; }

    effect->backend.reset(reshadefx::create_codegen_glsl(false, false, false));
    reshadefx::parser parser;
    if (!parser.parse(pp.output(), effect->backend.get())) {
        copy_error(error, error_size, pp.errors() + parser.errors());
        return nullptr;
    }
    effect->log = pp.errors() + parser.errors();

    const reshadefx::effect_module &module = effect->module();
    for (const auto &tex : module.textures) effect->texture_annotations.push_back(format_annotations(tex.annotations));
    for (const auto &smp : module.samplers) effect->sampler_annotations.push_back(format_annotations(smp.annotations));
    for (const auto &uni : module.uniforms) effect->uniform_annotations.push_back(format_annotations(uni.annotations));
    for (const auto &tech : module.techniques) {
        effect->technique_annotations.push_back(format_annotations(tech.annotations));
        std::vector<t2fx_effect::pass_data> list;
        for (const auto &pass : tech.passes) {
            t2fx_effect::pass_data data;
            for (const auto &binding : pass.sampler_bindings) {
                data.samplers.push_back((int)binding.index);
                data.sampler_bindings.push_back((int)binding.entry_point_binding);
            }
            for (const auto &binding : pass.texture_bindings) {
                data.textures.push_back((int)binding.index);
                data.texture_bindings.push_back((int)binding.entry_point_binding);
            }
            list.push_back(std::move(data));
        }
        effect->passes.push_back(std::move(list));
    }
    return effect.release();
}

void t2fx_free(t2fx_effect *effect) { delete effect; }
const char *t2fx_log(const t2fx_effect *effect) { return effect ? effect->log.c_str() : ""; }
unsigned t2fx_uniform_buffer_size(const t2fx_effect *effect) { return effect ? effect->module().total_uniform_size : 0; }

int t2fx_texture_count(const t2fx_effect *effect) { return effect ? (int)effect->module().textures.size() : 0; }
int t2fx_sampler_count(const t2fx_effect *effect) { return effect ? (int)effect->module().samplers.size() : 0; }
int t2fx_uniform_count(const t2fx_effect *effect) { return effect ? (int)effect->module().uniforms.size() : 0; }
int t2fx_technique_count(const t2fx_effect *effect) { return effect ? (int)effect->module().techniques.size() : 0; }

int t2fx_texture_get(const t2fx_effect *effect, int index, t2fx_texture *out) {
    if (!effect || !out || index < 0 || index >= t2fx_texture_count(effect)) return 0;
    const auto &tex = effect->module().textures[(size_t)index];
    out->name = tex.name.c_str();
    out->unique_name = tex.unique_name.c_str();
    out->semantic = tex.semantic.c_str();
    out->annotations = effect->texture_annotations[(size_t)index].c_str();
    out->width = tex.width; out->height = tex.height; out->depth = tex.depth; out->levels = tex.levels;
    out->dimensions = (int)tex.type;
    out->format = (int)tex.format;
    out->render_target = tex.render_target;
    return 1;
}

int t2fx_sampler_get(const t2fx_effect *effect, int index, t2fx_sampler *out) {
    if (!effect || !out || index < 0 || index >= t2fx_sampler_count(effect)) return 0;
    const auto &smp = effect->module().samplers[(size_t)index];
    out->name = smp.name.c_str();
    out->unique_name = smp.unique_name.c_str();
    out->texture_name = smp.texture_name.c_str();
    out->annotations = effect->sampler_annotations[(size_t)index].c_str();
    out->filter = (int)smp.filter;
    out->address_u = (int)smp.address_u; out->address_v = (int)smp.address_v; out->address_w = (int)smp.address_w;
    out->min_lod = smp.min_lod; out->max_lod = smp.max_lod; out->lod_bias = smp.lod_bias;
    out->srgb = smp.srgb;
    return 1;
}

int t2fx_uniform_get(const t2fx_effect *effect, int index, t2fx_uniform *out) {
    if (!effect || !out || index < 0 || index >= t2fx_uniform_count(effect)) return 0;
    const auto &uni = effect->module().uniforms[(size_t)index];
    out->name = uni.name.c_str();
    out->unique_name = uni.unique_name.c_str();
    out->annotations = effect->uniform_annotations[(size_t)index].c_str();
    out->base = base_of(uni.type);
    out->rows = (int)uni.type.rows; out->cols = (int)uni.type.cols;
    out->array_length = uni.type.is_array() ? (int)uni.type.array_length : 0;
    out->offset = uni.offset; out->size = uni.size;
    out->has_initializer = uni.has_initializer_value;
    for (int i = 0; i < 16; i++) {
        out->initializer[i] = uni.initializer_value.as_float[i];
        out->initializer_int[i] = uni.initializer_value.as_int[i];
    }
    return 1;
}

const char *t2fx_technique_name(const t2fx_effect *effect, int technique) {
    if (!effect || technique < 0 || technique >= t2fx_technique_count(effect)) return "";
    return effect->module().techniques[(size_t)technique].name.c_str();
}

const char *t2fx_technique_annotations(const t2fx_effect *effect, int technique) {
    if (!effect || technique < 0 || technique >= t2fx_technique_count(effect)) return "";
    return effect->technique_annotations[(size_t)technique].c_str();
}

int t2fx_pass_count(const t2fx_effect *effect, int technique) {
    if (!effect || technique < 0 || technique >= t2fx_technique_count(effect)) return 0;
    return (int)effect->module().techniques[(size_t)technique].passes.size();
}

int t2fx_pass_get(const t2fx_effect *effect, int technique, int index, t2fx_pass *out) {
    if (!effect || !out || index < 0 || index >= t2fx_pass_count(effect, technique)) return 0;
    const auto &pass = effect->module().techniques[(size_t)technique].passes[(size_t)index];
    const auto &data = effect->passes[(size_t)technique][(size_t)index];
    memset(out, 0, sizeof *out);
    out->name = pass.name.c_str();
    out->vertex_entry = pass.vs_entry_point.c_str();
    out->pixel_entry = pass.ps_entry_point.c_str();
    for (int i = 0; i < T2FX_MAX_TARGETS; i++) out->render_target[i] = pass.render_target_names[i].c_str();
    out->clear = pass.clear_render_targets;
    out->generate_mipmaps = pass.generate_mipmaps;
    out->srgb_write = pass.srgb_write_enable;
    out->blend_enable = pass.blend_enable[0];
    out->blend_op = (int)pass.color_blend_op[0];
    out->blend_src = (int)pass.source_color_blend_factor[0];
    out->blend_dst = (int)pass.dest_color_blend_factor[0];
    out->blend_alpha_op = (int)pass.alpha_blend_op[0];
    out->blend_alpha_src = (int)pass.source_alpha_blend_factor[0];
    out->blend_alpha_dst = (int)pass.dest_alpha_blend_factor[0];
    out->write_mask = pass.render_target_write_mask[0];
    out->stencil_enable = pass.stencil_enable;
    out->stencil_func = (int)pass.stencil_comparison_func;
    out->stencil_pass = (int)pass.stencil_pass_op;
    out->stencil_fail = (int)pass.stencil_fail_op;
    out->stencil_depth_fail = (int)pass.stencil_depth_fail_op;
    out->stencil_read_mask = pass.stencil_read_mask;
    out->stencil_write_mask = pass.stencil_write_mask;
    out->stencil_ref = pass.stencil_reference_value;
    out->topology = (int)pass.topology;
    out->num_vertices = pass.num_vertices;
    out->viewport_width = pass.viewport_width;
    out->viewport_height = pass.viewport_height;
    out->sampler_count = (int)data.samplers.size();
    out->sampler_index = data.samplers.data();
    out->sampler_binding = data.sampler_bindings.data();
    out->texture_count = (int)data.textures.size();
    out->texture_index = data.textures.data();
    out->texture_binding = data.texture_bindings.data();
    return 1;
}

const char *t2fx_shader_source(t2fx_effect *effect, const char *entry_point) {
    if (!effect || !entry_point) return nullptr;
    auto found = effect->shaders.find(entry_point);
    if (found != effect->shaders.end()) return found->second.c_str();
    std::string code, assembly, errors;
    if (!effect->backend->assemble_code_for_entry_point(entry_point, code, assembly, errors)) {
        effect->log += errors;
        return nullptr;
    }
    return effect->shaders.emplace(entry_point, std::move(code)).first->second.c_str();
}

}  // extern "C"
