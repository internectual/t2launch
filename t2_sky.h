/* Sky views for the hook (included by t2_hook.c, single translation unit).
 *
 * Each of the Sky's six face textures can host any number of "views" (animations now; video, 360 video,
 * terminals later): rectangles in face space with a source, an opacity and a blend mode. The engine keeps
 * drawing the sky itself, so fog, camera and depth stay right; we only change what is inside the face
 * textures. A face with no enabled view shows its original graphics (the fallback), because the original
 * pixels are kept in a GPU backup and put back.
 *
 *   face 0..5 = FR, RT, BK, LF, UP, DN  (Sky + 0x2A0.. = six TextureHandles -> TextureObject, GL name at +12)
 *
 * Image convention: (0,0) is the top-left of the face image. The face textures are filled through an FBO,
 * whose memory row 0 is the bottom of clip space, so image y maps straight to clip y = -1 + 2y.
 *
 * Runs on the game thread from the SDL_GL_SwapBuffers hook, with the game's GL context current.
 */
#include <ctype.h>

#define RS_TEXTURE_BINDING_2D_VALUE 0x8069
#define SKY_FACES 6
#define SKY_MAX_VIEWS 32
#define SKY_HANDLES_OFFSET 0x2A0
#define SKY_GL_NAME_OFFSET 12
#define SKY_RESOLVE_INTERVAL 30      /* frames between looking the Sky up again */
#define SKY_ANIMATION_MS 33          /* animated views redraw at about 30 fps */

enum sky_kind { SKY_SOLID, SKY_GRADIENT, SKY_PLASMA, SKY_STARS, SKY_AURORA, SKY_CALIBRATE, SKY_KIND_COUNT };
static const char *const sky_kind_names[SKY_KIND_COUNT] = {"solid", "gradient", "plasma", "stars", "aurora", "calibrate"};
static const int sky_kind_animated[SKY_KIND_COUNT] = {0, 0, 1, 1, 1, 0};
static const char *const sky_face_names[SKY_FACES] = {"fr", "rt", "bk", "lf", "up", "dn"};
/* Views are placed in displayed coordinates: the face as a viewer sees it from inside, walls upright, the top
   and bottom with north (+Y) at the top of the view. The engine maps the textures differently (Sky::renderSkyBox
   and calcPoints: wall u runs along the viewer's left, the top's u along +X which is the viewer's left, the
   bottom's v runs towards +Y, upside down for a north-up viewer), so the texture rectangle is mirrored:
   x on the four walls and the top, y on the bottom. */
static const int sky_face_flip_x[SKY_FACES] = {1, 1, 1, 1, 1, 0};
static const int sky_face_flip_y[SKY_FACES] = {0, 0, 0, 0, 0, 1};

struct sky_view {
    int id;                 /* 0 = free slot */
    int face;               /* 0..5 */
    char spec[256];         /* "builtin:plasma", ... */
    int kind;               /* enum sky_kind */
    float color[4];         /* builtin colour (solid/gradient), rgba */
    float x, y, w, h;       /* rect in face space, 0..1, origin top-left */
    float opacity;
    int blend;              /* 0 alpha, 1 add */
    int z;
    int enabled;
};

struct sky_face {
    void *texture_object;   /* the TextureObject the name came from */
    unsigned name;          /* GL texture name (engine's) */
    int width, height;
    unsigned backup;        /* original pixels */
    int modified;           /* the engine texture currently holds our composite */
    uint64_t last_draw_ms;
    unsigned retry_frame;   /* the original pixels were not there yet: look again at this frame */
};

static struct {
    int gl_state;                       /* 0 unresolved, 1 ready, -1 unavailable */
    unsigned char (*IsTexture)(unsigned);
    void (*GetTexLevelParameteriv)(unsigned, int, unsigned, int *);
    void (*GetTexImage)(unsigned, int, unsigned, unsigned, void *);
    void (*Uniform3f)(int, float, float, float);
    void (*Uniform4f)(int, float, float, float, float);
    void (*BlendFunc)(unsigned, unsigned);
    void *sky;                          /* the Sky object found last */
    unsigned frame;
    struct sky_face faces[SKY_FACES];
    struct sky_view views[SKY_MAX_VIEWS];
    int next_id;
    unsigned program, fbo, read_fbo;
    int u_kind, u_time, u_face, u_color, u_opacity;
    int full_box_applied;
    int gl_name_offset;                 /* where TextureObject keeps its GL name (found by probing) */
    uint64_t start_ms;
} sky;

static uint64_t sky_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ── Specs and views ───────────────────────────────────────────────── */

/* "fr".."dn", "0".."5", "front"... -> 0..5, or -1. */
static int sky_parse_face(const char *text) {
    if (!text || !*text) return -1;
    if (text[0] >= '0' && text[0] <= '5' && !text[1]) return text[0] - '0';
    static const char *const long_names[SKY_FACES] = {"front", "right", "back", "left", "up", "down"};
    for (int i = 0; i < SKY_FACES; i++)
        if (!strcasecmp(text, sky_face_names[i]) || !strcasecmp(text, long_names[i]) ||
            (i == 4 && !strcasecmp(text, "top")) || (i == 5 && !strcasecmp(text, "bottom"))) return i;
    return -1;
}

