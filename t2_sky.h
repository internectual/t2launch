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
#include <dirent.h>
#include <signal.h>
#include "t2_ytmesh.h"

#ifndef GL_TEXTURE1_
#define GL_TEXTURE1_ 0x84C1
#endif
#define GL_TEXTURE2_ 0x84C2
#define GL_TEXTURE3_ 0x84C3
#include <sys/prctl.h>

#define RS_TEXTURE_BINDING_2D_VALUE 0x8069
#define SKY_FACES 6
#define SKY_MAX_VIEWS 32
#define SKY_HANDLES_OFFSET 0x2A0
#define SKY_GL_NAME_OFFSET 12
#define SKY_RESOLVE_INTERVAL 30      /* frames between looking the Sky up again */
#define SKY_ANIMATION_MS 33          /* animated views redraw at about 30 fps */

enum sky_kind { SKY_SOLID, SKY_GRADIENT, SKY_PLASMA, SKY_STARS, SKY_AURORA, SKY_CALIBRATE,
                SKY_VIDEO, SKY_EQUIRECT, SKY_KIND_COUNT };
#define SKY_BUILTIN_COUNT 6          /* the kinds above that need no helper process */
#define SKY_ALL 6                    /* "face" value: a 360 view that covers all six faces */
#define SKY_SPEC_SIZE 512
#define SKY_MAX_SOURCES 12
static const char *const sky_kind_names[SKY_BUILTIN_COUNT] = {"solid", "gradient", "plasma", "stars", "aurora", "calibrate"};
static const int sky_kind_animated[SKY_KIND_COUNT] = {0, 0, 1, 1, 1, 0, 1, 1};
static const char *const sky_face_names[SKY_FACES + 1] = {"fr", "rt", "bk", "lf", "up", "dn", "all"};
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
    char spec[SKY_SPEC_SIZE];   /* "builtin:plasma", "video:/path/clip.mp4", ... */
    int group;              /* views made together (a 360 view is six); removed together */
    int source;             /* index into sky.sources for video kinds, else -1 */
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
    int width, height;      /* the size views are drawn at (never below sky.face_size) */
    int backup_width, backup_height;   /* the size of the engine's own pixels */
    unsigned backup;        /* original pixels */
    int modified;           /* the engine texture currently holds our composite */
    uint64_t last_draw_ms;
    int force;              /* a media source delivered a new frame: redraw this face */
    unsigned retry_frame;   /* the original pixels were not there yet: look again at this frame */
};

struct sky_media {
    int kind;                 /* SKY_VIDEO or SKY_EQUIRECT */
    int still;                /* image: (a picture or GIF), played slowly */
    int youtube;              /* a youtube.com / youtu.be page: resolved to stream URLs by yt-dlp when it starts */
    int term;                 /* 1 = a terminal (layout = profile name), 2 = the game console log */
    char source[480];         /* a local path or an http/https/rtsp URL */
    char layout[16];          /* 360 input layout for v360 (default "e", equirect) */
};

struct sky_source {
    int in_use, refs;
    char key[SKY_SPEC_SIZE];
    struct sky_media media;
    int width, height, fps;
    pid_t video_pid, audio_pid;
    int fd, error_fd, input_fd;     /* frames in, helper errors in, keystrokes out (terminals) */
    pthread_t thread;
    int thread_running;
    volatile int stop, ended, failed;
    unsigned char *slots[3];
    int latest, reading, writing;
    unsigned seq, uploaded;
    pthread_mutex_t lock;
    unsigned texture;
    int texture_ready, failure_reported;
    char error[200];
    int spawn_out, spawn_err;       /* YouTube: the pipe ends the starter thread hands to ffmpeg once it has resolved the page */
    int loop, with_audio, yt_height;
    unsigned char *lookup;          /* YouTube mesh video: the packed direction -> video lookup, until the game thread uploads it */
    int lookup_width, lookup_height;
    unsigned map_texture;
    int yuv;                        /* frames are planar YUV 4:2:0 (Y, then U, then V): a third of the bytes of RGBA, converted in the shader */
    unsigned tex_u, tex_v;          /* the chroma planes' textures (texture is the luma plane's) */
    double volume;
    char device[64];
    char note[200];                 /* first line the helper wrote to stderr (the pipe is drained every frame) */
};

static struct {
    int gl_state;                       /* 0 unresolved, 1 ready, -1 unavailable */
    unsigned char (*IsTexture)(unsigned);
    void (*GetTexLevelParameteriv)(unsigned, int, unsigned, int *);
    void (*GetTexImage)(unsigned, int, unsigned, unsigned, void *);
    void (*Uniform3f)(int, float, float, float);
    void (*Uniform4f)(int, float, float, float, float);
    void (*BlendFunc)(unsigned, unsigned);
    void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void *sky;                          /* the Sky object found last */
    unsigned frame;
    struct sky_face faces[SKY_FACES];
    struct sky_view views[SKY_MAX_VIEWS];
    int next_id;
    unsigned program, fbo, read_fbo;
    int u_kind, u_time, u_face, u_color, u_opacity;
    int full_box_applied;
    int gl_name_offset;                 /* where TextureObject keeps its GL name (found by probing) */
    struct sky_source sources[SKY_MAX_SOURCES];
    int u_tex, u_map, u_usemap, u_tex_u, u_tex_v, u_useyuv;
    int capture_source;                 /* 1 + the index of the source whose terminal has the keyboard; 0 = none */
    uint64_t start_ms;
} sky;

static uint64_t sky_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}


/* ── Media sources (ffmpeg helper processes) ───────────────────────── */

/* The game is 32-bit, so video is decoded by a 64-bit ffmpeg started with the game's loader settings removed;
   it writes raw RGBA frames to a pipe and a reader thread keeps the newest one for the game thread to upload. */
/* "http://", "https://", "rtsp://" or a path: no option-looking or control characters. */
static int sky_media_source_ok(const char *text) {
    if (!text || !*text || strlen(text) >= sizeof(((struct sky_media *)0)->source)) return 0;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) if (*c < 0x20 || *c == 0x7f) return 0;
    if (text[0] == '-') return 0;
    const char *colon = strstr(text, "://");
    if (colon) return !strncmp(text, "http://", 7) || !strncmp(text, "https://", 8) || !strncmp(text, "rtsp://", 7);
    return !strchr(text, ':') || text[0] == '/' || text[0] == '.';   /* no other scheme (file:, concat:, pipe:) */
}

static int sky_layout_token_ok(const char *token) {
    static const char *const allowed[] = {"e", "equirect", "c3x2", "c6x1", "c1x6", "c2x3", "eac", "fisheye",
                                          "e-sbs", "e-tb"};
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) if (!strcmp(token, allowed[i])) return 1;
    return 0;
}

/* A YouTube watch/short/live page (only these hosts are ever handed to yt-dlp). */
static int sky_youtube_url(const char *text) {
    static const char *const hosts[] = {"https://www.youtube.com/", "https://youtube.com/", "https://m.youtube.com/", "https://youtu.be/"};
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) if (!strncmp(text, hosts[i], strlen(hosts[i]))) return 1;
    return 0;
}

/* "video:<src>", "image:<src>" or "video360:<src>[@layout]"; 0 when it is none of those or invalid. */
static int sky_parse_media(const char *spec, struct sky_media *media) {
    memset(media, 0, sizeof *media);
    if (!spec) return 0;
    const char *rest;
    if (!strcmp(spec, "console")) { media->kind = SKY_VIDEO; media->term = 2; snprintf(media->source, sizeof media->source, "-"); return 1; }
    if (!strncmp(spec, "terminal", 8) && (spec[8] == 0 || spec[8] == ':')) {
        /* only a profile NAME: its command lives in the user's own config file, never in the game's scripts */
        const char *name = spec[8] ? spec + 9 : "default";
        size_t length = strlen(name);
        if (length == 0 || length >= sizeof media->layout) return 0;
        for (size_t i = 0; i < length; i++) if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') return 0;
        media->kind = SKY_VIDEO; media->term = 1;
        snprintf(media->source, sizeof media->source, "-");
        snprintf(media->layout, sizeof media->layout, "%s", name);
        return 1;
    }
    if (!strncmp(spec, "video360:", 9)) { media->kind = SKY_EQUIRECT; rest = spec + 9; }
    else if (!strncmp(spec, "video:", 6)) { media->kind = SKY_VIDEO; rest = spec + 6; }
    else if (!strncmp(spec, "image:", 6)) { media->kind = SKY_VIDEO; media->still = 1; rest = spec + 6; }
    else return 0;
    snprintf(media->source, sizeof media->source, "%s", rest);
    snprintf(media->layout, sizeof media->layout, "e");
    if (media->kind == SKY_EQUIRECT) {
        char *at = strrchr(media->source, '@');
        if (at && sky_layout_token_ok(at + 1)) {
            snprintf(media->layout, sizeof media->layout, "%s", at + 1);
            *at = 0;
        }
        if (!strcmp(media->layout, "equirect")) snprintf(media->layout, sizeof media->layout, "e");
    }
    if (!sky_media_source_ok(media->source)) return 0;
    media->youtube = sky_youtube_url(media->source);
    return 1;
}