/* "#rrggbb" or "#rrggbbaa" -> rgba floats. */
static int sky_parse_color(const char *text, float *out) {
    if (!text || text[0] != '#') return 0;
    size_t length = strlen(text + 1);
    if (length != 6 && length != 8) return 0;
    for (size_t i = 1; i <= length; i++) if (!isxdigit((unsigned char)text[i])) return 0;
    unsigned long value = strtoul(text + 1, NULL, 16);
    if (length == 6) value = (value << 8) | 0xff;
    out[0] = (float)((value >> 24) & 0xff) / 255.0f; out[1] = (float)((value >> 16) & 0xff) / 255.0f;
    out[2] = (float)((value >> 8) & 0xff) / 255.0f; out[3] = (float)(value & 0xff) / 255.0f;
    return 1;
}

/* "builtin:<kind>[:#colour]" -> kind (and colour); -1 when the spec is not a supported source. */
static int sky_parse_spec(const char *spec, float *color) {
    static const char prefix[] = "builtin:";
    color[0] = 0.0f; color[1] = 0.8f; color[2] = 0.2f; color[3] = 1.0f;
    if (!spec || strncmp(spec, prefix, sizeof prefix - 1) != 0) return -1;
    char name[32] = "";
    const char *rest = spec + sizeof prefix - 1;
    size_t length = strcspn(rest, ":");
    if (length == 0 || length >= sizeof name) return -1;
    memcpy(name, rest, length); name[length] = 0;
    int kind = -1;
    for (int i = 0; i < SKY_KIND_COUNT; i++) if (!strcasecmp(name, sky_kind_names[i])) kind = i;
    if (kind < 0) return -1;
    if (rest[length] == ':') sky_parse_color(rest + length + 1, color);
    return kind;
}

static struct sky_view *sky_find_view(int id) {
    for (int i = 0; i < SKY_MAX_VIEWS; i++) if (sky.views[i].id == id && id) return &sky.views[i];
    return NULL;
}

static int sky_view_count(void) {
    int count = 0;
    for (int i = 0; i < SKY_MAX_VIEWS; i++) count += sky.views[i].id != 0;
    return count;
}

/* Add a view; returns its id (> 0) or 0 when the spec/face is invalid or the table is full. */
static int sky_add_view(int face, const char *spec, float x, float y, float w, float h, float opacity) {
    float color[4];
    int kind = sky_parse_spec(spec, color);
    if (face < 0 || face >= SKY_FACES || kind < 0 || strlen(spec) >= sizeof sky.views[0].spec) return 0;
    for (int i = 0; i < SKY_MAX_VIEWS; i++) {
        if (sky.views[i].id) continue;
        struct sky_view *view = &sky.views[i];
        memset(view, 0, sizeof *view);
        view->id = ++sky.next_id;
        view->face = face;
        snprintf(view->spec, sizeof view->spec, "%s", spec);
        view->kind = kind;
        memcpy(view->color, color, sizeof color);
        view->x = x < 0 ? 0 : x > 1 ? 1 : x;
        view->y = y < 0 ? 0 : y > 1 ? 1 : y;
        view->w = w <= 0 ? 0.01f : w > 1 ? 1 : w;
        view->h = h <= 0 ? 0.01f : h > 1 ? 1 : h;
        view->opacity = opacity < 0 ? 0 : opacity > 1 ? 1 : opacity;
        view->enabled = 1;
        return view->id;
    }
    return 0;
}

static int sky_set_field(struct sky_view *view, const char *field, const char *value) {
    float number = (float)atof(value);
    if (!strcmp(field, "x")) view->x = number < 0 ? 0 : number > 1 ? 1 : number;
    else if (!strcmp(field, "y")) view->y = number < 0 ? 0 : number > 1 ? 1 : number;
    else if (!strcmp(field, "w")) view->w = number <= 0 ? 0.01f : number > 1 ? 1 : number;
    else if (!strcmp(field, "h")) view->h = number <= 0 ? 0.01f : number > 1 ? 1 : number;
    else if (!strcmp(field, "opacity")) view->opacity = number < 0 ? 0 : number > 1 ? 1 : number;
    else if (!strcmp(field, "blend")) view->blend = !strcasecmp(value, "add") || atoi(value) == 1;
    else if (!strcmp(field, "z")) view->z = atoi(value);
    else if (!strcmp(field, "enabled")) view->enabled = !strcasecmp(value, "true") || atoi(value) != 0;
    else if (!strcmp(field, "face")) { int face = sky_parse_face(value); if (face < 0) return 0; view->face = face; }
    else if (!strcmp(field, "spec")) {
        float color[4];
        int kind = sky_parse_spec(value, color);
        if (kind < 0 || strlen(value) >= sizeof view->spec) return 0;
        snprintf(view->spec, sizeof view->spec, "%s", value);
        view->kind = kind;
        memcpy(view->color, color, sizeof color);
    } else return 0;
    return 1;
}

/* ── Layouts: one view per line ────────────────────────────────────── */

static int sky_layout_name_ok(const char *name) {
    if (!name || !*name || strlen(name) > 64) return 0;
    for (const char *c = name; *c; c++) if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') return 0;
    return 1;
}

static int sky_layout_path(const char *name, char *path, size_t size, int make_directory) {
    if (!sky_layout_name_ok(name)) return 0;
    const char *home = getenv("HOME");
    const char *data = getenv("XDG_DATA_HOME");
    char base[600], directory[700];
    if (data && *data) snprintf(base, sizeof base, "%s", data);
    else snprintf(base, sizeof base, "%s/.local/share", home ? home : ".");
    snprintf(directory, sizeof directory, "%s/t2launch/skyviews", base);
    if (make_directory) {
        char parent[700];
        snprintf(parent, sizeof parent, "%s/t2launch", base);
        mkdir(parent, 0755);
        mkdir(directory, 0755);
    }
    return snprintf(path, size, "%s/%s.cfg", directory, name) < (int)size;
}