static double sky_option_number(const char *key, double fallback, double minimum, double maximum) {
    const char *text = hook_option_str(key);
    double value = text && *text ? atof(text) : fallback;
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

/* faces are drawn at least this large (option sky.face_size): the engine's own sky textures are 512 (or a 4x4 stub),
   far less than a 1440p video can show */
static int sky_face_size(void) {
    int size = (int)sky_option_number("sky.face_size", 1024, 512, 4096);
    int power = 512;
    while (power * 2 <= size) power *= 2;
    return power;
}

static const char *sky_ffmpeg_path(void) {
    const char *override = getenv("T2LAUNCH_SKY_FFMPEG");
    if (override && *override) return override;
    const char *configured = hook_option_str("sky.ffmpeg");
    return configured && *configured ? configured : "ffmpeg";
}

static char *sky_dup(const char *text) { return strdup(text ? text : ""); }

static int sky_input_argument(const struct sky_media *media, char *out, size_t size) {
    if (strstr(media->source, "://")) return snprintf(out, size, "%s", media->source) < (int)size;
    /* The panel's file picker lists files the way the engine finds them: relative to base/ (media/clip.mp4 is
       base/media/clip.mp4), while the helper runs in the game directory. */
    const char *path = media->source;
    char under_base[560];
    if (path[0] != '/' && access(path, R_OK) != 0 && snprintf(under_base, sizeof under_base, "base/%s", path) < (int)sizeof under_base &&
        access(under_base, R_OK) == 0) path = under_base;
    return snprintf(out, size, "file:%s", path) < (int)size;   /* "file:" keeps a path from being an option */
}

/* The video helper's command line (strdup'ed strings, NULL-terminated); returns the number of arguments. */
static int sky_build_video_argv(const struct sky_media *media, int width, int height, int fps, int loop, char **argv, int max) {
    char input[540], filter[400];
    if (max < 28 || !sky_input_argument(media, input, sizeof input)) return 0;
    if (media->kind == SKY_EQUIRECT && strcmp(media->layout, "e") != 0) {
        const char *stereo = "";
        const char *layout = media->layout;
        if (!strcmp(layout, "e-sbs")) { layout = "e"; stereo = ":in_stereo=sbs:out_stereo=2d"; }
        else if (!strcmp(layout, "e-tb")) { layout = "e"; stereo = ":in_stereo=tb:out_stereo=2d"; }
        snprintf(filter, sizeof filter, "v360=input=%s:output=e%s:w=%d:h=%d:interp=linear,fps=%d,format=rgba", layout, stereo, width, height, fps);
    } else if (media->kind == SKY_EQUIRECT) {
        snprintf(filter, sizeof filter, "scale=%d:%d,fps=%d,format=rgba", width, height, fps);
    } else {
        snprintf(filter, sizeof filter, "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,fps=%d,format=rgba",
                 width, height, width, height, fps);
    }
    int n = 0;
    argv[n++] = sky_dup(sky_ffmpeg_path());
    argv[n++] = sky_dup("-hide_banner"); argv[n++] = sky_dup("-loglevel"); argv[n++] = sky_dup("error");
    argv[n++] = sky_dup("-nostdin"); argv[n++] = sky_dup("-nostats");
    argv[n++] = sky_dup("-protocol_whitelist"); argv[n++] = sky_dup("file,http,https,tcp,tls,crypto,rtsp,rtp,udp");
    if (strncmp(media->source, "rtsp://", 7) != 0) argv[n++] = sky_dup("-re");   /* pace files; live streams pace themselves */
    if (loop || media->still) { argv[n++] = sky_dup("-stream_loop"); argv[n++] = sky_dup("-1"); }
    argv[n++] = sky_dup("-i"); argv[n++] = sky_dup(input);
    argv[n++] = sky_dup("-an"); argv[n++] = sky_dup("-sn");
    argv[n++] = sky_dup("-vf"); argv[n++] = sky_dup(filter);
    argv[n++] = sky_dup("-f"); argv[n++] = sky_dup("rawvideo");
    argv[n++] = sky_dup("-pix_fmt"); argv[n++] = sky_dup("rgba");
    argv[n++] = sky_dup("pipe:1");
    argv[n] = NULL;
    return n;
}

/* The audio helper: the same input, video dropped, played to PulseAudio/PipeWire. */
static int sky_build_audio_argv(const struct sky_media *media, int loop, double volume, const char *device, char **argv, int max) {
    char input[540], filter[48];
    if (max < 28 || !sky_input_argument(media, input, sizeof input)) return 0;
    int n = 0;
    argv[n++] = sky_dup(sky_ffmpeg_path());
    argv[n++] = sky_dup("-hide_banner"); argv[n++] = sky_dup("-loglevel"); argv[n++] = sky_dup("error");
    argv[n++] = sky_dup("-nostdin"); argv[n++] = sky_dup("-nostats");
    argv[n++] = sky_dup("-protocol_whitelist"); argv[n++] = sky_dup("file,http,https,tcp,tls,crypto,rtsp,rtp,udp");
    if (strncmp(media->source, "rtsp://", 7) != 0) argv[n++] = sky_dup("-re");
    if (loop) { argv[n++] = sky_dup("-stream_loop"); argv[n++] = sky_dup("-1"); }
    argv[n++] = sky_dup("-i"); argv[n++] = sky_dup(input);
    argv[n++] = sky_dup("-vn"); argv[n++] = sky_dup("-sn");
    snprintf(filter, sizeof filter, "volume=%.2f", volume);
    argv[n++] = sky_dup("-af"); argv[n++] = sky_dup(filter);
    argv[n++] = sky_dup("-f"); argv[n++] = sky_dup("pulse");
    if (device && *device && strspn(device, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:+@-") == strlen(device)) {
        argv[n++] = sky_dup("-device"); argv[n++] = sky_dup(device);
    }
    argv[n++] = sky_dup("Tribes 2 sky");
    argv[n] = NULL;
    return n;
}

static void sky_free_argv(char **argv) { for (int i = 0; argv[i]; i++) free(argv[i]); }

/* Start a helper with stdout (and optionally stderr) on pipes the hook reads; returns its pid or -1. */
static pid_t sky_spawn(char **argv, int stdin_fd, int stdout_fd, int stderr_fd) {
    char **env = helper_environment();
    if (!env) return -1;
    pid_t child = fork();
    if (child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);   /* never outlive the game */
        int null = open("/dev/null", O_RDWR);
        dup2(stdin_fd >= 0 ? stdin_fd : null, STDIN_FILENO);
        dup2(stdout_fd >= 0 ? stdout_fd : null, STDOUT_FILENO);
        dup2(stderr_fd >= 0 ? stderr_fd : null, STDERR_FILENO);
        execvpe(argv[0], argv, env);
        _exit(127);
    }
    free(env);
    return child;
}

static size_t sky_frame_bytes(const struct sky_source *source) {
    size_t pixels = (size_t)source->width * (size_t)source->height;
    return source->yuv ? pixels * 3 / 2 : pixels * 4;
}

static void *sky_reader_thread(void *argument) {
    struct sky_source *source = argument;
    size_t frame_bytes = sky_frame_bytes(source);
    while (!source->stop) {
        unsigned char *slot = source->slots[source->writing];
        size_t got = 0;
        while (got < frame_bytes && !source->stop) {
            ssize_t n = read(source->fd, slot + got, frame_bytes - got);
            if (n > 0) { got += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if (got < frame_bytes) break;                       /* the helper ended */
        pthread_mutex_lock(&source->lock);
        source->latest = source->writing;
        source->seq++;
        for (int i = 0; i < 3; i++) if (i != source->latest && i != source->reading) { source->writing = i; break; }
        pthread_mutex_unlock(&source->lock);
    }
    if (!source->stop) {
        if (!source->seq) {   /* it never produced a frame: say why */
            source->failed = 1;
            char text[200] = "";
            ssize_t n = source->error_fd >= 0 ? read(source->error_fd, text, sizeof text - 1) : 0;
            if (n > 0) { text[n] = 0; for (char *c = text; *c; c++) if (*c == '\n' || *c == '\r') { *c = 0; break; } }
            if (!text[0]) snprintf(text, sizeof text, "%s", source->note);
            snprintf(source->error, sizeof source->error, "%s", text[0] ? text : "the helper produced no frames (is ffmpeg installed and the source readable?)");
        }
        source->ended = 1;
    }
    return NULL;
}

/* The console log is a file the engine writes when logging is on: mode 1 opens, appends and closes per line, so
   the helper always sees whole lines. The switch is the engine's own setLogMode() callback, called natively:
   running script from inside a console callback (nested Con::evaluate) corrupted the engine's evaluator and froze
   the game. 0x080EF170 is what ConsoleFunction(setLogMode) registers on the client (Windows: dumpConsoleConstructor
   lists it); it reads argv[1] and calls Con::setLogMode(). */
#define SKY_SET_LOG_MODE_CALLBACK 0x080EF170
static const unsigned char sky_set_log_mode_prologue[] = {0x55, 0x89, 0xe5, 0x83, 0xec, 0x08, 0x8b, 0x45, 0x10, 0x83, 0xc4, 0xf4};

static void sky_log_mode_native(int mode) {
    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory) || !rv_memory_range(&memory, (void *)SKY_SET_LOG_MODE_CALLBACK, sizeof sky_set_log_mode_prologue, PROT_READ) ||
        memcmp((void *)SKY_SET_LOG_MODE_CALLBACK, sky_set_log_mode_prologue, sizeof sky_set_log_mode_prologue) != 0) return;
    const char *argv[2] = {"setLogMode", mode ? "1" : "0"};
    ((void (*)(void *, int, const char **))SKY_SET_LOG_MODE_CALLBACK)(NULL, 2, argv);
}

static void (*sky_log_mode_fn)(int) = sky_log_mode_native;

static void sky_set_log_mode(int mode) {
    if (!registered || target->dedicated) return;
    sky_log_mode_fn(mode);
}

/* The text helper's command line: python, the helper script, and its options. */
static int sky_build_terminal_argv(const struct sky_media *media, int width, int height, int fps, char **argv, int max) {
    const char *helper = getenv("T2LAUNCH_SKY_TERMINAL_HELPER");
    const char *python = getenv("T2LAUNCH_SKY_PYTHON");
    if (max < 20 || !helper || !*helper || access(helper, R_OK) != 0) return 0;
    char text[32], tail[PATH_MAX + 16];
    int n = 0;
    argv[n++] = sky_dup(python && *python ? python : "python3");
    argv[n++] = sky_dup(helper);
    argv[n++] = sky_dup("--width"); snprintf(text, sizeof text, "%d", width); argv[n++] = sky_dup(text);
    argv[n++] = sky_dup("--height"); snprintf(text, sizeof text, "%d", height); argv[n++] = sky_dup(text);
    argv[n++] = sky_dup("--font"); snprintf(text, sizeof text, "%d", (int)sky_option_number("sky.terminal_font", 16, 8, 48)); argv[n++] = sky_dup(text);
    argv[n++] = sky_dup("--fps"); snprintf(text, sizeof text, "%d", fps); argv[n++] = sky_dup(text);
    if (media->term == 2) {
        /* The engine writes console.log through Loki's user-directory layer: ~/.loki/tribes2/ rather than the
           game's working directory. T2LAUNCH_SKY_CONSOLE_LOG overrides both. */
        const char *override = getenv("T2LAUNCH_SKY_CONSOLE_LOG"), *home = getenv("HOME");
        char user_directory[PATH_MAX], cwd[PATH_MAX];
        struct stat info;
        snprintf(user_directory, sizeof user_directory, "%s/.loki/tribes2", home ? home : "");
        if (override && *override) snprintf(tail, sizeof tail, "%s", override);
        else if (home && stat(user_directory, &info) == 0 && S_ISDIR(info.st_mode)) snprintf(tail, sizeof tail, "%s/console.log", user_directory);
        else {
            if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
            snprintf(tail, sizeof tail, "%s/console.log", cwd);
        }
        argv[n++] = sky_dup("--mode"); argv[n++] = sky_dup("console");
        argv[n++] = sky_dup("--tail"); argv[n++] = sky_dup(tail);
    } else {
        argv[n++] = sky_dup("--mode"); argv[n++] = sky_dup("shell");
        argv[n++] = sky_dup("--profile"); argv[n++] = sky_dup(media->layout);
    }
    argv[n] = NULL;
    return n;
}

static const char *sky_ytdlp_path(void) {
    static char home_copy[PATH_MAX];
    const char *override = getenv("T2LAUNCH_SKY_YTDLP");
    if (override && *override) return override;
    const char *configured = hook_option_str("sky.ytdlp"), *home = getenv("HOME");
    if (configured && *configured) return configured;
    if (home && snprintf(home_copy, sizeof home_copy, "%s/.local/bin/yt-dlp", home) < (int)sizeof home_copy && access(home_copy, X_OK) == 0) return home_copy;
    return "yt-dlp";
}

/* One ffmpeg process for a YouTube video: the picture goes to the pipe and the sound to PulseAudio from the same
   clock, so they cannot drift apart the way two independently started helpers do. The video and audio streams are
   separate URLs (DASH); audio_url may be empty when the page offered one combined stream. */
static int sky_build_youtube_argv(const struct sky_source *source, const char *video_url, const char *audio_url, char **argv, int max) {
    char filter[400], volume[48];
    if (max < 72) return 0;
    const struct sky_media *media = &source->media;
    if (media->kind == SKY_EQUIRECT && strcmp(media->layout, "e") != 0) {
        const char *stereo = "", *layout = media->layout;
        if (!strcmp(layout, "e-sbs")) { layout = "e"; stereo = ":in_stereo=sbs:out_stereo=2d"; }
        else if (!strcmp(layout, "e-tb")) { layout = "e"; stereo = ":in_stereo=tb:out_stereo=2d"; }
        snprintf(filter, sizeof filter, "v360=input=%s:output=e%s:w=%d:h=%d:interp=linear,fps=%d,format=rgba", layout, stereo, source->width, source->height, source->fps);
    } else if (media->kind == SKY_EQUIRECT) {
        snprintf(filter, sizeof filter, "scale=%d:%d,fps=%d,format=rgba", source->width, source->height, source->fps);
    } else {
        snprintf(filter, sizeof filter, "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,fps=%d,format=rgba",
                 source->width, source->height, source->width, source->height, source->fps);
    }
    int n = 0;
    argv[n++] = sky_dup(sky_ffmpeg_path());
    argv[n++] = sky_dup("-hide_banner"); argv[n++] = sky_dup("-loglevel"); argv[n++] = sky_dup("error");
    argv[n++] = sky_dup("-nostdin"); argv[n++] = sky_dup("-nostats");
    argv[n++] = sky_dup("-protocol_whitelist"); argv[n++] = sky_dup("file,http,https,tcp,tls,crypto");
    int mesh = source->lookup != NULL;   /* the picture goes out as the raw mesh frame; the sky shader unwraps it */
    const char *urls[2] = {video_url, audio_url};
    for (int i = 0; i < 2; i++) {
        if (!urls[i][0]) continue;
        argv[n++] = sky_dup("-reconnect"); argv[n++] = sky_dup("1");
        argv[n++] = sky_dup("-reconnect_streamed"); argv[n++] = sky_dup("1");
        argv[n++] = sky_dup("-reconnect_delay_max"); argv[n++] = sky_dup("5");
        argv[n++] = sky_dup("-re");
        if (source->loop) { argv[n++] = sky_dup("-stream_loop"); argv[n++] = sky_dup("-1"); }
        argv[n++] = sky_dup("-i"); argv[n++] = sky_dup(urls[i]);
    }
    if (mesh || source->yuv) snprintf(filter, sizeof filter, source->yuv ? "fps=%d,format=yuv420p" : "fps=%d,format=rgba", source->fps);
    argv[n++] = sky_dup("-map"); argv[n++] = sky_dup("0:v:0");
    argv[n++] = sky_dup("-an"); argv[n++] = sky_dup("-sn");
    argv[n++] = sky_dup("-vf"); argv[n++] = sky_dup(filter);
    argv[n++] = sky_dup("-f"); argv[n++] = sky_dup("rawvideo");
    argv[n++] = sky_dup("-pix_fmt"); argv[n++] = sky_dup(source->yuv ? "yuv420p" : "rgba");
    argv[n++] = sky_dup("pipe:1");
    if (source->with_audio) {
        argv[n++] = sky_dup("-map"); argv[n++] = sky_dup(audio_url[0] ? "1:a:0" : "0:a:0?");
        argv[n++] = sky_dup("-vn"); argv[n++] = sky_dup("-sn");
        snprintf(volume, sizeof volume, "volume=%.2f", source->volume);
        argv[n++] = sky_dup("-af"); argv[n++] = sky_dup(volume);
        argv[n++] = sky_dup("-f"); argv[n++] = sky_dup("pulse");
        /* ffmpeg's default PulseAudio buffer is ~0.5 s while the picture is written at once, so the sound came out half a
           second late (measured with a flash+beep clip); a 40 ms buffer brings it to ~130 ms and stable */
        argv[n++] = sky_dup("-buffer_duration"); argv[n++] = sky_dup("40");
        if (source->device[0]) { argv[n++] = sky_dup("-device"); argv[n++] = sky_dup(source->device); }
        argv[n++] = sky_dup("Tribes 2 sky");
    }
    argv[n] = NULL;
    return n;
}

/* Run a helper to completion with its stdout captured (never on the game thread). The pid is published under the
   source lock and re-checked against `stop`, so sky_source_destroy can end it at any point. 1 when it exited 0. */
static int sky_capture(struct sky_source *source, char **argv, int stderr_fd, char *buffer, size_t size, size_t *got_out) {
    int pipe_fd[2], resolved = 0;
    size_t got = 0;
    if (pipe2(pipe_fd, O_CLOEXEC) == 0) {
        pthread_mutex_lock(&source->lock);
        pid_t pid = source->stop ? -1 : sky_spawn(argv, -1, pipe_fd[1], stderr_fd);
        if (pid > 0) source->video_pid = pid;
        pthread_mutex_unlock(&source->lock);
        close(pipe_fd[1]);
        while (pid > 0 && got < size - 1) {
            ssize_t r = read(pipe_fd[0], buffer + got, size - 1 - got);
            if (r > 0) { got += (size_t)r; continue; }
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        close(pipe_fd[0]);
        if (pid > 0) {
            if (got >= size - 1) kill(pid, SIGKILL);   /* all we wanted */
            int status = 0;
            waitpid(pid, &status, 0);
            resolved = (WIFEXITED(status) && WEXITSTATUS(status) == 0) || got >= size - 1;
        }
        pthread_mutex_lock(&source->lock);
        source->video_pid = 0;   /* reaped: destroy must not signal a recycled pid */
        pthread_mutex_unlock(&source->lock);
    }
    buffer[got] = 0;
    if (got_out) *got_out = got;
    return resolved;
}

/* YouTube stores 360 video as a mesh projection described in the MP4 header: fetch the header, decode the mesh and
   unwrap it into the lookup the sky shader uses; the frame buffers are resized to the video's own size. 0 when the
   stream has no mesh (plain equirect, a webm...). */
static int sky_youtube_mesh_lookup(struct sky_source *source, const char *video_url, int frame_width, int frame_height) {
    if (frame_width < 2 || frame_height < 2 || (size_t)frame_width * frame_height > 7680u * 4320u ||
        source->media.kind != SKY_EQUIRECT || strcmp(source->media.layout, "e") != 0) return 0;
    static const size_t header_size = 131072;
    unsigned char *header = malloc(header_size + 1);
    if (!header) return 0;
    char *argv[16];
    int n = 0;
    argv[n++] = sky_dup("curl"); argv[n++] = sky_dup("-s"); argv[n++] = sky_dup("-L");
    argv[n++] = sky_dup("--max-time"); argv[n++] = sky_dup("20");
    argv[n++] = sky_dup("--proto"); argv[n++] = sky_dup("=https");
    argv[n++] = sky_dup("-r"); argv[n++] = sky_dup("0-131071");
    argv[n++] = sky_dup(video_url); argv[n] = NULL;
    size_t got = 0;
    sky_capture(source, argv, -1, (char *)header, header_size + 1, &got);
    sky_free_argv(argv);
    struct ytm_mesh mesh;
    int ok = !source->stop && got > 64 && ytm_mesh_from_header(header, got, &mesh);
    free(header);
    if (!ok) return 0;
    int width = source->width * 2 > 4096 ? 4096 : source->width * 2, height = width / 2;
    unsigned char *lookup = malloc((size_t)width * height * 4);
    ok = lookup && ytm_render_lookup(&mesh, width, height, lookup);
    ytm_mesh_free(&mesh);
    int planar = !(frame_width & 1) && !(frame_height & 1);   /* 4:2:0 needs even sizes */
    size_t frame_bytes = (size_t)frame_width * (size_t)frame_height * (planar ? 3 : 4) / (planar ? 2 : 1);
    unsigned char *slots[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 3 && ok; i++) { slots[i] = malloc(frame_bytes); ok = slots[i] != NULL; }
    if (ok) {
        pthread_mutex_lock(&source->lock);   /* no frame has been delivered yet, so the game thread is not looking at the slots */
        for (int i = 0; i < 3; i++) { free(source->slots[i]); source->slots[i] = slots[i]; }
        source->width = frame_width; source->height = frame_height; source->yuv = planar;
        source->lookup = lookup; source->lookup_width = width; source->lookup_height = height;
        pthread_mutex_unlock(&source->lock);
    } else {
        for (int i = 0; i < 3; i++) free(slots[i]);
        free(lookup);
    }
    return ok;
}

/* A YouTube video that is plain equirect: play it at its own size as YUV instead of scaling it down to sky.sphere_width. */
static int sky_youtube_native_yuv(struct sky_source *source, int frame_width, int frame_height) {
    if (frame_width < 256 || frame_width > 8192 || frame_height < 128 || (frame_width & 1) || (frame_height & 1) ||
        source->media.kind != SKY_EQUIRECT || strcmp(source->media.layout, "e") != 0) return 0;
    size_t frame_bytes = (size_t)frame_width * (size_t)frame_height * 3 / 2;
    unsigned char *slots[3] = {NULL, NULL, NULL};
    int ok = 1;
    for (int i = 0; i < 3 && ok; i++) { slots[i] = malloc(frame_bytes); ok = slots[i] != NULL; }
    if (ok) {
        pthread_mutex_lock(&source->lock);   /* no frame has been delivered yet */
        for (int i = 0; i < 3; i++) { free(source->slots[i]); source->slots[i] = slots[i]; }
        source->width = frame_width; source->height = frame_height; source->yuv = 1;
        pthread_mutex_unlock(&source->lock);
    } else for (int i = 0; i < 3; i++) free(slots[i]);
    return ok;
}

/* Runs yt-dlp, unwraps the mesh if the video has one, then replaces itself with the frame reader. */
static void *sky_youtube_thread(void *argument) {
    struct sky_source *source = argument;
    char text[16384], format[160], height[16];
    char *argv[80];
    snprintf(height, sizeof height, "%d", source->yt_height);
    /* the tallest video up to the limit (H.264 when there is a choice, and the lower frame rate: both decode cheaper) */
    snprintf(format, sizeof format, "bv*[height<=%s]+ba/b[height<=%s]/b", height, height);
    int n = 0;
    argv[n++] = sky_dup(sky_ytdlp_path());
    argv[n++] = sky_dup("--ignore-config"); argv[n++] = sky_dup("--no-playlist"); argv[n++] = sky_dup("--no-warnings");
    argv[n++] = sky_dup("--print"); argv[n++] = sky_dup("%(width)s %(height)s"); argv[n++] = sky_dup("--print"); argv[n++] = sky_dup("urls");
    argv[n++] = sky_dup("-S"); argv[n++] = sky_dup("res,+fps,vcodec:avc1");
    argv[n++] = sky_dup("-f"); argv[n++] = sky_dup(format);
    argv[n++] = sky_dup("--"); argv[n++] = sky_dup(source->media.source); argv[n] = NULL;
    size_t got = 0;
    int resolved = sky_capture(source, argv, source->spawn_err, text, sizeof text, &got);
    sky_free_argv(argv);
    /* first line: the picture size; then one or two stream URLs */
    int frame_width = 0, frame_height = 0;
    char *video_url = text, *audio_url = "";
    char *newline = strchr(text, '\n');
    if (newline && sscanf(text, "%d %d", &frame_width, &frame_height) == 2) {
        video_url = newline + 1;
        newline = strchr(video_url, '\n');
    } else frame_width = frame_height = 0;
    if (newline) { *newline++ = 0; audio_url = newline; char *end = strchr(audio_url, '\n'); if (end) *end = 0; }
    for (char *c = text; *c; c++) if (*c == '\r') *c = 0;
    int ok = resolved && !source->stop && !strncmp(video_url, "https://", 8) && (!audio_url[0] || !strncmp(audio_url, "https://", 8));
    if (!ok) fprintf(stderr, "[t2_hook] Sky views: yt-dlp (%s) %s, %zu bytes of output, stop=%d\n", sky_ytdlp_path(),
                     resolved ? "exited ok" : "failed", got, source->stop);
    if (ok) {
        int mesh = sky_youtube_mesh_lookup(source, video_url, frame_width, frame_height);
        if (mesh) fprintf(stderr, "[t2_hook] Sky views: YouTube mesh projection decoded (%dx%d frame, %dx%d lookup)\n", frame_width, frame_height, source->lookup_width, source->lookup_height);
        else if (source->media.kind == SKY_EQUIRECT) {
            int native = sky_youtube_native_yuv(source, frame_width, frame_height);
            fprintf(stderr, "[t2_hook] Sky views: no YouTube mesh found; playing the frame as plain equirect (%dx%d%s)\n", source->width, source->height, native ? ", YUV, unscaled" : "");
        }
        int count = sky_build_youtube_argv(source, video_url, audio_url, argv, 80);
        if (count) {
            pthread_mutex_lock(&source->lock);
            pid_t pid = source->stop ? -1 : sky_spawn(argv, -1, source->spawn_out, source->spawn_err);
            if (pid > 0) source->video_pid = pid;
            pthread_mutex_unlock(&source->lock);
            sky_free_argv(argv);
            close(source->spawn_out); close(source->spawn_err);
            source->spawn_out = source->spawn_err = -1;
            if (pid > 0) return sky_reader_thread(source);
        }
    }
    if (source->spawn_out >= 0) close(source->spawn_out);
    if (source->spawn_err >= 0) close(source->spawn_err);
    source->spawn_out = source->spawn_err = -1;
    if (!source->stop) {
        if (!source->note[0]) snprintf(source->note, sizeof source->note, "yt-dlp could not resolve this page (is it installed? sky.ytdlp)");
        snprintf(source->error, sizeof source->error, "%s", source->note);
        source->failed = 1;
        source->ended = 1;
    }
    return NULL;
}

static void sky_source_destroy(struct sky_source *source) {
    pthread_mutex_lock(&source->lock);
    source->stop = 1;
    /* killing the helper closes the pipe's write end, which ends the reader thread's read() */
    if (source->video_pid > 0) kill(source->video_pid, SIGKILL);
    pthread_mutex_unlock(&source->lock);
    if (source->audio_pid > 0) kill(source->audio_pid, SIGKILL);
    if (source->thread_running) pthread_join(source->thread, NULL);
    if (source->fd >= 0) close(source->fd);
    if (source->error_fd >= 0) close(source->error_fd);
    if (source->input_fd >= 0) close(source->input_fd);
    if (sky.capture_source == (int)(source - sky.sources) + 1) sky.capture_source = 0;
    if (source->media.term == 2) sky_set_log_mode(0);
    if (source->video_pid > 0) waitpid(source->video_pid, NULL, 0);
    if (source->audio_pid > 0) waitpid(source->audio_pid, NULL, 0);
    free(source->lookup);
    if (source->map_texture && sky.gl_state > 0) rs_gl.DeleteTextures(1, &source->map_texture);
    if (source->tex_u && sky.gl_state > 0) rs_gl.DeleteTextures(1, &source->tex_u);
    if (source->tex_v && sky.gl_state > 0) rs_gl.DeleteTextures(1, &source->tex_v);
    for (int i = 0; i < 3; i++) free(source->slots[i]);
    if (source->texture && sky.gl_state > 0) rs_gl.DeleteTextures(1, &source->texture);
    pthread_mutex_destroy(&source->lock);
    memset(source, 0, sizeof *source);
}

static void sky_source_release(int index) {
    if (index < 0 || index >= SKY_MAX_SOURCES || !sky.sources[index].in_use) return;
    if (--sky.sources[index].refs <= 0) sky_source_destroy(&sky.sources[index]);
}

/* A running source for this spec, started now when there is none (views of the same spec share one). */
static int sky_source_acquire(const char *spec, const struct sky_media *media) {
    for (int i = 0; i < SKY_MAX_SOURCES; i++)
        if (sky.sources[i].in_use && !strcmp(sky.sources[i].key, spec)) { sky.sources[i].refs++; return i; }
    if (!hook_option_bool("sky.media")) { fprintf(stderr, "[t2_hook] Sky views: media sources are turned off (sky.media)\n"); return -1; }
    int index = -1;
    for (int i = 0; i < SKY_MAX_SOURCES && index < 0; i++) if (!sky.sources[i].in_use) index = i;
    if (index < 0) return -1;
    struct sky_source *source = &sky.sources[index];
    memset(source, 0, sizeof *source);
    snprintf(source->key, sizeof source->key, "%s", spec);
    source->media = *media;
    source->fd = source->error_fd = source->input_fd = -1;
    int fps = media->still ? 5 : (int)sky_option_number("sky.stream_fps", 24, 1, 60);
    if (media->term) {
        fps = 12;
        source->width = (int)sky_option_number("sky.terminal_width", 1024, 256, 2048) & ~1;
        source->height = (int)sky_option_number("sky.terminal_height", 768, 192, 2048) & ~1;
    } else if (media->kind == SKY_EQUIRECT) {
        source->width = (int)sky_option_number("sky.sphere_width", 2048, 256, 4096) & ~1;
        source->height = source->width / 2;
    } else {
        source->width = (int)sky_option_number("sky.video_width", 1280, 128, 3840) & ~1;
        source->height = (source->width * 9 / 16) & ~1;
    }
    source->fps = fps;
    int loop = hook_option_bool("sky.stream_loop");
    char *argv[40];
    int youtube = media->youtube && !media->term && !media->still;
    argv[0] = NULL;
    source->loop = loop;
    source->with_audio = hook_option_bool("sky.stream_audio");
    source->volume = sky_option_number("sky.stream_volume", 50, 0, 200) / 100.0;
    source->yt_height = (int)sky_option_number("sky.youtube_height", 2160, 144, 4320);
    snprintf(source->device, sizeof source->device, "%s", getenv("T2LAUNCH_AUDIO_OUTPUT") && strspn(getenv("T2LAUNCH_AUDIO_OUTPUT"), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:+@-") == strlen(getenv("T2LAUNCH_AUDIO_OUTPUT")) ? getenv("T2LAUNCH_AUDIO_OUTPUT") : "");
    source->spawn_out = source->spawn_err = -1;
    int argc = youtube ? 1 : media->term ? sky_build_terminal_argv(media, source->width, source->height, fps, argv, 40)
                           : sky_build_video_argv(media, source->width, source->height, fps, loop, argv, 40);
    if (!argc) {
        if (media->term) fprintf(stderr, "[t2_hook] Sky views: no text helper (T2LAUNCH_SKY_TERMINAL_HELPER)\n");
        return -1;
    }
    int out[2], err[2], input[2] = {-1, -1};
    if (pipe2(out, O_CLOEXEC) != 0) { sky_free_argv(argv); return -1; }
    if (pipe2(err, O_CLOEXEC) != 0) { close(out[0]); close(out[1]); sky_free_argv(argv); return -1; }
    /* keystrokes go through a socket so that a dead helper makes send() fail instead of raising SIGPIPE */
    if (media->term && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input) != 0) {
        close(out[0]); close(out[1]); close(err[0]); close(err[1]); sky_free_argv(argv); return -1;
    }
    pthread_mutex_init(&source->lock, NULL);
    size_t frame_bytes = (size_t)source->width * (size_t)source->height * 4;
    for (int i = 0; i < 3; i++) source->slots[i] = malloc(frame_bytes);
    source->latest = -1; source->reading = -1; source->writing = 0;
    if (youtube) {
        /* the starter thread resolves the page, then spawns ffmpeg on these pipe ends and closes them */
        source->spawn_out = out[1]; source->spawn_err = err[1];
        source->video_pid = 0;
    } else {
        source->video_pid = sky_spawn(argv, input[1], out[1], err[1]);
        sky_free_argv(argv);
        close(out[1]); close(err[1]);
    }
    if (input[1] >= 0) close(input[1]);
    source->input_fd = input[0];
    source->fd = out[0];
    source->error_fd = err[0];
    fcntl(source->error_fd, F_SETFL, O_NONBLOCK);
    if (source->video_pid < 0 || !source->slots[0] || !source->slots[1] || !source->slots[2]) {
        source->in_use = 1;
        sky_source_destroy(source);
        return -1;
    }
    source->in_use = 1;
    source->refs = 1;
    if (pthread_create(&source->thread, NULL, youtube ? sky_youtube_thread : sky_reader_thread, source) == 0) source->thread_running = 1;
    if (media->term == 2) sky_set_log_mode(1);
    if (!media->term && !youtube && (media->kind != SKY_VIDEO || !media->still)) {
        if (hook_option_bool("sky.stream_audio")) {
            char *audio[40];
            const char *device = getenv("T2LAUNCH_AUDIO_OUTPUT");
            if (sky_build_audio_argv(media, loop, sky_option_number("sky.stream_volume", 50, 0, 200) / 100.0, device, audio, 40)) {
                source->audio_pid = sky_spawn(audio, -1, -1, -1);
                sky_free_argv(audio);
            }
        }
    }
    fprintf(stderr, "[t2_hook] Sky views: started %s (%dx%d at %d fps%s)\n", spec, source->width, source->height, fps,
            (source->audio_pid > 0 || (youtube && source->with_audio)) ? ", with audio" : "");
    return index;
}

/* Take the newest frame, if there is a new one: its slot is held until the caller is done with it. */
static unsigned char *sky_source_take_frame(struct sky_source *source, unsigned *seq) {
    unsigned char *frame = NULL;
    pthread_mutex_lock(&source->lock);
    if (source->latest >= 0 && source->seq != source->uploaded) {
        source->reading = source->latest;
        frame = source->slots[source->latest];
        *seq = source->seq;
    }
    pthread_mutex_unlock(&source->lock);
    return frame;
}

static void sky_source_done_with_frame(struct sky_source *source, unsigned seq) {
    pthread_mutex_lock(&source->lock);
    source->uploaded = seq;
    source->reading = -1;
    pthread_mutex_unlock(&source->lock);
}

/* ── Specs and views ───────────────────────────────────────────────── */

/* "fr".."dn", "0".."5", "front"... -> 0..5, or -1. */
static int sky_parse_face(const char *text) {
    if (!text || !*text) return -1;
    if (text[0] >= '0' && text[0] <= '5' && !text[1]) return text[0] - '0';
    if (!strcasecmp(text, "all") || !strcasecmp(text, "sphere")) return SKY_ALL;
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
    struct sky_media media;
    if (spec && strncmp(spec, prefix, sizeof prefix - 1) != 0) return sky_parse_media(spec, &media) ? media.kind : -1;
    if (!spec) return -1;
    char name[32] = "";
    const char *rest = spec + sizeof prefix - 1;
    size_t length = strcspn(rest, ":");
    if (length == 0 || length >= sizeof name) return -1;
    memcpy(name, rest, length); name[length] = 0;
    int kind = -1;
    for (int i = 0; i < SKY_BUILTIN_COUNT; i++) if (!strcasecmp(name, sky_kind_names[i])) kind = i;
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

static void sky_view_free(struct sky_view *view) {
    if (view->id && view->source >= 0) sky_source_release(view->source);
    memset(view, 0, sizeof *view);
}

static void sky_views_clear(void) {
    for (int i = 0; i < SKY_MAX_VIEWS; i++) sky_view_free(&sky.views[i]);
}

/* Point a view at a source spec (starting its helper when it is media); 0 when the spec is invalid or the
   source cannot start. The old source is released only once the new one is running. */
static int sky_view_bind(struct sky_view *view, const char *spec) {
    float color[4];
    int kind = sky_parse_spec(spec, color);
    if (kind < 0 || strlen(spec) >= sizeof view->spec) return 0;
    int source = -1;
    if (kind >= SKY_VIDEO) {
        struct sky_media media;
        if (!sky_parse_media(spec, &media)) return 0;
        source = sky_source_acquire(spec, &media);
        if (source < 0) return 0;
    }
    if (view->source >= 0) sky_source_release(view->source);
    view->source = source;
    snprintf(view->spec, sizeof view->spec, "%s", spec);
    view->kind = kind;
    memcpy(view->color, color, sizeof color);
    return 1;
}

static struct sky_view *sky_add_one(int face, const char *spec, float x, float y, float w, float h, float opacity) {
    for (int i = 0; i < SKY_MAX_VIEWS; i++) {
        if (sky.views[i].id) continue;
        struct sky_view *view = &sky.views[i];
        memset(view, 0, sizeof *view);
        view->source = -1;
        view->face = face;
        if (!sky_view_bind(view, spec)) { memset(view, 0, sizeof *view); return NULL; }
        view->id = ++sky.next_id;
        view->x = x < 0 ? 0 : x > 1 ? 1 : x;
        view->y = y < 0 ? 0 : y > 1 ? 1 : y;
        view->w = w <= 0 ? 0.01f : w > 1 ? 1 : w;
        view->h = h <= 0 ? 0.01f : h > 1 ? 1 : h;
        view->opacity = opacity < 0 ? 0 : opacity > 1 ? 1 : opacity;
        view->enabled = 1;
        return view;
    }
    return NULL;
}

/* Add a view; returns its id (> 0) or 0 when the spec/face is invalid, the source cannot start or the table is
   full. Face "all" (a 360 video on the whole sky) makes six views that share one source and one group. */
static int sky_add_view(int face, const char *spec, float x, float y, float w, float h, float opacity) {
    float color[4];
    int kind = sky_parse_spec(spec, color);
    if (face < 0 || face > SKY_ALL || kind < 0) return 0;
    if (face != SKY_ALL) {
        if (kind == SKY_EQUIRECT) return 0;   /* a sphere needs all six faces */
        struct sky_view *view = sky_add_one(face, spec, x, y, w, h, opacity);
        return view ? view->id : 0;
    }
    if (kind != SKY_EQUIRECT) return 0;
    int free_slots = 0;
    for (int i = 0; i < SKY_MAX_VIEWS; i++) free_slots += sky.views[i].id == 0;
    if (free_slots < SKY_FACES) return 0;
    int first = 0;
    for (int f = 0; f < SKY_FACES; f++) {
        struct sky_view *view = sky_add_one(f, spec, 0, 0, 1, 1, opacity);
        if (!view) {                                    /* cannot happen after the slot check; undo anyway */
            for (int i = 0; i < SKY_MAX_VIEWS; i++) if (sky.views[i].id && sky.views[i].group == first && first) sky_view_free(&sky.views[i]);
            return 0;
        }
        if (!first) first = view->id;
        view->group = first;
    }
    return first;
}

/* Remove a view, and the rest of its 360 group. */
static void sky_remove_view(struct sky_view *view) {
    int group = view->group;
    sky_view_free(view);
    if (group) for (int i = 0; i < SKY_MAX_VIEWS; i++) if (sky.views[i].id && sky.views[i].group == group) sky_view_free(&sky.views[i]);
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
    else if (!strcmp(field, "face")) {
        int face = sky_parse_face(value);
        if (face < 0 || face >= SKY_FACES || view->kind == SKY_EQUIRECT) return 0;   /* a sphere stays on its faces */
        view->face = face;
    } else if (!strcmp(field, "spec")) {
        if (view->group) return 0;   /* the faces of a 360 view share one source: remove it and add another */
        float color[4];
        int kind = sky_parse_spec(value, color);
        if (kind < 0 || kind == SKY_EQUIRECT) return 0;
        return sky_view_bind(view, value);
    } else return 0;
    return 1;
}

/* Apply a field to a view and, for a 360 group, to every face of it. */
static int sky_set_field_group(struct sky_view *view, const char *field, const char *value) {
    int ok = sky_set_field(view, field, value);
    if (ok && view->group && strcmp(field, "face") && strcmp(field, "spec"))
        for (int i = 0; i < SKY_MAX_VIEWS; i++)
            if (sky.views[i].id && sky.views[i].group == view->group && &sky.views[i] != view) sky_set_field(&sky.views[i], field, value);
    return ok;
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

/* Layout lines are space separated, so a space (and the escape character) inside a spec is written as %20 / %25. */
static void sky_escape_spec(const char *spec, char *out, size_t size) {
    size_t used = 0;
    for (; *spec && used + 4 < size; spec++) {
        if (*spec == ' ' || *spec == '%') used += (size_t)snprintf(out + used, size - used, "%%%02X", (unsigned char)*spec);
        else out[used++] = *spec;
    }
    out[used] = 0;
}

static void sky_unescape_spec(const char *in, char *out, size_t size) {
    size_t used = 0;
    for (; *in && used + 1 < size; in++) {
        if (in[0] == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = {in[1], in[2], 0};
            out[used++] = (char)strtol(hex, NULL, 16);
            in += 2;
        } else out[used++] = *in;
    }
    out[used] = 0;
}

/* Text of one view: "face=up spec=builtin:plasma x=0 y=0 w=1 h=1 opacity=1 blend=alpha z=0 enabled=1". */
static void sky_view_line(const struct sky_view *view, char *out, size_t size) {
    char spec[SKY_SPEC_SIZE * 3];
    sky_escape_spec(view->spec, spec, sizeof spec);
    snprintf(out, size, "face=%s spec=%s x=%.4f y=%.4f w=%.4f h=%.4f opacity=%.3f blend=%s z=%d enabled=%d",
             sky_face_names[view->face], spec, (double)view->x, (double)view->y, (double)view->w,
             (double)view->h, (double)view->opacity, view->blend ? "add" : "alpha", view->z, view->enabled);
}

/* Parse "key=value key=value ..." (values have no spaces) into a new view; returns its id or 0. */
static int sky_view_from_line(const char *line) {
    char copy[2600];
    snprintf(copy, sizeof copy, "%s", line);
    int face = -1;
    char spec[SKY_SPEC_SIZE] = "";
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
        else if (!strcmp(tokens[i][0], "spec")) sky_unescape_spec(tokens[i][1], spec, sizeof spec);
    }
    int id = sky_add_view(face, spec, 0, 0, 1, 1, 1);
    if (!id) return 0;
    struct sky_view *view = sky_find_view(id);
    for (int i = 0; i < count; i++)
        if (strcmp(tokens[i][0], "face") && strcmp(tokens[i][0], "spec")) sky_set_field_group(view, tokens[i][0], tokens[i][1]);
    return id;
}

/* ── GL ────────────────────────────────────────────────────────────── */

static const char sky_vertex_shader[] =
    "void main() { gl_TexCoord[0] = gl_MultiTexCoord0; gl_Position = gl_Vertex; }\n";

/* One fragment shader for every built-in source; uv is local to the view's rectangle, (0,0) top-left. */
static const char sky_fragment_shader[] =
    "uniform int kind; uniform float time; uniform int face; uniform vec4 color; uniform float opacity; uniform sampler2D tex; uniform sampler2D map; uniform int usemap; uniform sampler2D tex_u; uniform sampler2D tex_v; uniform int useyuv;\n"
    /* planar YUV 4:2:0 video (BT.709, limited range: what the streams are) or an RGBA texture */
    "vec3 video(vec2 p) {\n"
    " if (useyuv == 0) return texture2D(tex, p).rgb;\n"
    " float y = (texture2D(tex, p).r - 0.0627451) * 1.1643836;\n"
    " float u = (texture2D(tex_u, p).r - 0.5019608) * 1.1383929;\n"
    " float v = (texture2D(tex_v, p).r - 0.5019608) * 1.1383929;\n"
    " return clamp(vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u), 0.0, 1.0);\n"
    "}\n"
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
    " else if (kind == 6) { c = vec4(texture2D(tex, uv).rgb, 1.0); }\n"
    " else if (kind == 7) {\n"
    "  vec2 p = uv * 2.0 - 1.0; float u = p.x, w = -p.y; vec3 d;\n"
    "  if (face == 0) d = vec3(-u, -1.0, w); else if (face == 1) d = vec3(1.0, -u, w);\n"
    "  else if (face == 2) d = vec3(u, 1.0, w); else if (face == 3) d = vec3(-1.0, u, w);\n"
    "  else if (face == 4) d = vec3(-u, w, 1.0); else d = vec3(u, w, -1.0);\n"
    "  d = normalize(d); float lon = atan(d.x, d.y); float lat = asin(clamp(d.z, -1.0, 1.0));\n"
    "  vec2 st = vec2(0.5 + lon / 6.2831853, 0.5 - lat / 3.1415927);\n"
    "  if (usemap != 0) {\n"
    "   vec4 m = floor(texture2D(map, st) * 255.0 + 0.5);\n"
    "   if (m.r > 254.5 && m.g > 254.5 && m.b > 254.5 && m.a > 254.5) c = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "   else c = vec4(video(vec2((m.r * 256.0 + m.g) / 65535.0, (m.b * 256.0 + m.a) / 65535.0)), 1.0);\n"
    "  } else c = vec4(video(st), 1.0); }\n"
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
    SKY_FN(BlendFunc, "glBlendFunc"); SKY_FN(TexSubImage2D, "glTexSubImage2D");
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
    sky.u_tex = rs_gl.GetUniformLocation(sky.program, "tex");
    sky.u_map = rs_gl.GetUniformLocation(sky.program, "map");
    sky.u_usemap = rs_gl.GetUniformLocation(sky.program, "usemap");
    sky.u_tex_u = rs_gl.GetUniformLocation(sky.program, "tex_u");
    sky.u_tex_v = rs_gl.GetUniformLocation(sky.program, "tex_v");
    sky.u_useyuv = rs_gl.GetUniformLocation(sky.program, "useyuv");
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
    face->backup_width = width; face->backup_height = height;
    /* The engine can hold a face at a tiny size when the stock sky never shows it (the Matrix map's bottom is 4x4):
       a view drawn into that would be a blur, so such a face is re-specified at full size below. */
    int minimum = sky_face_size();
    int render_width = width < minimum ? minimum : width, render_height = height < minimum ? minimum : height;
    face->width = render_width; face->height = render_height;
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
    if (render_width == width && render_height == height) gl_gamma.TexImage2D(RS_TEXTURE_2D, 0, RS_RGBA8, width, height, 0, RS_RGBA, RS_UNSIGNED_BYTE, pixels);
    else gl_gamma.TexImage2D(RS_TEXTURE_2D, 0, RS_RGBA8, render_width, render_height, 0, RS_RGBA, RS_UNSIGNED_BYTE, NULL);
    free(pixels);
    gl_gamma.BindTexture(RS_TEXTURE_2D, (unsigned)previous);
    if (render_width != width || render_height != height)
        fprintf(stderr, "[t2_hook] Sky view face %d: GL texture %u, %dx%d (enlarged from the engine's %dx%d)\n", (int)(face - sky.faces), face->name,
                render_width, render_height, width, height);
    else fprintf(stderr, "[t2_hook] Sky view face %d: GL texture %u, %dx%d\n", (int)(face - sky.faces), face->name, width, height);
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
        if (sky.views[i].id && sky.views[i].enabled && sky.views[i].face == index &&
            (sky.views[i].source < 0 || sky.sources[sky.views[i].source].texture_ready)) views[count++] = &sky.views[i];
    qsort(views, (size_t)count, sizeof views[0], sky_compare_views);

    /* The face texture is the draw target; the backup is the read source for the fallback pixels. */
    rs_gl.BindFramebuffer(RS_READ_FRAMEBUFFER, sky.read_fbo);
    rs_gl.FramebufferTexture2D(RS_READ_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, face->backup, 0);
    rs_gl.BindFramebuffer(RS_DRAW_FRAMEBUFFER, sky.fbo);
    rs_gl.FramebufferTexture2D(RS_DRAW_FRAMEBUFFER, RS_COLOR_ATTACHMENT0, RS_TEXTURE_2D, face->name, 0);
    unsigned attachment = RS_COLOR_ATTACHMENT0;
    rs_gl.DrawBuffers(1, &attachment);
    if (rs_gl.CheckFramebufferStatus(RS_DRAW_FRAMEBUFFER) != RS_FRAMEBUFFER_COMPLETE) return;
    rs_gl.BlitFramebuffer(0, 0, face->backup_width, face->backup_height, 0, 0, face->width, face->height, RS_COLOR_BUFFER_BIT,
                          face->backup_width == face->width && face->backup_height == face->height ? RS_NEAREST : RS_LINEAR);   /* the original graphics (the fallback) */
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
            if (view->kind >= SKY_VIDEO) {
                rs_gl.Uniform1i(sky.u_tex, 0);
                gl_gamma.BindTexture(RS_TEXTURE_2D, view->source >= 0 ? sky.sources[view->source].texture : 0);
                unsigned map = view->source >= 0 ? sky.sources[view->source].map_texture : 0;
                rs_gl.Uniform1i(sky.u_usemap, map != 0);
                if (map) {
                    gl_gamma.ActiveTexture(GL_TEXTURE1_);
                    gl_gamma.BindTexture(RS_TEXTURE_2D, map);
                    rs_gl.Uniform1i(sky.u_map, 1);
                    gl_gamma.ActiveTexture(GL_TEXTURE0_);
                }
                const struct sky_source *video = view->source >= 0 ? &sky.sources[view->source] : NULL;
                rs_gl.Uniform1i(sky.u_useyuv, video && video->yuv && video->tex_u && video->tex_v);
                if (video && video->yuv && video->tex_u && video->tex_v) {
                    gl_gamma.ActiveTexture(GL_TEXTURE2_);
                    gl_gamma.BindTexture(RS_TEXTURE_2D, video->tex_u);
                    rs_gl.Uniform1i(sky.u_tex_u, 2);
                    gl_gamma.ActiveTexture(GL_TEXTURE3_);
                    gl_gamma.BindTexture(RS_TEXTURE_2D, video->tex_v);
                    rs_gl.Uniform1i(sky.u_tex_v, 3);
                    gl_gamma.ActiveTexture(GL_TEXTURE0_);
                }
            }
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

/* Views whose helper died or never produced a frame are removed, so their faces fall back to the original graphics. */
static void sky_drain_helper_errors(struct sky_source *source) {
    char text[4096];
    if (source->error_fd < 0) return;
    for (int rounds = 0; rounds < 16; rounds++) {   /* a chatty helper must not fill the pipe and stall */
        ssize_t n = read(source->error_fd, text, sizeof text - 1);
        if (n <= 0) break;
        if (!source->note[0]) {
            text[n] = 0;
            for (char *c = text; *c; c++) if (*c == '\n' || *c == '\r') { *c = 0; break; }
            snprintf(source->note, sizeof source->note, "%s", text);
        }
    }
}

static void sky_media_reap(void) {
    for (int i = 0; i < SKY_MAX_SOURCES; i++) {
        struct sky_source *source = &sky.sources[i];
        if (source->in_use) sky_drain_helper_errors(source);
        if (!source->in_use || !(source->failed || source->ended)) continue;
        if (source->failed && !source->failure_reported) {
            source->failure_reported = 1;
            fprintf(stderr, "[t2_hook] Sky views: %s failed: %s\n", source->key, source->error);
        } else if (!source->failed) {
            fprintf(stderr, "[t2_hook] Sky views: %s ended\n", source->key);
        }
        for (int v = 0; v < SKY_MAX_VIEWS; v++)
            if (sky.views[v].id && sky.views[v].source == i) sky_view_free(&sky.views[v]);   /* the last release destroys the source */
        if (sky.sources[i].in_use) sky_source_destroy(&sky.sources[i]);   /* a source nobody uses (cannot normally happen) */
    }
}

/* Put each media source's newest frame into its GL texture. */
static void sky_upload_sources(void) {
    for (int i = 0; i < SKY_MAX_SOURCES; i++) {
        struct sky_source *source = &sky.sources[i];
        unsigned seq = 0;
        if (!source->in_use) continue;
        unsigned char *frame = sky_source_take_frame(source, &seq);
        if (!frame) continue;
        gl_gamma.ActiveTexture(GL_TEXTURE0_);
        if (source->yuv) {
            /* three single-channel planes: Y at full size, U and V at half size in each direction */
            const int w = source->width, h = source->height;
            const unsigned char *planes[3] = {frame, frame + (size_t)w * h, frame + (size_t)w * h + (size_t)(w / 2) * (h / 2)};
            unsigned *ids[3] = {&source->texture, &source->tex_u, &source->tex_v};
            const int widths[3] = {w, w / 2, w / 2}, heights[3] = {h, h / 2, h / 2};
            const int first = source->texture == 0;
            rs_gl.PixelStorei(0x0CF5 /* GL_UNPACK_ALIGNMENT */, 1);
            for (int plane = 0; plane < 3; plane++) {
                if (first) {
                    gl_gamma.GenTextures(1, ids[plane]);
                    gl_gamma.BindTexture(RS_TEXTURE_2D, *ids[plane]);
                    gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
                    gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
                    gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_WRAP_S_, source->media.kind == SKY_EQUIRECT && !source->lookup ? 0x2901 : GL_CLAMP_TO_EDGE_);
                    gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
                    gl_gamma.TexImage2D(RS_TEXTURE_2D, 0, 0x8040 /* GL_LUMINANCE8 */, widths[plane], heights[plane], 0, 0x1909 /* GL_LUMINANCE */, GL_UNSIGNED_BYTE_, planes[plane]);
                } else {
                    gl_gamma.BindTexture(RS_TEXTURE_2D, *ids[plane]);
                    sky.TexSubImage2D(RS_TEXTURE_2D, 0, 0, 0, widths[plane], heights[plane], 0x1909, GL_UNSIGNED_BYTE_, planes[plane]);
                }
            }
            rs_gl.PixelStorei(0x0CF5, 4);
        } else if (!source->texture) {
            gl_gamma.GenTextures(1, &source->texture);
            gl_gamma.BindTexture(RS_TEXTURE_2D, source->texture);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_MIN_FILTER_, source->media.kind == SKY_EQUIRECT ? GL_LINEAR_ : 0x2703 /* LINEAR_MIPMAP_LINEAR */);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_WRAP_S_, source->media.kind == SKY_EQUIRECT && !source->lookup ? 0x2901 : GL_CLAMP_TO_EDGE_);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
            gl_gamma.TexImage2D(RS_TEXTURE_2D, 0, GL_RGBA8_, source->width, source->height, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, frame);
        } else {
            gl_gamma.BindTexture(RS_TEXTURE_2D, source->texture);
            sky.TexSubImage2D(RS_TEXTURE_2D, 0, 0, 0, source->width, source->height, GL_RGBA_, GL_UNSIGNED_BYTE_, frame);
        }
        if (source->lookup && !source->map_texture) {   /* YouTube mesh: the direction lookup, sampled NEAREST by the shader */
            gl_gamma.ActiveTexture(GL_TEXTURE1_);
            gl_gamma.GenTextures(1, &source->map_texture);
            gl_gamma.BindTexture(RS_TEXTURE_2D, source->map_texture);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_MIN_FILTER_, 0x2600 /* NEAREST */);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_MAG_FILTER_, 0x2600);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_WRAP_S_, 0x2901 /* REPEAT */);
            gl_gamma.TexParameteri(RS_TEXTURE_2D, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
            gl_gamma.TexImage2D(RS_TEXTURE_2D, 0, GL_RGBA8_, source->lookup_width, source->lookup_height, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, source->lookup);
            gl_gamma.ActiveTexture(GL_TEXTURE0_);
            free(source->lookup);
            source->lookup = NULL;   /* the texture has it now (map_texture marks the source as a mesh source) */
        }
        if (source->media.kind != SKY_EQUIRECT) rs_gl.GenerateMipmap(RS_TEXTURE_2D);   /* flat media is shown smaller than it is decoded */
        source->texture_ready = 1;
        sky_source_done_with_frame(source, seq);
    }
}