/* Text of one view: "face=up spec=builtin:plasma x=0 y=0 w=1 h=1 opacity=1 blend=alpha z=0 enabled=1". */
static void sky_view_line(const struct sky_view *view, char *out, size_t size) {
    snprintf(out, size, "face=%s spec=%s x=%.4f y=%.4f w=%.4f h=%.4f opacity=%.3f blend=%s z=%d enabled=%d",
             sky_face_names[view->face], view->spec, (double)view->x, (double)view->y, (double)view->w,
             (double)view->h, (double)view->opacity, view->blend ? "add" : "alpha", view->z, view->enabled);
}

/* Parse "key=value key=value ..." (values have no spaces) into a new view; returns its id or 0. */
static int sky_view_from_line(const char *line) {
    char copy[600];
    snprintf(copy, sizeof copy, "%s", line);
    int face = -1;
    char spec[256] = "";
    char *tokens[16][2];
    int count = 0;
    for (char *token = strtok(copy, " \t\r\n"); token && count < 16; token = strtok(NULL, " \t\r\n")) {
        char *equals = strchr(token, '=');
        if (!equals) continue;
        *equals = 0;
        tokens[count][0] = token; tokens[count][1] = equals + 1; count++;
    }
    for (int i = 0; i < count; i++) {
        if (!strcmp(tokens[i][0], "face")) face = sky_parse_face(tokens[i][1]);
        else if (!strcmp(tokens[i][0], "spec")) snprintf(spec, sizeof spec, "%s", tokens[i][1]);
    }
    int id = sky_add_view(face, spec, 0, 0, 1, 1, 1);
    if (!id) return 0;
    struct sky_view *view = sky_find_view(id);
    for (int i = 0; i < count; i++)
        if (strcmp(tokens[i][0], "face") && strcmp(tokens[i][0], "spec")) sky_set_field(view, tokens[i][0], tokens[i][1]);
    return id;
}

/* ── GL ────────────────────────────────────────────────────────────── */

static const char sky_vertex_shader[] =
    "void main() { gl_TexCoord[0] = gl_MultiTexCoord0; gl_Position = gl_Vertex; }\n";

/* One fragment shader for every built-in source; uv is local to the view's rectangle, (0,0) top-left. */
static const char sky_fragment_shader[] =
    "uniform int kind; uniform float time; uniform int face; uniform vec4 color; uniform float opacity;\n"
    "float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "float box(vec2 uv, vec2 lo, vec2 hi) { return step(lo.x, uv.x) * step(uv.x, hi.x) * step(lo.y, uv.y) * step(uv.y, hi.y); }\n"
    "vec4 faceColor() {\n"
    " if (face == 0) return vec4(1.0, 0.25, 0.25, 1.0); if (face == 1) return vec4(0.25, 1.0, 0.25, 1.0);\n"
    " if (face == 2) return vec4(0.3, 0.45, 1.0, 1.0); if (face == 3) return vec4(1.0, 1.0, 0.25, 1.0);\n"
    " if (face == 4) return vec4(1.0, 0.3, 1.0, 1.0); return vec4(0.3, 1.0, 1.0, 1.0); }\n"
    "void main() {\n"
    " vec2 uv = gl_TexCoord[0].xy;\n"
    " vec4 c = color;\n"
    " if (kind == 1) { c = vec4(mix(color.rgb, color.rgb * 0.15, uv.y), color.a); }\n"
    " else if (kind == 2) {\n"
    "  float v = sin(uv.x * 9.0 + time) + sin(uv.y * 9.0 + time * 1.3) + sin((uv.x + uv.y) * 7.0 + time * 0.7)\n"
    "          + sin(length(uv - 0.5) * 13.0 - time * 2.0);\n"
    "  v = v * 0.25 + 0.5; c = vec4(0.05, 0.25 + 0.75 * v, 0.1 + 0.35 * v * v, 1.0); }\n"
    " else if (kind == 3) {\n"
    "  vec2 g = uv * vec2(46.0, 46.0); vec2 cell = floor(g); vec2 f = fract(g) - 0.5;\n"
    "  float h = hash(cell + float(face) * 7.0); float star = step(0.93, h);\n"
    "  float twinkle = 0.55 + 0.45 * sin(time * (1.0 + h * 5.0) + h * 40.0);\n"
    "  float d = length(f); float a = star * twinkle * smoothstep(0.42, 0.05, d);\n"
    "  c = vec4(0.8 + 0.2 * h, 0.9, 1.0, a); }\n"
    " else if (kind == 4) {\n"
    "  float wave = sin(uv.x * 6.0 + time * 0.6) * 0.12 + sin(uv.x * 13.0 - time * 0.4) * 0.05;\n"
    "  float band = smoothstep(0.30, 0.0, abs(uv.y - 0.45 - wave));\n"
    "  float shimmer = 0.6 + 0.4 * sin(uv.x * 40.0 + time * 2.0);\n"
    "  c = vec4(mix(vec3(0.1, 1.0, 0.45), vec3(0.8, 0.2, 1.0), uv.y), band * shimmer * 0.9); }\n"
    " else if (kind == 5) {\n"
    "  vec4 f = faceColor();\n"
    "  float edge = 1.0 - box(uv, vec2(0.02), vec2(0.98));\n"
    "  float gridv = step(0.97, fract(uv.x * 8.0)) + step(0.97, fract(uv.y * 8.0));\n"
    "  float glyph = box(uv, vec2(0.30, 0.20), vec2(0.40, 0.80)) + box(uv, vec2(0.30, 0.20), vec2(0.70, 0.30))\n"
    "              + box(uv, vec2(0.30, 0.47), vec2(0.60, 0.57));\n"
    "  float dots = 0.0; for (int i = 0; i < 6; i++) if (float(i) <= float(face)) dots += box(uv, vec2(0.08 + 0.065 * float(i), 0.90), vec2(0.12 + 0.065 * float(i), 0.95));\n"
    "  float arrow = box(uv, vec2(0.78, 0.35), vec2(0.82, 0.65)) + step(abs(uv.x - 0.80), (0.35 - uv.y) * 0.5) * step(0.1, uv.y) * step(uv.y, 0.35);\n"
    "  float mark = clamp(edge + glyph + dots + arrow, 0.0, 1.0);\n"
    "  c = vec4(mix(f.rgb * 0.25 + gridv * 0.15, f.rgb, mark), 1.0); }\n"
    " gl_FragColor = vec4(c.rgb, c.a * opacity);\n"
    "}\n";