/* Called after each frame on the game thread. */
static void sky_views_tick(void) {
    sky_options_once();
    sky_media_reap();
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
    sky_upload_sources();
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
    for (int i = 0; i < SKY_MAX_SOURCES; i++) {   /* the texture ids are gone; the next frame uploads again */
        sky.sources[i].texture = 0;
        sky.sources[i].texture_ready = 0;
    }
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
    char spec[SKY_SPEC_SIZE];                     /* argv lives on the engine's string stack: keep our own copy */
    if (argc > 2) snprintf(spec, sizeof spec, "%s", argv[2]);
    int id = argc > 2 ? sky_add_view(face, spec, x, y, w, h, opacity) : 0;
    snprintf(result, sizeof result, "%d", id);
    return result;
}

static const char *sky_set_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 4) return "0";
    struct sky_view *view = sky_find_view(atoi(argv[1]));
    return view && sky_set_field_group(view, argv[2], argv[3]) ? "1" : "0";
}

static const char *sky_remove_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    struct sky_view *view = argc > 1 ? sky_find_view(atoi(argv[1])) : NULL;
    if (!view) return "0";
    sky_remove_view(view);
    return "1";
}

static const char *sky_clear_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    sky_views_clear();
    return "1";
}

static const char *sky_list_cb(void *obj, S32 argc, const char **argv) {
    static char result[32768];
    (void)obj; (void)argc; (void)argv;
    size_t used = 0;
    result[0] = 0;
    for (int i = 0; i < SKY_MAX_VIEWS; i++) {
        if (!sky.views[i].id) continue;
        char line[2400];
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
        char line[2400];
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
    sky_views_clear();
    char line[2600];
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

/* One record per view for the panel: id, face, spec, x, y, w, h, opacity, enabled, group (tab separated). */
static const char *sky_views_cb(void *obj, S32 argc, const char **argv) {
    static char result[32768];
    (void)obj; (void)argc; (void)argv;
    size_t used = 0;
    result[0] = 0;
    for (int i = 0; i < SKY_MAX_VIEWS; i++) {
        const struct sky_view *view = &sky.views[i];
        if (!view->id) continue;
        int written = snprintf(result + used, sizeof result - used, "%s%d\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.3f\t%d\t%d",
                               used ? "\n" : "", view->id, sky_face_names[view->face], view->spec, (double)view->x,
                               (double)view->y, (double)view->w, (double)view->h, (double)view->opacity,
                               view->enabled, view->group);
        if (written < 0 || used + (size_t)written >= sizeof result) break;
        used += (size_t)written;
    }
    return result;
}

static int sky_compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The saved layouts, one name per line. */
static const char *sky_layouts_cb(void *obj, S32 argc, const char **argv) {
    static char result[8192];
    (void)obj; (void)argc; (void)argv;
    char probe[900];
    result[0] = 0;
    if (!sky_layout_path("x", probe, sizeof probe, 0)) return result;
    char *slash = strrchr(probe, '/');
    if (!slash) return result;
    *slash = 0;
    DIR *directory = opendir(probe);
    if (!directory) return result;
    char *names[128];
    int count = 0;
    struct dirent *entry;
    while (count < 128 && (entry = readdir(directory)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".cfg") != 0) continue;
        char name[80];
        if (length - 4 >= sizeof name) continue;
        memcpy(name, entry->d_name, length - 4);
        name[length - 4] = 0;
        if (sky_layout_name_ok(name)) names[count++] = strdup(name);
    }
    closedir(directory);
    qsort(names, (size_t)count, sizeof names[0], sky_compare_names);
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        int written = snprintf(result + used, sizeof result - used, "%s%s", used ? "\n" : "", names[i]);
        free(names[i]);
        if (written > 0 && used + (size_t)written < sizeof result) used += (size_t)written;
    }
    return result;
}