static int sky_resolve_gl(void) {
    if (sky.gl_state) return sky.gl_state > 0;
    sky.gl_state = -1;
    if (!resolve_gl_gamma() || !rs_resolve_gl()) return 0;
    void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    void *scope = gl ? gl : RTLD_DEFAULT;
    void *(*get_proc)(const unsigned char *) = (void *(*)(const unsigned char *))dlsym(scope, "glXGetProcAddressARB");
#define SKY_FN(field, name) \
    do { *(void **)&sky.field = get_proc ? get_proc((const unsigned char *)name) : NULL; \
         if (!sky.field) *(void **)&sky.field = dlsym(scope, name); \
         if (!sky.field) { fprintf(stderr, "[t2_hook] Sky views unavailable: no %s\n", name); return 0; } } while (0)
    SKY_FN(IsTexture, "glIsTexture"); SKY_FN(GetTexLevelParameteriv, "glGetTexLevelParameteriv");
    SKY_FN(GetTexImage, "glGetTexImage"); SKY_FN(Uniform3f, "glUniform3f"); SKY_FN(Uniform4f, "glUniform4f");
    SKY_FN(BlendFunc, "glBlendFunc");
#undef SKY_FN
    const char *sources[2] = {sky_vertex_shader, sky_fragment_shader};
    unsigned types[2] = {RS_VERTEX_SHADER, RS_FRAGMENT_SHADER};
    sky.program = rs_gl.CreateProgram();
    for (int i = 0; i < 2; i++) {
        unsigned shader = rs_compile_shader(types[i], sources[i], "sky views");
        if (!shader) return 0;
        rs_gl.AttachShader(sky.program, shader);
        rs_gl.DeleteShader(shader);
    }
    rs_gl.LinkProgram(sky.program);
    int linked = 0;
    rs_gl.GetProgramiv(sky.program, RS_LINK_STATUS, &linked);
    if (!linked) { fprintf(stderr, "[t2_hook] Sky views: shader did not link\n"); return 0; }
    sky.u_kind = rs_gl.GetUniformLocation(sky.program, "kind");
    sky.u_time = rs_gl.GetUniformLocation(sky.program, "time");
    sky.u_face = rs_gl.GetUniformLocation(sky.program, "face");
    sky.u_color = rs_gl.GetUniformLocation(sky.program, "color");
    sky.u_opacity = rs_gl.GetUniformLocation(sky.program, "opacity");
    rs_gl.GenFramebuffers(1, &sky.fbo);
    rs_gl.GenFramebuffers(1, &sky.read_fbo);
    sky.gl_state = 1;
    return 1;
}

/* ── Finding the Sky and its face textures ─────────────────────────── */

static void *sky_find_object(void) {
    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory) || !rv_memory_range(&memory, (void *)RV_ID_DICTIONARY, 4, PROT_READ)) return NULL;
    void *dictionary = rv_pointer((void *)RV_ID_DICTIONARY, 0);
    if (!rv_memory_range(&memory, dictionary, 0x4000, PROT_READ)) return NULL;
    size_t visited = 0;
    for (size_t bucket = 0; bucket < 4096; bucket++) {
        void *object = rv_pointer(dictionary, bucket * 4);
        for (unsigned depth = 0; object && depth < 256 && visited < 32768; depth++, visited++) {
            if (!rv_memory_range(&memory, object, 0x30, PROT_READ)) break;
            const char *class_name = rv_class_name(&memory, object);
            /* a hosted game has two Skies: the server's and the client's ghost (net flag 0x40 at +0x18,
               the same bit the demo-camera patch deals with); only the ghost has loaded textures */
            if (class_name && !strcmp(class_name, "Sky") && rv_memory_range(&memory, object, 0x2C0, PROT_READ)) {
                if (rv_word(object, SKY_HANDLES_OFFSET)) return object;   /* the client's ghost has textures loaded */
            }
            object = rv_pointer(object, 0x10);
        }
    }
    return NULL;
}

static void sky_forget_faces(void) {
    for (int i = 0; i < SKY_FACES; i++) {
        if (sky.faces[i].backup) rs_gl.DeleteTextures(1, &sky.faces[i].backup);
        memset(&sky.faces[i], 0, sizeof sky.faces[i]);
    }
}

/* Where a TextureObject keeps its GL texture name: the engine source says +12, but the shipped build is probed:
   the first word offset where every face's object holds a distinct name that glIsTexture accepts. */
static int sky_probe_name_offset(const struct rv_memory *memory, void *const *objects) {
    static const int candidates[] = {12, 16, 20, 8, 24, 28, 32, 36, 40, 4};
    for (size_t c = 0; c < sizeof candidates / sizeof candidates[0]; c++) {
        unsigned seen[SKY_FACES] = {0};
        int count = 0, ok = 1;
        for (int i = 0; i < SKY_FACES && ok; i++) {
            if (!objects[i] || !rv_memory_range(memory, objects[i], 64, PROT_READ)) continue;
            unsigned name = rv_word(objects[i], (size_t)candidates[c]);
            if (!name || name > 1000000 || !sky.IsTexture(name)) { ok = 0; break; }
            for (int j = 0; j < count; j++) if (seen[j] == name) ok = 0;
            seen[count++] = name;
        }
        if (ok && count >= 4) return candidates[c];
    }
    return 0;
}

/* Read the six TextureObjects from the Sky and note a change (new mission, new materialList). */
static int sky_resolve_faces(void) {
    void *object = sky.sky;
    struct rv_memory memory;
    int valid = object && rv_memory_snapshot(&memory) && rv_memory_range(&memory, object, 0x2C0, PROT_READ);
    if (valid) {
        const char *class_name = rv_class_name(&memory, object);
        valid = class_name && !strcmp(class_name, "Sky");
    }
    if (!valid) sky.sky = object = sky_find_object();
    if (!object) return 0;
    if (!rv_memory_snapshot(&memory)) return 0;
    void *objects[SKY_FACES];
    int changed = 0;
    for (int i = 0; i < SKY_FACES; i++) {
        objects[i] = rv_pointer(object, SKY_HANDLES_OFFSET + 4 * (size_t)i);
        changed |= objects[i] != sky.faces[i].texture_object;
    }
    if (!changed) return 1;
    sky.gl_name_offset = sky_probe_name_offset(&memory, objects);
    fprintf(stderr, "[t2_hook] Sky views: Sky %p, texture objects %p %p %p %p %p %p, GL name at +%d\n", object,
            objects[0], objects[1], objects[2], objects[3], objects[4], objects[5], sky.gl_name_offset);
    int found = 0;
    for (int i = 0; i < SKY_FACES; i++) {
        struct sky_face *face = &sky.faces[i];
        if (face->backup) rs_gl.DeleteTextures(1, &face->backup);
        memset(face, 0, sizeof *face);
        face->texture_object = objects[i];
        if (sky.gl_name_offset && objects[i] && rv_memory_range(&memory, objects[i], 64, PROT_READ)) {
            unsigned name = rv_word(objects[i], (size_t)sky.gl_name_offset);
            if (name && sky.IsTexture(name)) face->name = name;
        }
        found += face->name != 0;
    }
    return found;
}

/* Ensure a face has a backup of its original pixels (and an RGBA8 render-capable texture). */
static int sky_capture_face(struct sky_face *face) {
    if (face->backup) return 1;
    int previous = 0, width = 0, height = 0;
    gl_gamma.GetIntegerv(RS_TEXTURE_BINDING_2D_VALUE, &previous);
    gl_gamma.BindTexture(RS_TEXTURE_2D, face->name);
    sky.GetTexLevelParameteriv(RS_TEXTURE_2D, 0, 0x1000, &width);    /* GL_TEXTURE_WIDTH */
    sky.GetTexLevelParameteriv(RS_TEXTURE_2D, 0, 0x1001, &height);   /* GL_TEXTURE_HEIGHT */
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        gl_gamma.BindTexture(RS_TEXTURE_2D, (unsigned)previous);
        return 0;
    }
    face->width = width; face->height = height;
    /* Copy the original pixels through the CPU: this also decompresses compressed faces. */
    unsigned char *pixels = malloc((size_t)width * (size_t)height * 4);
    if (!pixels) { gl_gamma.BindTexture(RS_TEXTURE_2D, (unsigned)previous); return 0; }
    sky.GetTexImage(RS_TEXTURE_2D, 0, RS_RGBA, RS_UNSIGNED_BYTE, pixels);
    unsigned long sum = 0;
    for (size_t i = 0; i < (size_t)width * (size_t)height * 4; i += 4) sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
    if (sum == 0) {
        /* The engine creates the GL texture early but fills it when the face is first drawn; wait for that,
           otherwise the "original" would be black. */
        free(pixels);
        gl_gamma.BindTexture(RS_TEXTURE_2D, (unsigned)previous);
        face->retry_frame = sky.frame + 30;
        return 0;
    }
    face->backup = rs_make_texture(width, height, 1, 28, NULL, 0);
    gl_gamma.BindTexture(RS_TEXTURE_2D, face->backup);
    rs_gl.TexSubImage2D(RS_TEXTURE_2D, 0, 0, 0, width, height, RS_RGBA, RS_UNSIGNED_BYTE, pixels);
    /* Make the engine texture renderable: re-specify level 0 as plain RGBA8 with the same pixels. */
    gl_gamma.BindTexture(RS_TEXTURE_2D, face->name);
    gl_gamma.TexImage2D(RS_TEXTURE_2D, 0, RS_RGBA8, width, height, 0, RS_RGBA, RS_UNSIGNED_BYTE, pixels);
    free(pixels);
    gl_gamma.BindTexture(RS_TEXTURE_2D, (unsigned)previous);
    fprintf(stderr, "[t2_hook] Sky view face %d: GL texture %u, %dx%d\n", (int)(face - sky.faces), face->name, width, height);
    return 1;
}