static const char *sky_status_cb(void *obj, S32 argc, const char **argv) {
    static char result[64];
    (void)obj; (void)argc; (void)argv;
    int sources = 0;
    for (int i = 0; i < SKY_MAX_SOURCES; i++) sources += sky.sources[i].in_use;
    snprintf(result, sizeof result, "%d\t%d\t%d", sky.full_box_applied, sky_view_count(), sources);
    return result;
}

/* ── Keyboard capture for terminal views ───────────────────────────── */

/* SDL 1.2 key symbols and modifiers */
#define SKY_SDLK_BACKSPACE 8
#define SKY_SDLK_TAB 9
#define SKY_SDLK_RETURN 13
#define SKY_SDLK_ESCAPE 27
#define SKY_SDLK_DELETE 127
#define SKY_KMOD_SHIFT 0x0003
#define SKY_KMOD_CTRL 0x00C0
#define SKY_KMOD_ALT 0x0300

/* Terminal bytes for one key press; returns the length (0 when the key has no text). The unicode value from SDL
   is used when the game turned unicode translation on, otherwise a US layout is assumed for printable keys. */
static size_t sky_translate_key(unsigned sym, unsigned mod, unsigned unicode, unsigned char *out) {
    static const struct { unsigned sym; const char *bytes; } special[] = {
        {SKY_SDLK_RETURN, "\r"}, {SKY_SDLK_BACKSPACE, "\x7f"}, {SKY_SDLK_ESCAPE, "\x1b"}, {SKY_SDLK_DELETE, "\x1b[3~"},
        {273, "\x1b[A"}, {274, "\x1b[B"}, {275, "\x1b[C"}, {276, "\x1b[D"}, {277, "\x1b[2~"}, {278, "\x1b[H"},
        {279, "\x1b[F"}, {280, "\x1b[5~"}, {281, "\x1b[6~"}, {282, "\x1bOP"}, {283, "\x1bOQ"}, {284, "\x1bOR"},
        {285, "\x1bOS"}, {286, "\x1b[15~"}, {287, "\x1b[17~"}, {288, "\x1b[18~"}, {289, "\x1b[19~"},
        {290, "\x1b[20~"}, {291, "\x1b[21~"}, {292, "\x1b[23~"}, {293, "\x1b[24~"},
    };
    size_t length = 0;
    int alt = (mod & SKY_KMOD_ALT) != 0;
    if (sym == SKY_SDLK_TAB) {
        const char *bytes = (mod & SKY_KMOD_SHIFT) ? "\x1b[Z" : "\t";
        length = strlen(bytes);
        memcpy(out, bytes, length);
        return length;
    }
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++) {
        if (special[i].sym != sym) continue;
        if (alt) out[length++] = 0x1b;
        size_t n = strlen(special[i].bytes);
        memcpy(out + length, special[i].bytes, n);
        return length + n;
    }
    unsigned character = 0;
    if (unicode >= 0x20 && unicode != 0x7f) character = unicode;
    else if (sym >= 32 && sym <= 126) {                    /* a printable key with no unicode value: US layout */
        character = sym;
        if (sym >= 'a' && sym <= 'z' && (mod & SKY_KMOD_SHIFT)) character = sym - 32;
        else if (mod & SKY_KMOD_SHIFT) {
            static const char from[] = "1234567890-=[]\\;',./`", to[] = "!@#$%^&*()_+{}|:\"<>?~";
            const char *at = strchr(from, (int)sym);
            if (at) character = (unsigned char)to[at - from];
        }
    }
    if (!character) return 0;
    if (mod & SKY_KMOD_CTRL) {                              /* Ctrl+letter and the classic punctuation controls */
        unsigned lower = character >= 'A' && character <= 'Z' ? character + 32 : character;
        if (lower >= 'a' && lower <= 'z') character = lower - 'a' + 1;
        else if (character == '[') character = 0x1b;
        else if (character == '\\') character = 0x1c;
        else if (character == ']') character = 0x1d;
        else if (character == ' ' || character == '2') character = 0;
        else if (character == '/') character = 0x1f;
    }
    if (alt) out[length++] = 0x1b;
    if (character < 0x80) out[length++] = (unsigned char)character;
    else if (character < 0x800) { out[length++] = (unsigned char)(0xC0 | (character >> 6)); out[length++] = (unsigned char)(0x80 | (character & 0x3F)); }
    else { out[length++] = (unsigned char)(0xE0 | (character >> 12)); out[length++] = (unsigned char)(0x80 | ((character >> 6) & 0x3F)); out[length++] = (unsigned char)(0x80 | (character & 0x3F)); }
    return length;
}