/* ── Drawing ───────────────────────────────────────────────────────── */

static int sky_compare_views(const void *a, const void *b) {
    const struct sky_view *left = *(struct sky_view *const *)a, *right = *(struct sky_view *const *)b;
    return left->z != right->z ? left->z - right->z : left->id - right->id;
}

static void sky_render_face(int index, double seconds) {
    struct sky_face *face = &sky.faces[index];
    struct sky_view *views[SKY_MAX_VIEWS];
    int count = 0;
    for (int i = 0; i < SKY_MAX_VIEWS; i++)
        if (sky.views[i].id && sky.views[i].enabled && sky.views[i].face == index) views[count++] = &sky.views[i];
    qsort(views, (size_t)count, sizeof views[0], sky_compare_views);

    /* The face texture is the draw target; the backup is the read source for the fallback pixels. */
    rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, sky.read_fbo);
    rs_gl.FramebufferTexture2D(RS_READ_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, face->backup, 0);
    rs_gl.BindFramebuffer(RS_DRAW_FRAMEBUFFER, sky.fbo);
    rs_gl.FramebufferTexture2D(RS_DRAW_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, face->name, 0);
    unsigned attachment = RS_COLOR_ATTACHMENT0;
    rs_gl.DrawBuffers(1, &attachment);
    if (rs_gl.CheckFramebufferStatus(RS_DRAW_FRAMEBUFFER) != RS_FRAMEBUFFER_COMPLETE) return;
    rs_gl.BlitFramebuffer(0, 0, face->width, face->height, 0, 0, face->width, face->height,
                          RS_COLOR_BUFFER_BIT, RS_NEAREST);   /* the original graphics (the fallback) */
    face->modified = count > 0;
    if (count) {
        rs_gl.BindFramebuffer(RS_FRAMEBUFFER, sky.fbo);
        gl_gamma.Viewport(0, 0, face->width, face->height);
        gl_gamma.Enable(RS_BLEND);
        rs_gl.UseProgram(sky.program);
        for (int i = 0; i < count; i++) {
            const struct sky_view *view = views[i];
            sky.BlendFunc(0x0302, view->blend ? 1u : 0x0303u);   /* SRC_ALPHA, ONE or ONE_MINUS_SRC_ALPHA */
            rs_gl.Uniform1i(sky.u_kind, view->kind);
            gl_gamma.Uniform1f(sky.u_time, (float)seconds);
            rs_gl.Uniform1i(sky.u_face, index);
            sky.Uniform4f(sky.u_color, view->color[0], view->color[1], view->color[2], view->color[3]);
            gl_gamma.Uniform1f(sky.u_opacity, view->opacity);
            /* the rectangle in texture space, and which way its local (0,0)..(1,1) runs */
            float fx = sky_face_flip_x[index] ? 1.0f : 0.0f, fy = sky_face_flip_y[index] ? 1.0f : 0.0f;
            float left = fx ? 1.0f - (view->x + view->w) : view->x, top = fy ? 1.0f - (view->y + view->h) : view->y;
            float x0 = -1.0f + 2.0f * left, x1 = -1.0f + 2.0f * (left + view->w);
            float y0 = -1.0f + 2.0f * top, y1 = -1.0f + 2.0f * (top + view->h);
            gl_gamma.Begin(GL_QUADS_);
            gl_gamma.TexCoord2f(fx, fy); gl_gamma.Vertex2f(x0, y0);
            gl_gamma.TexCoord2f(1 - fx, fy); gl_gamma.Vertex2f(x1, y0);
            gl_gamma.TexCoord2f(1 - fx, 1 - fy); gl_gamma.Vertex2f(x1, y1);
            gl_gamma.TexCoord2f(fx, 1 - fy); gl_gamma.Vertex2f(x0, y1);
            gl_gamma.End();
        }
        gl_gamma.Disable(RS_BLEND);
        rs_gl.UseProgram(0);
    }
    gl_gamma.BindTexture(RS_TEXTURE_2D, face->name);
    rs_gl.GenerateMipmap(RS_TEXTURE_2D);
}

/* ── Per frame ─────────────────────────────────────────────────────── */

static int sky_any_work(void) {
    if (sky_view_count()) return 1;
    for (int i = 0; i < SKY_FACES; i++) if (sky.faces[i].modified) return 1;
    return 0;
}

static int sky_set_full_box(int on);
static const char *sky_layout_load_cb(void *obj, S32 argc, const char **argv);

/* Once the console is up: apply the startup options (a layout, the full box). */
static void sky_options_once(void) {
    static int done;
    if (done || !registered) return;
    done = 1;
    if (hook_option_bool("sky.full_box")) (void)sky_set_full_box(1);
    const char *layout = hook_option_str("sky.layout");
    if (layout && *layout) {
        const char *argv[2] = {"t2launch_skyLayoutLoad", layout};
        const char *loaded = sky_layout_load_cb(NULL, 2, argv);
        fprintf(stderr, "[t2_hook] Sky layout %s: %s view(s)\n", layout, loaded);
    }
}