/* One packet for the text helper: type, length, payload. A full socket just drops the key. */
static void sky_send_packet(struct sky_source *source, char type, const unsigned char *payload, size_t length) {
    unsigned char packet[2 + 255];
    if (source->input_fd < 0 || length > 255) return;
    packet[0] = (unsigned char)type;
    packet[1] = (unsigned char)length;
    memcpy(packet + 2, payload, length);
    (void)send(source->input_fd, packet, 2 + length, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void sky_set_capture(int source_index) {          /* -1 releases */
    if (sky.capture_source) {
        struct sky_source *old = &sky.sources[sky.capture_source - 1];
        if (old->in_use) sky_send_packet(old, 'f', (const unsigned char *)"0", 1);
    }
    sky.capture_source = 0;
    if (source_index < 0 || source_index >= SKY_MAX_SOURCES || !sky.sources[source_index].in_use ||
        sky.sources[source_index].media.term != 1) return;
    sky.capture_source = source_index + 1;
    sky_send_packet(&sky.sources[source_index], 'f', (const unsigned char *)"1", 1);
}

/* The first terminal on a visible view, for the hotkey. */
static int sky_first_terminal_source(void) {
    for (int i = 0; i < SKY_MAX_VIEWS; i++)
        if (sky.views[i].id && sky.views[i].enabled && sky.views[i].source >= 0 && sky.sources[sky.views[i].source].media.term == 1)
            return sky.views[i].source;
    return -1;
}

static const char *sky_capture_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    int id = argc > 1 ? atoi(argv[1]) : 0;
    if (id <= 0) { sky_set_capture(-1); return "0"; }
    struct sky_view *view = sky_find_view(id);
    if (!view || view->source < 0) return "0";
    sky_set_capture(view->source);
    return sky.capture_source ? "1" : "0";
}

/* Ctrl+End opens and closes the panel (an SDL 1.2 key event as in reshade_key_event: sym at 8, mod at 12). */
#define SKY_KEY_PANEL 279   /* SDLK_END */
static int sky_panel_installed;

#define SKY_KEY_CAPTURE 280   /* SDLK_PAGEUP: Ctrl+PageUp gives the keyboard to a terminal view and takes it back */

/* Called for every SDL event the game is about to see; returns 1 when the event is consumed here (every key
   while a terminal has the keyboard, except the hotkey that releases it). */
static int sky_key_event(const unsigned char *event) {
    if ((event[0] != 2 && event[0] != 3) || (!sky_panel_installed && !sky.capture_source)) return 0;   /* SDL_KEYDOWN / UP */
    unsigned sym, mod, unicode = 0;
    memcpy(&sym, event + 8, sizeof sym);
    memcpy(&mod, event + 12, sizeof mod);
    unicode = (unsigned)event[16] | ((unsigned)event[17] << 8);
    int down = event[0] == 2;
    if (sym == SKY_KEY_CAPTURE && (mod & SKY_KMOD_CTRL)) {
        if (down && Con__evaluate) {
            if (sky.capture_source) sky_set_capture(-1);
            else sky_set_capture(sky_first_terminal_source());
        }
        return 1;
    }
    if (sky.capture_source) {
        struct sky_source *source = &sky.sources[sky.capture_source - 1];
        unsigned char bytes[16];
        size_t length = down ? sky_translate_key(sym, mod, unicode, bytes) : 0;
        if (length) sky_send_packet(source, 'k', bytes, length);
        return 1;
    }
    if (down && sym == SKY_KEY_PANEL && (mod & SKY_KMOD_CTRL) && Con__evaluate)
        (void)Con__evaluate("t2launch_skyHotkey();", 0, "t2launch/sky_key.cs", 0);
    return 0;
}

/* The in-game panel, built on first use from stock Shell controls (the technique of the ReShade panel). */
static const char sky_panel_script[] =
    "$T2LSV::Rows = 5;\n"
    "function t2launch_svText(%name, %text, %x, %y, %w) {\n"
    " return new GuiTextCtrl(%name) {\n"
    "  profile = \"ShellTextProfile\"; horizSizing = \"right\"; vertSizing = \"bottom\";\n"
    "  position = %x SPC %y; extent = %w SPC 22; minExtent = \"8 8\"; visible = \"1\"; text = %text;\n"
    " };\n"
    "}\n"
    "function t2launch_svButton(%name, %text, %x, %y, %command) {\n"
    " return new ShellBitmapButton(%name) {\n"
    "  profile = \"ShellButtonProfile\"; horizSizing = \"right\"; vertSizing = \"top\";\n"
    "  position = %x SPC %y; extent = \"100 38\"; minExtent = \"32 38\"; visible = \"1\";\n"
    "  command = %command; text = %text; simpleStyle = \"0\";\n"
    " };\n"
    "}\n"
    "function t2launch_svPopup(%name, %x, %y, %w, %text) {\n"
    " return new ShellPopupMenu(%name) {\n"
    "  profile = \"ShellPopupProfile\"; horizSizing = \"right\"; vertSizing = \"bottom\";\n"
    "  position = %x SPC %y; extent = %w SPC 36; minExtent = \"49 36\"; visible = \"1\"; text = %text;\n"
    "  maxLength = \"255\"; maxPopupHeight = \"220\"; buttonBitmap = \"gui/shll_pulldown\";\n"
    "  rolloverBarBitmap = \"gui/shll_pulldownbar_rol\"; selectedBarBitmap = \"gui/shll_pulldownbar_act\"; noButtonStyle = \"0\";\n"
    " };\n"
    "}\n"
    "function t2launch_svEdit(%name, %x, %y, %w, %max) {\n"
    " return new ShellTextEditCtrl(%name) {\n"
    "  profile = \"NewTextEditProfile\"; horizSizing = \"right\"; vertSizing = \"bottom\";\n"
    "  position = %x SPC %y; extent = %w SPC 38; minExtent = \"32 38\"; visible = \"1\";\n"
    "  hideCursor = \"0\"; bypassHideCursor = \"0\"; helpTag = \"0\"; maxLength = %max; historySize = \"0\";\n"
    "  password = \"0\"; tabComplete = \"0\"; deniedSound = \"InputDeniedSound\"; sinkAllKeyEvents = \"0\";\n"
    " };\n"
    "}\n"
    "function t2launch_svBuild() {\n"
    " if (isObject(T2LSkyDlg)) return;\n"
    " new GuiControl(T2LSkyDlg) {\n"
    "  profile = \"DlgBackProfile\"; horizSizing = \"width\"; vertSizing = \"height\";\n"
    "  position = \"0 0\"; extent = \"640 480\"; minExtent = \"8 8\"; visible = \"1\";\n"
    " };\n"
    " %pane = new ShellPaneCtrl() {\n"
    "  profile = \"ShellDlgPaneProfile\"; horizSizing = \"center\"; vertSizing = \"center\";\n"
    "  position = \"10 10\"; extent = \"620 460\"; minExtent = \"48 92\"; visible = \"1\"; text = \"SKY VIEWS\"; noTitleBar = \"0\";\n"
    " };\n"
    " T2LSkyDlg.add(%pane);\n"
    " %pane.add(t2launch_svPopup(\"T2LSV_ViewMenu\", 24, 40, 360, \"Views\"));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_AddBtn\", \"ADD\", 394, 38, \"t2launch_svAdd();\"));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_RemoveBtn\", \"REMOVE\", 496, 38, \"t2launch_svRemove();\"));\n"
    " %pane.add(t2launch_svPopup(\"T2LSV_FaceMenu\", 24, 84, 120, \"Face\"));\n"
    " %pane.add(t2launch_svPopup(\"T2LSV_SourceMenu\", 154, 84, 200, \"Source\"));\n"
    " %t = new ShellToggleButton(T2LSV_OnTgl) {\n"
    "  profile = \"ShellRadioProfile\"; horizSizing = \"right\"; vertSizing = \"bottom\";\n"
    "  position = \"364 88\"; extent = \"120 30\"; minExtent = \"26 27\"; visible = \"1\"; text = \"VISIBLE\";\n"
    "  command = \"t2launch_svEnabled();\";\n"
    " };\n"
    " %pane.add(%t);\n"
    " %pane.add(t2launch_svButton(\"T2LSV_UpdateBtn\", \"UPDATE\", 496, 82, \"t2launch_svUpdate();\"));\n"
    " %pane.add(t2launch_svText(\"T2LSV_SrcLabel\", \"File, URL, #color or terminal profile\", 24, 126, 300));\n"
    " %pane.add(t2launch_svEdit(\"T2LSV_Source\", 24, 146, 460, 500));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_BrowseBtn\", \"BROWSE\", 494, 144, \"t2launch_svBrowse();\"));\n"
    " %names = \"X Y WIDTH HEIGHT OPACITY\";\n"
    " for (%r = 0; %r < $T2LSV::Rows; %r++) {\n"
    "  %y = 196 + %r * 30;\n"
    "  %pane.add(t2launch_svText(\"T2LSV_Label\" @ %r, getWord(%names, %r), 24, %y, 90));\n"
    "  %s = new ShellSliderCtrl(\"T2LSV_Slider\" @ %r) {\n"
    "   profile = \"ShellSliderProfile\"; horizSizing = \"right\"; vertSizing = \"bottom\";\n"
    "   position = \"120 \" @ (%y - 2); extent = \"380 24\"; minExtent = \"12 24\"; visible = \"1\";\n"
    "   variable = \"value\"; altCommand = \"t2launch_svSlider(\" @ %r @ \");\"; range = \"0.000000 1.000000\";\n"
    "   ticks = \"1000\"; value = \"0\"; usePlusMinus = \"1\";\n"
    "  };\n"
    "  %pane.add(%s);\n"
    "  %pane.add(t2launch_svText(\"T2LSV_Value\" @ %r, \"\", 510, %y, 80));\n"
    " }\n"
    " %pane.add(t2launch_svPopup(\"T2LSV_LayoutMenu\", 24, 350, 190, \"Layout\"));\n"
    " %pane.add(t2launch_svEdit(\"T2LSV_LayoutName\", 224, 349, 150, 40));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_SaveBtn\", \"SAVE\", 384, 348, \"t2launch_svSave();\"));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_LoadBtn\", \"LOAD\", 494, 348, \"t2launch_svLoad();\"));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_DoneBtn\", \"DONE\", 24, 400, \"t2launch_skyClose();\"));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_ClearBtn\", \"CLEAR\", 130, 400, \"t2launch_svClear();\"));\n"
    " %pane.add(t2launch_svButton(\"T2LSV_TypeBtn\", \"TYPE\", 236, 400, \"t2launch_svType();\"));\n"
    " %b = new ShellToggleButton(T2LSV_BoxTgl) {\n"
    "  profile = \"ShellRadioProfile\"; horizSizing = \"right\"; vertSizing = \"bottom\";\n"
    "  position = \"346 404\"; extent = \"150 30\"; minExtent = \"26 27\"; visible = \"1\"; text = \"FULL SKY BOX\";\n"
    "  command = \"t2launch_svBox();\";\n"
    " };\n"
    " %pane.add(%b);\n"
    " %pane.add(t2launch_svText(\"T2LSV_Status\", \"\", 24, 434, 560));\n"
    " T2LSV_FaceMenu.add(\"fr\", 1); T2LSV_FaceMenu.add(\"rt\", 2); T2LSV_FaceMenu.add(\"bk\", 3);\n"
    " T2LSV_FaceMenu.add(\"lf\", 4); T2LSV_FaceMenu.add(\"up\", 5); T2LSV_FaceMenu.add(\"dn\", 6); T2LSV_FaceMenu.add(\"all\", 7);\n"
    " $T2LSV::Kinds = \"plasma stars aurora gradient solid calibrate video image video360 terminal console\";\n"
    " for (%i = 0; %i < 11; %i++) T2LSV_SourceMenu.add(getWord($T2LSV::Kinds, %i), %i + 1);\n"
    "}\n"
    "function T2LSV_ViewMenu::onSelect(%this, %id, %text) {\n"
    " if ($T2LSV::Filling) return;\n"
    " $T2LSV::Sel = %id;\n"
    " t2launch_svLoadFields();\n"
    "}\n"
    "function T2LSV_LayoutMenu::onSelect(%this, %id, %text) {\n"
    " if ($T2LSV::Filling) return;\n"
    " T2LSV_LayoutName.setValue(%text);\n"
    "}\n"
    "function t2launch_svRecord(%id) {\n"
    " %list = t2launch_skyViews();\n"
    " for (%i = 0; %i < getRecordCount(%list); %i++) { %rec = getRecord(%list, %i); if (getField(%rec, 0) == %id) return %rec; }\n"
    " return \"\";\n"
    "}\n"
    "function t2launch_svFaceId(%name) {\n"
    " %names = \"fr rt bk lf up dn all\";\n"
    " for (%i = 0; %i < 7; %i++) if (getWord(%names, %i) $= %name) return %i + 1;\n"
    " return 1;\n"
    "}\n"
    "function t2launch_svSpec() {\n"
    " %kind = getWord($T2LSV::Kinds, T2LSV_SourceMenu.getSelected() - 1);\n"
    " %text = T2LSV_Source.getValue();\n"
    " if (%kind $= \"video\" || %kind $= \"image\" || %kind $= \"video360\") return %kind @ \":\" @ %text;\n"
    " if (%kind $= \"console\") return \"console\";\n"
    " if (%kind $= \"terminal\") return %text $= \"\" ? \"terminal\" : \"terminal:\" @ %text;\n"
    " if ((%kind $= \"solid\" || %kind $= \"gradient\") && getSubStr(%text, 0, 1) $= \"#\") return \"builtin:\" @ %kind @ \":\" @ %text;\n"
    " return \"builtin:\" @ %kind;\n"
    "}\n"
    "function t2launch_svLoadFields() {\n"
    " %rec = t2launch_svRecord($T2LSV::Sel);\n"
    " if (%rec $= \"\") return;\n"
    " $T2LSV::Filling = 1;\n"
    " T2LSV_FaceMenu.setSelected(t2launch_svFaceId(getField(%rec, 1)));\n"
    " %spec = getField(%rec, 2);\n"
    " %colon = strpos(%spec, \":\");\n"
    " if (%colon < 0) { %kind = %spec; %rest = \"\"; } else { %kind = getSubStr(%spec, 0, %colon); %rest = getSubStr(%spec, %colon + 1, 1000); }\n"
    " if (%kind $= \"builtin\") { %c2 = strpos(%rest, \":\"); if (%c2 >= 0) { %kind = getSubStr(%rest, 0, %c2); %rest = getSubStr(%rest, %c2 + 1, 1000); } else { %kind = %rest; %rest = \"\"; } }\n"
    " for (%i = 0; %i < 11; %i++) if (getWord($T2LSV::Kinds, %i) $= %kind) T2LSV_SourceMenu.setSelected(%i + 1);\n"
    " T2LSV_Source.setValue(%rest);\n"
    " T2LSV_OnTgl.setValue(getField(%rec, 8));\n"
    " for (%r = 0; %r < $T2LSV::Rows; %r++) {\n"
    "  %o = \"T2LSV_Slider\" @ %r; %v = \"T2LSV_Value\" @ %r;\n"
    "  %val = getField(%rec, 3 + %r);\n"
    "  %o.setValue(%val); %v.setText(mFloatLength(%val, 2));\n"
    " }\n"
    " $T2LSV::Filling = 0;\n"
    "}\n"
    "function t2launch_svRefresh() {\n"
    " if (!isObject(T2LSkyDlg)) return;\n"
    " $T2LSV::Filling = 1;\n"
    " T2LSV_ViewMenu.clear();\n"
    " %list = t2launch_skyViews();\n"
    " %count = getRecordCount(%list);\n"
    " %selected = 0;\n"
    " for (%i = 0; %i < %count; %i++) {\n"
    "  %rec = getRecord(%list, %i);\n"
    "  %spec = getField(%rec, 2);\n"
    "  if (strlen(%spec) > 40) %spec = \"...\" @ getSubStr(%spec, strlen(%spec) - 37, 40);\n"
    "  T2LSV_ViewMenu.add(getField(%rec, 0) @ \"  \" @ getField(%rec, 1) @ \"  \" @ %spec, getField(%rec, 0));\n"
    "  if (getField(%rec, 0) == $T2LSV::Sel) %selected = $T2LSV::Sel;\n"
    " }\n"
    " if (!%selected && %count) %selected = getField(getRecord(%list, 0), 0);\n"
    " $T2LSV::Sel = %selected;\n"
    " if (%selected) T2LSV_ViewMenu.setSelected(%selected);\n"
    " T2LSV_LayoutMenu.clear();\n"
    " %names = t2launch_skyLayouts();\n"
    " for (%i = 0; %i < getRecordCount(%names); %i++) T2LSV_LayoutMenu.add(getRecord(%names, %i), %i + 1);\n"
    " %status = t2launch_skyStatus();\n"
    " T2LSV_BoxTgl.setValue(getField(%status, 0));\n"
    " T2LSV_Status.setText(getField(%status, 1) @ \" view(s), \" @ getField(%status, 2) @ \" media\");\n"
    " $T2LSV::Sig = t2launch_svSignature();\n"
    " $T2LSV::Filling = 0;\n"
    " if (%selected) t2launch_svLoadFields();\n"
    "}\n"
    /* A view whose clip ends (or whose helper dies) is removed by the hook, not by the panel. While the panel is open it
       checks once a second whether the set of views changed (ids, faces, specs: not slider values, so a drag or an open
       dropdown is never disturbed) and refreshes itself if so. */
    "function t2launch_svSignature() {\n"
    " %list = t2launch_skyViews();\n"
    " %sig = \"\";\n"
    " for (%i = 0; %i < getRecordCount(%list); %i++) { %rec = getRecord(%list, %i); %sig = %sig @ getField(%rec, 0) @ \":\" @ getField(%rec, 1) @ \":\" @ getField(%rec, 2) @ \";\"; }\n"
    " return %sig @ getField(t2launch_skyStatus(), 2);\n"
    "}\n"
    "function t2launch_svPoll() {\n"
    " $T2LSV::PollOn = 0;\n"
    " if (!isObject(T2LSkyDlg) || !T2LSkyDlg.isAwake()) return;\n"
    " if (t2launch_svSignature() !$= $T2LSV::Sig) t2launch_svRefresh();\n"
    " $T2LSV::PollOn = 1;\n"
    " schedule(1000, 0, \"t2launch_svPoll\");\n"
    "}\n"
    "function t2launch_svAdd() {\n"
    " %face = getWord(\"fr rt bk lf up dn all\", T2LSV_FaceMenu.getSelected() - 1);\n"
    " %id = t2launch_skyViewAdd(%face, t2launch_svSpec(), 0, 0, 1, 1, 1);\n"
    " if (%id) $T2LSV::Sel = %id;\n"
    " else T2LSV_Status.setText(\"could not add (check face/source)\");\n"
    " if (%id) t2launch_svRefresh();\n"
    "}\n"
    "function t2launch_svUpdate() {\n"
    " if (!$T2LSV::Sel) return;\n"
    " %face = getWord(\"fr rt bk lf up dn all\", T2LSV_FaceMenu.getSelected() - 1);\n"
    " if (%face !$= \"all\") t2launch_skyViewSet($T2LSV::Sel, \"face\", %face);\n"
    " t2launch_skyViewSet($T2LSV::Sel, \"spec\", t2launch_svSpec());\n"
    " t2launch_svRefresh();\n"
    "}\n"
    "function t2launch_svRemove() {\n"
    " if (!$T2LSV::Sel) return;\n"
    " t2launch_skyViewRemove($T2LSV::Sel);\n"
    " $T2LSV::Sel = 0;\n"
    " t2launch_svRefresh();\n"
    "}\n"
    "function t2launch_svClear() {\n"
    " t2launch_skyViewClear();\n"
    " $T2LSV::Sel = 0;\n"
    " t2launch_svRefresh();\n"
    "}\n"
    "function t2launch_svEnabled() {\n"
    " if ($T2LSV::Filling || !$T2LSV::Sel) return;\n"
    " t2launch_skyViewSet($T2LSV::Sel, \"enabled\", T2LSV_OnTgl.getValue() ? 1 : 0);\n"
    "}\n"
    "function t2launch_svSlider(%r) {\n"
    " if ($T2LSV::Filling || !$T2LSV::Sel) return;\n"
    " %o = \"T2LSV_Slider\" @ %r; %v = \"T2LSV_Value\" @ %r;\n"
    " %val = %o.getValue();\n"
    " t2launch_skyViewSet($T2LSV::Sel, getWord(\"x y w h opacity\", %r), %val);\n"
    " %v.setText(mFloatLength(%val, 2));\n"
    "}\n"
    "function t2launch_svType() {\n"
    " %rec = t2launch_svRecord($T2LSV::Sel);\n"
    " %spec = getField(%rec, 2);\n"
    " if (getSubStr(%spec, 0, 8) !$= \"terminal\") { T2LSV_Status.setText(\"select a terminal view first\"); return; }\n"
    " t2launch_skyClose();\n"
    " t2launch_skyKeyCapture($T2LSV::Sel);\n"
    "}\n"
    "function t2launch_svIsMedia(%file) {\n"
    " %kind = getWord($T2LSV::Kinds, T2LSV_SourceMenu.getSelected() - 1);\n"
    " %ext = strlwr(fileExt(%file)) @ \" \";\n"
    " if (strlen(%ext) < 3) return 0;\n"
    " if (%kind $= \"image\") return strpos(\".png .jpg .jpeg .gif .bmp .webp \", %ext) >= 0;\n"
    " return strpos(\".mp4 .mkv .webm .avi .mov .m4v .ogv .flv .mpg .mpeg .gif .ts \", %ext) >= 0;\n"
    "}\n"
    "function t2launch_svBrowse() {\n"
    " %kind = getWord($T2LSV::Kinds, T2LSV_SourceMenu.getSelected() - 1);\n"
    " if (%kind !$= \"video\" && %kind !$= \"image\" && %kind !$= \"video360\") { T2LSV_Status.setText(\"pick a video, image or video360 source first\"); return; }\n"
    " ShellGetLoadFilename(\"CHOOSE MEDIA\", \"media/*.*\", \"t2launch_svIsMedia\", \"t2launch_svPicked\");\n"
    "}\n"
    "function t2launch_svPicked(%name) {\n"
    " if (%name $= \"\") return;\n"
    " %file = \"\";\n"
    " for (%f = findFirstFile(\"media/\" @ %name @ \".*\"); %f !$= \"\"; %f = findNextFile(\"media/\" @ %name @ \".*\"))\n"
    "  if (%file $= \"\" && t2launch_svIsMedia(%f)) %file = %f;\n"
    " if (%file $= \"\") { T2LSV_Status.setText(\"could not find \" @ %name); return; }\n"
    " T2LSV_Source.setValue(%file);\n"
    " T2LSV_Status.setText(\"chosen: \" @ %file @ \" (press ADD or UPDATE)\");\n"
    "}\n"
    "function t2launch_svBox() {\n"
    " if ($T2LSV::Filling) return;\n"
    " t2launch_skyFullBox(T2LSV_BoxTgl.getValue() ? 1 : 0);\n"
    "}\n"
    "function t2launch_svSave() {\n"
    " %name = T2LSV_LayoutName.getValue();\n"
    " if (%name $= \"\") return;\n"
    " T2LSV_Status.setText(t2launch_skyLayoutSave(%name) ? \"saved \" @ %name : \"save failed\");\n"
    " schedule(1200, 0, \"t2launch_svRefresh\");\n"
    "}\n"
    "function t2launch_svLoad() {\n"
    " %name = T2LSV_LayoutName.getValue();\n"
    " if (%name $= \"\") return;\n"
    " %n = t2launch_skyLayoutLoad(%name);\n"
    " $T2LSV::Sel = 0;\n"
    " t2launch_svRefresh();\n"
    " T2LSV_Status.setText(%n $= \"0\" ? \"no such layout\" : %n @ \" view(s) loaded\");\n"
    "}\n"
    "function t2launch_skyOpen() {\n"
    " t2launch_svBuild();\n"
    " t2launch_svRefresh();\n"
    " Canvas.pushDialog(T2LSkyDlg);\n"
    " if (!$T2LSV::PollOn) { $T2LSV::PollOn = 1; schedule(1000, 0, \"t2launch_svPoll\"); }\n"
    "}\n"
    "function t2launch_skyClose() {\n"
    " if (isObject(T2LSkyDlg)) Canvas.popDialog(T2LSkyDlg);\n"
    "}\n"
    "function t2launch_skyHotkey() {\n"
    " if (isObject(T2LSkyDlg) && T2LSkyDlg.isAwake()) t2launch_skyClose();\n"
    " else t2launch_skyOpen();\n"
    "}\n";

static void sky_install_panel(void) {
    if (sky_panel_installed || !Con__evaluate) return;
    Con__evaluate(sky_panel_script, 0, "t2launch/sky_panel.cs", 0);
    sky_panel_installed = 1;
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
    Con__addMethodS(NULL, "t2launch_skyKeyCapture", sky_capture_cb, "t2launch_skyKeyCapture(viewId) - give the keyboard to that terminal view; 0 releases it", 2, 2);
    Con__addMethodS(NULL, "t2launch_skyViews", sky_views_cb, "t2launch_skyViews() - id, face, spec, x, y, w, h, opacity, enabled, group records", 1, 1);
    Con__addMethodS(NULL, "t2launch_skyLayouts", sky_layouts_cb, "t2launch_skyLayouts() - saved layout names", 1, 1);
    Con__addMethodS(NULL, "t2launch_skyStatus", sky_status_cb, "t2launch_skyStatus() - full box, views, media sources", 1, 1);
    Con__addMethodS(NULL, "t2launch_skyFullBox", sky_full_box_cb,
        "t2launch_skyFullBox(on) - draw the lower half of the sky walls too (maps without terrain)", 2, 2);
}