/* Texture work waits until the game is actually playing: the client connection exists and has been given its
   control object. (Doing it while a mission is still loading is not safe, and evaluating script from the swap hook
   at that time stalls the load, so this reads the connection natively.) Cached for 15 frames. */
static int sky_game_playing(void) {
    static unsigned counter;
    static int playing;
    if (counter++ % 15 == 0) {
        struct rv_memory memory;
        playing = 0;
        if (rv_memory_snapshot(&memory) && rv_memory_range(&memory, (void *)RV_CONNECTION, 4, PROT_READ)) {
            void *connection = rv_pointer((void *)RV_CONNECTION, 0);
            playing = connection && rv_memory_range(&memory, connection, 0x8438, PROT_READ) &&
                      rv_word(connection, 0x825c) != 0;
        }
    }
    return playing;
}

/* Called after each frame on the game thread. */
static void sky_views_tick(void) {
    sky_options_once();
    if (!sky_any_work() || !sky_game_playing()) return;
    sky.frame++;
    if (!sky_resolve_gl()) return;
    if (sky.frame == 1 || sky.frame % SKY_RESOLVE_INTERVAL == 0) (void)sky_resolve_faces();
    uint64_t now = sky_now_ms();
    if (!sky.start_ms) sky.start_ms = now;
    double seconds = (double)(now - sky.start_ms) / 1000.0;

    int work[SKY_FACES] = {0}, any = 0;
    for (int i = 0; i < SKY_FACES; i++) {
        struct sky_face *face = &sky.faces[i];
        if (!face->name) continue;
        int views = 0, animated = 0, fresh = 0;
        for (int v = 0; v < SKY_MAX_VIEWS; v++) {
            const struct sky_view *view = &sky.views[v];
            if (!view->id || !view->enabled || view->face != i) continue;
            views++;
            animated |= sky_kind_animated[view->kind];
        }
        fresh = face->last_draw_ms == 0 || now - face->last_draw_ms >= (animated ? SKY_ANIMATION_MS : 250);
        /* a face redraws when it has views (animated ones at ~30 fps, the rest about 4 times a second so edits
           show promptly) or when it has to be put back to its original pixels */
        if ((views && fresh) || (!views && face->modified)) { work[i] = 1; any = 1; }
    }
    if (!any) return;

    post_begin();
    int previous_read = 0, previous_draw = 0;
    rs_gl.GetIntegerv(RS_READ_FRAMEBUFFER_BINDING, &previous_read);
    rs_gl.GetIntegerv(RS_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    for (int i = 0; i < SKY_FACES; i++) {
        if (!work[i]) continue;
        struct sky_face *face = &sky.faces[i];
        if (face->retry_frame && sky.frame < face->retry_frame) continue;
        if (!sky_capture_face(face)) continue;   /* not drawn by the engine yet (or unreadable): try again later */
        sky_render_face(i, seconds);
        face->last_draw_ms = now;
    }
    rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, (unsigned)previous_read);
    rs_gl.BindFramebuffer(RS_DRAW_FRAMEBUFFER, (unsigned)previous_draw);
    post_end();
}

/* A new GL context: every id from the old one is gone. */
static void sky_views_context_lost(void) {
    memset(sky.faces, 0, sizeof sky.faces);
    sky.sky = NULL;
    sky.program = sky.fbo = sky.read_fbo = 0;
    sky.full_box_applied = 0;
    if (sky.gl_state > 0) sky.gl_state = 0;
}

/* ── Full sky box ──────────────────────────────────────────────────── */

/* Sky::render clips the four walls at the camera's height when the sky is not in fog (banHeights[0] =
   HORIZON = 0 and banHeights[1] = 60), assuming terrain hides everything below. Without terrain the lower
   half of every wall is missing. Store -1e6 in both instead, so the whole box is drawn and the fog bans
   collapse to nothing. Linux client: the two stores at 0x08221CF9 and 0x08221D03. */
#define SKY_FULL_BOX_ADDRESS_A 0x08221CF9
#define SKY_FULL_BOX_ADDRESS_B 0x08221D03
static const unsigned char sky_full_box_a_original[] = {0xc7, 0x85, 0xc4, 0xfc, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
static const unsigned char sky_full_box_a_patched[] = {0xc7, 0x85, 0xc4, 0xfc, 0xff, 0xff, 0x00, 0x24, 0x74, 0xc9};
static const unsigned char sky_full_box_b_original[] = {0xd9, 0x05, 0x6c, 0xfc, 0x44, 0x08, 0xd9, 0x9d, 0xc8, 0xfc, 0xff, 0xff};
static const unsigned char sky_full_box_b_patched[] = {0xc7, 0x85, 0xc8, 0xfc, 0xff, 0xff, 0x00, 0x24, 0x74, 0xc9, 0x90, 0x90};

static int sky_set_full_box(int on) {
    if (!target || target->dedicated) return 0;
    if (on == sky.full_box_applied) return 1;
    const struct code_patch patches[] = {
        {SKY_FULL_BOX_ADDRESS_A, on ? sky_full_box_a_original : sky_full_box_a_patched, sizeof sky_full_box_a_original, 0,
         on ? sky_full_box_a_patched : sky_full_box_a_original, sizeof sky_full_box_a_patched},
        {SKY_FULL_BOX_ADDRESS_B, on ? sky_full_box_b_original : sky_full_box_b_patched, sizeof sky_full_box_b_original, 0,
         on ? sky_full_box_b_patched : sky_full_box_b_original, sizeof sky_full_box_b_patched},
    };
    if (!apply_code_patches("Sky full box", patches, 2)) return 0;
    sky.full_box_applied = on;
    fprintf(stderr, "[t2_hook] Sky full box %s (memory only)\n", on ? "on: all six faces draw completely" : "off");
    return 1;
}

/* ── Console commands ──────────────────────────────────────────────── */

static const char *sky_add_cb(void *obj, S32 argc, const char **argv) {
    static char result[32];
    (void)obj;
    int face = argc > 1 ? sky_parse_face(argv[1]) : -1;
    float x = argc > 3 ? (float)atof(argv[3]) : 0, y = argc > 4 ? (float)atof(argv[4]) : 0;
    float w = argc > 5 ? (float)atof(argv[5]) : 1, h = argc > 6 ? (float)atof(argv[6]) : 1;
    float opacity = argc > 7 ? (float)atof(argv[7]) : 1;
    int id = argc > 2 ? sky_add_view(face, argv[2], x, y, w, h, opacity) : 0;
    snprintf(result, sizeof result, "%d", id);
    return result;
}

static const char *sky_set_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 4) return "0";
    struct sky_view *view = sky_find_view(atoi(argv[1]));
    return view && sky_set_field(view, argv[2], argv[3]) ? "1" : "0";
}

static const char *sky_remove_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    struct sky_view *view = argc > 1 ? sky_find_view(atoi(argv[1])) : NULL;
    if (!view) return "0";
    memset(view, 0, sizeof *view);
    return "1";
}

static const char *sky_clear_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    memset(sky.views, 0, sizeof sky.views);
    return "1";
}

static const char *sky_list_cb(void *obj, S32 argc, const char **argv) {
    static char result[16384];
    (void)obj; (void)argc; (void)argv;
    size_t used = 0;
    result[0] = 0;
    for (int i = 0; i < SKY_MAX_VIEWS; i++) {
        if (!sky.views[i].id) continue;
        char line[400];
        sky_view_line(&sky.views[i], line, sizeof line);
        int written = snprintf(result + used, sizeof result - used, "%s%d %s", used ? "\n" : "", sky.views[i].id, line);
        if (written < 0 || used + (size_t)written >= sizeof result) break;
        used += (size_t)written;
    }
    return result;
}

static const char *sky_layout_save_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    char path[900];
    if (argc < 2 || !sky_layout_path(argv[1], path, sizeof path, 1)) return "0";
    FILE *file = fopen(path, "w");
    if (!file) return "0";
    fputs("# t2launch sky layout: one view per line\n", file);
    for (int i = 0; i < SKY_MAX_VIEWS; i++) {
        if (!sky.views[i].id) continue;
        char line[400];
        sky_view_line(&sky.views[i], line, sizeof line);
        fprintf(file, "%s\n", line);
    }
    return fclose(file) == 0 ? "1" : "0";
}

static const char *sky_layout_load_cb(void *obj, S32 argc, const char **argv) {
    static char result[32];
    (void)obj;
    char path[900];
    if (argc < 2 || !sky_layout_path(argv[1], path, sizeof path, 0)) return "0";
    FILE *file = fopen(path, "r");
    if (!file) return "0";
    memset(sky.views, 0, sizeof sky.views);
    char line[600];
    int loaded = 0;
    while (fgets(line, sizeof line, file)) {
        char *text = line;
        while (*text == ' ' || *text == '\t') text++;
        if (*text == '#' || *text == '\n' || *text == 0) continue;
        loaded += sky_view_from_line(text) != 0;
    }
    fclose(file);
    snprintf(result, sizeof result, "%d", loaded);
    return result;
}

static const char *sky_full_box_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    int on = argc > 1 && (!strcasecmp(argv[1], "true") || atoi(argv[1]) != 0);
    return sky_set_full_box(on) ? (on ? "1" : "0") : "";
}

static void sky_register_commands(void) {
    Con__addMethodS(NULL, "t2launch_skyViewAdd", sky_add_cb,
        "t2launch_skyViewAdd(face, spec[, x, y, w, h, opacity]) - add a view to a sky face, returns its id", 3, 8);
    Con__addMethodS(NULL, "t2launch_skyViewSet", sky_set_cb,
        "t2launch_skyViewSet(id, field, value) - face, spec, x, y, w, h, opacity, blend, z, enabled", 4, 4);
    Con__addMethodS(NULL, "t2launch_skyViewRemove", sky_remove_cb, "t2launch_skyViewRemove(id)", 2, 2);
    Con__addMethodS(NULL, "t2launch_skyViewClear", sky_clear_cb, "t2launch_skyViewClear() - remove every view", 1, 1);
    Con__addMethodS(NULL, "t2launch_skyViewList", sky_list_cb, "t2launch_skyViewList() - one view per line", 1, 1);
    Con__addMethodS(NULL, "t2launch_skyLayoutSave", sky_layout_save_cb, "t2launch_skyLayoutSave(name)", 2, 2);
    Con__addMethodS(NULL, "t2launch_skyLayoutLoad", sky_layout_load_cb, "t2launch_skyLayoutLoad(name) - replace the views", 2, 2);
    Con__addMethodS(NULL, "t2launch_skyFullBox", sky_full_box_cb,
        "t2launch_skyFullBox(on) - draw the lower half of the sky walls too (maps without terrain)", 2, 2);
}
