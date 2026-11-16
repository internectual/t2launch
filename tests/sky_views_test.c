/* Sky views: spec/face/colour parsing, view and layout round trips, the face flip table, and the full-box patch
 * (applied and reverted on synthetic pages, bytes checked against the real client).
 * gcc -m32 -O2 -std=c99 -mstackrealign -fPIE -pie tests/sky_views_test.c -o sky_views_test -pthread -ldl
 */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>
#include <sys/wait.h>

static int log_mode_seen = -1;
static void log_mode_stub(int mode) { log_mode_seen = mode; }
static const char *evaluated;
static const char *hotkey_probe(const char *code, unsigned char flag, const char *file, unsigned char other) {
    (void)flag; (void)file; (void)other;
    evaluated = code;
    return "";
}

static unsigned char *map_site(uintptr_t address, const unsigned char *bytes, size_t length, size_t *size_out) {
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t page = address & ~((uintptr_t)size - 1);
    unsigned char *mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    memcpy((void *)address, bytes, length);
    *size_out = size;
    return mapping;
}

int main(void) {
    /* faces */
    assert(sky_parse_face("fr") == 0 && sky_parse_face("RT") == 1 && sky_parse_face("back") == 2);
    assert(sky_parse_face("left") == 3 && sky_parse_face("top") == 4 && sky_parse_face("down") == 5);
    assert(sky_parse_face("3") == 3 && sky_parse_face("6") == -1 && sky_parse_face("") == -1 && sky_parse_face(NULL) == -1);

    /* colours and specs */
    float color[4];
    assert(sky_parse_color("#ff8000", color) && color[0] == 1.0f && color[3] == 1.0f && color[2] == 0.0f);
    assert(sky_parse_color("#00000080", color) && color[3] > 0.49f && color[3] < 0.51f);
    assert(!sky_parse_color("ff8000", color) && !sky_parse_color("#ff80", color) && !sky_parse_color("#gg0000", color));
    assert(sky_parse_spec("builtin:plasma", color) == SKY_PLASMA);
    assert(sky_parse_spec("builtin:SOLID:#102030", color) == SKY_SOLID && color[2] > 0.18f && color[2] < 0.2f);
    assert(sky_parse_spec("builtin:nonsense", color) < 0 && sky_parse_spec("clip:/x.mp4", color) < 0 && sky_parse_spec(NULL, color) < 0);

    /* media specs */
    struct sky_media media;
    assert(sky_parse_media("video:/tmp/a.mp4", &media) && media.kind == SKY_VIDEO && !media.still);
    assert(sky_parse_media("image:./x.png", &media) && media.kind == SKY_VIDEO && media.still);
    assert(sky_parse_media("video360:/tmp/a.mp4", &media) && media.kind == SKY_EQUIRECT && !strcmp(media.layout, "e"));
    assert(sky_parse_media("video360:https://h.example/v.mp4@c3x2", &media) && !strcmp(media.layout, "c3x2") &&
           !strcmp(media.source, "https://h.example/v.mp4"));
    assert(sky_parse_media("video360:/tmp/a.mp4@e-sbs", &media) && !strcmp(media.layout, "e-sbs"));
    assert(sky_parse_media("video:rtsp://cam.local/stream", &media));
    assert(!sky_parse_media("video:", &media) && !sky_parse_media("video:-i", &media) && !sky_parse_media("video:file:/x", &media));
    assert(!sky_parse_media("video:concat:a|b", &media) && !sky_parse_media("video:pipe:0", &media) && !sky_parse_media("video:ftp://x/y", &media));
    assert(!sky_parse_media("video:/tmp/a\nb", &media) && !sky_parse_media("movie:/tmp/a", &media) && !sky_parse_media(NULL, &media));
    assert(sky_parse_spec("video:/tmp/a.mp4", color) == SKY_VIDEO && sky_parse_spec("video360:/a", color) == SKY_EQUIRECT);
    assert(sky_parse_spec("video:-x", color) < 0);

    /* helper command lines */
    char *argv[40];
    assert(sky_parse_media("video:/tmp/my clip.mp4", &media));
    int argc = sky_build_video_argv(&media, 1280, 720, 24, 1, argv, 40);
    assert(argc > 10 && !strcmp(argv[0], "ffmpeg"));
    int saw_input = 0, saw_loop = 0, saw_re = 0, saw_filter = 0;
    for (int i = 0; i < argc; i++) {
        saw_input |= !strcmp(argv[i], "file:/tmp/my clip.mp4");
        saw_loop |= !strcmp(argv[i], "-stream_loop");
        saw_re |= !strcmp(argv[i], "-re");
        saw_filter |= strstr(argv[i], "scale=1280:720") && strstr(argv[i], "fps=24") && strstr(argv[i], "format=rgba");
    }
    assert(saw_input && saw_loop && saw_re && saw_filter && !strcmp(argv[argc - 1], "pipe:1"));
    sky_free_argv(argv);
    assert(sky_parse_media("video360:/tmp/c.mp4@c3x2", &media));
    argc = sky_build_video_argv(&media, 2048, 1024, 30, 0, argv, 40);
    int saw_v360 = 0; saw_loop = 0;
    for (int i = 0; i < argc; i++) {
        saw_v360 |= strstr(argv[i], "v360=input=c3x2:output=e") && strstr(argv[i], "w=2048:h=1024");
        saw_loop |= !strcmp(argv[i], "-stream_loop");
    }
    assert(saw_v360 && !saw_loop);
    sky_free_argv(argv);
    assert(sky_parse_media("video:rtsp://cam/s", &media));
    argc = sky_build_video_argv(&media, 1280, 720, 24, 1, argv, 40);
    saw_re = 0;
    for (int i = 0; i < argc; i++) saw_re |= !strcmp(argv[i], "-re");
    assert(!saw_re);                                   /* live streams are not paced */
    sky_free_argv(argv);
    assert(sky_parse_media("video:/tmp/a.mp4", &media));
    argc = sky_build_audio_argv(&media, 1, 0.5, "alsa_output.pci-0000_00_1f.3.analog-stereo", argv, 40);
    int saw_pulse = 0, saw_vn = 0, saw_device = 0, saw_volume = 0;
    for (int i = 0; i < argc; i++) {
        saw_pulse |= !strcmp(argv[i], "pulse"); saw_vn |= !strcmp(argv[i], "-vn");
        saw_device |= !strcmp(argv[i], "alsa_output.pci-0000_00_1f.3.analog-stereo"); saw_volume |= !strcmp(argv[i], "volume=0.50");
    }
    assert(saw_pulse && saw_vn && saw_device && saw_volume);
    sky_free_argv(argv);
    argc = sky_build_audio_argv(&media, 0, 1.0, "bad device;rm", argv, 40);      /* an odd device name is dropped */
    for (int i = 0; i < argc; i++) assert(strcmp(argv[i], "-device") != 0);
    sky_free_argv(argv);

    /* a fake ffmpeg that records its arguments and writes five 1280x720 RGBA frames */
    char directory[] = "/tmp/sky_views_test_XXXXXX";
    assert(mkdtemp(directory));
    char fake[256], record[256];
    snprintf(fake, sizeof fake, "%s/ffmpeg", directory);
    snprintf(record, sizeof record, "%s/args", directory);
    FILE *script = fopen(fake, "w");
    assert(script);
    fprintf(script, "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> %s/args.$$\n"
                    "case \" $* \" in *\" -vn \"*) sleep 30; exit 0;; esac\n"
                    "i=0; while [ $i -lt 5 ]; do head -c 3686400 /dev/zero; sleep 0.05; i=$((i+1)); done\n", directory);
    fclose(script);
    chmod(fake, 0755);
    setenv("T2LAUNCH_SKY_FFMPEG", fake, 1);
    memset(&sky, 0, sizeof sky);
    for (int i = 0; i < SKY_MAX_SOURCES; i++) sky.sources[i].fd = -1;

    /* a flat video: a source starts, frames arrive, it ends on its own */
    int video = sky_add_view(2, "video:/tmp/clip.mp4", 0.1f, 0.1f, 0.5f, 0.5f, 1);
    assert(video);
    struct sky_view *flat = sky_find_view(video);
    assert(flat->kind == SKY_VIDEO && flat->source >= 0 && sky.sources[flat->source].in_use);
    struct sky_source *source = &sky.sources[flat->source];
    assert(source->width == 1280 && source->height == 720 && source->audio_pid > 0);
    for (int i = 0; i < 100 && !source->ended; i++) usleep(50000);
    assert(source->ended && !source->failed && source->seq >= 1);
    unsigned seq = 0;
    unsigned char *frame = sky_source_take_frame(source, &seq);
    assert(frame && seq == source->seq && source->reading >= 0);
    sky_source_done_with_frame(source, seq);
    assert(sky_source_take_frame(source, &seq) == NULL && source->reading == -1);
    sky_media_reap();                                                  /* ended: the view goes, the helper is gone */
    assert(sky_view_count() == 0 && !source->in_use);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);       /* no stray children, audio included */

    /* two views of one spec share a source; the last release stops it */
    int first = sky_add_view(0, "video:/tmp/clip.mp4", 0, 0, 1, 1, 1), second = sky_add_view(1, "video:/tmp/clip.mp4", 0, 0, 1, 1, 1);
    assert(first && second && sky_find_view(first)->source == sky_find_view(second)->source);
    int shared = sky_find_view(first)->source;
    assert(sky.sources[shared].refs == 2);
    sky_remove_view(sky_find_view(first));
    assert(sky.sources[shared].in_use && sky.sources[shared].refs == 1);
    sky_remove_view(sky_find_view(second));
    assert(!sky.sources[shared].in_use);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    /* a 360 view: six faces, one source, one group; edits and removal apply to the group */
    assert(sky_add_view(SKY_ALL, "builtin:plasma", 0, 0, 1, 1, 1) == 0);                 /* only 360 video can cover the sky */
    assert(sky_add_view(0, "video360:/tmp/eq.mp4", 0, 0, 1, 1, 1) == 0);                 /* and it needs all the faces */
    int sphere = sky_add_view(SKY_ALL, "video360:/tmp/eq.mp4", 0, 0, 1, 1, 0.8f);
    assert(sphere && sky_view_count() == 6);
    int members = 0, src = -1;
    for (int i = 0; i < SKY_MAX_VIEWS; i++) {
        if (!sky.views[i].id) continue;
        members++;
        assert(sky.views[i].group == sphere && sky.views[i].kind == SKY_EQUIRECT);
        if (src < 0) src = sky.views[i].source;
        assert(sky.views[i].source == src);
    }
    assert(members == 6 && sky.sources[src].refs == 6);
    assert(sky_set_field_group(sky_find_view(sphere), "opacity", "0.3"));
    for (int i = 0; i < SKY_MAX_VIEWS; i++) if (sky.views[i].id) assert(fabsf(sky.views[i].opacity - 0.3f) < 1e-4f);
    assert(!sky_set_field(sky_find_view(sphere), "face", "up") && !sky_set_field(sky_find_view(sphere), "spec", "builtin:stars"));
    sky_remove_view(sky_find_view(sphere));
    assert(sky_view_count() == 0 && !sky.sources[src].in_use);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    /* a spec that cannot run: the view exists until the helper dies without a frame, then it is reaped */
    setenv("T2LAUNCH_SKY_FFMPEG", "/nonexistent/ffmpeg", 1);
    int broken = sky_add_view(3, "video:/tmp/clip.mp4", 0, 0, 1, 1, 1);
    assert(broken);
    source = &sky.sources[sky_find_view(broken)->source];
    for (int i = 0; i < 100 && !source->failed; i++) usleep(50000);
    assert(source->failed && source->error[0]);
    sky_media_reap();
    assert(sky_view_count() == 0 && !sky.sources[0].in_use);
    setenv("T2LAUNCH_SKY_FFMPEG", fake, 1);

    /* sky.media off: no helper starts */
    for (size_t i = 0; i < HOOK_OPTION_COUNT; i++)
        if (!strcmp(hook_options[i].key, "sky.media")) { snprintf(hook_options[i].value, sizeof hook_options[i].value, "0"); hook_options[i].set = 1; }
    assert(sky_add_view(0, "video:/tmp/clip.mp4", 0, 0, 1, 1, 1) == 0);
    for (size_t i = 0; i < HOOK_OPTION_COUNT; i++)
        if (!strcmp(hook_options[i].key, "sky.media")) { snprintf(hook_options[i].value, sizeof hook_options[i].value, "1"); }
    unlink(record);
    char command[400];
    snprintf(command, sizeof command, "rm -rf %s", directory);
    assert(system(command) == 0);
    unsetenv("T2LAUNCH_SKY_FFMPEG");

    /* views: clamping, fields, lines */
    memset(&sky, 0, sizeof sky);
    int id = sky_add_view(4, "builtin:stars", -1, 0.25f, 3, 0, 2);
    assert(id == 1);
    struct sky_view *view = sky_find_view(id);
    assert(view && view->face == 4 && view->x == 0 && view->w == 1 && view->h > 0 && view->opacity == 1);
    assert(sky_add_view(9, "builtin:stars", 0, 0, 1, 1, 1) == 0);       /* bad face */
    assert(sky_add_view(0, "builtin:zzz", 0, 0, 1, 1, 1) == 0);         /* bad spec */
    assert(sky_set_field(view, "opacity", "0.5") && view->opacity == 0.5f);
    assert(sky_set_field(view, "blend", "add") && view->blend == 1);
    assert(sky_set_field(view, "face", "lf") && view->face == 3);
    assert(sky_set_field(view, "spec", "builtin:aurora") && view->kind == SKY_AURORA);
    assert(!sky_set_field(view, "spec", "builtin:bad") && view->kind == SKY_AURORA);
    assert(!sky_set_field(view, "nonsense", "1"));
    char line[400];
    sky_view_line(view, line, sizeof line);
    assert(strstr(line, "face=lf spec=builtin:aurora") && strstr(line, "blend=add"));
    int copy = sky_view_from_line(line);
    assert(copy && copy != id);
    struct sky_view *twin = sky_find_view(copy);
    assert(twin->face == 3 && twin->kind == SKY_AURORA && twin->blend == 1 && twin->opacity == 0.5f && twin->enabled);
    assert(sky_view_count() == 2);
    for (int i = 1; i < SKY_MAX_VIEWS; i++) assert(sky_add_view(0, "builtin:solid", 0, 0, 1, 1, 1) != 0 || sky_view_count() == SKY_MAX_VIEWS);
    assert(sky_add_view(0, "builtin:solid", 0, 0, 1, 1, 1) == 0);       /* the table is full */

    /* the panel's data: views, layouts, status, and the Ctrl+End hotkey */
    sky_views_clear();
    int panel_view = sky_add_view(4, "builtin:aurora", 0.1f, 0.2f, 0.5f, 0.6f, 0.75f);
    const char *records = sky_views_cb(NULL, 1, NULL);
    char expected[200];
    snprintf(expected, sizeof expected, "%d\tup\tbuiltin:aurora\t0.1000\t0.2000\t0.5000\t0.6000\t0.750\t1\t0", panel_view);
    assert(!strcmp(records, expected));
    assert(sky_add_view(1, "builtin:stars", 0, 0, 1, 1, 1) != 0 && strchr(sky_views_cb(NULL, 1, NULL), '\n'));
    assert(!strncmp(sky_status_cb(NULL, 1, NULL), "0\t2\t0", 5));
    char layout_root[] = "/tmp/sky_layouts_XXXXXX";
    assert(mkdtemp(layout_root));
    setenv("XDG_DATA_HOME", layout_root, 1);
    const char *save_b[2] = {"t2launch_skyLayoutSave", "bravo"}, *save_a[2] = {"t2launch_skyLayoutSave", "alpha"};
    assert(!strcmp(sky_layout_save_cb(NULL, 2, save_b), "1") && !strcmp(sky_layout_save_cb(NULL, 2, save_a), "1"));
    assert(!strcmp(sky_layouts_cb(NULL, 1, NULL), "alpha\nbravo"));
    const char *load_a[2] = {"t2launch_skyLayoutLoad", "alpha"};
    assert(!strcmp(sky_layout_load_cb(NULL, 2, load_a), "2") && sky_view_count() == 2);
    char cleanup[200];
    snprintf(cleanup, sizeof cleanup, "rm -rf %s", layout_root);
    assert(system(cleanup) == 0);
    unsetenv("XDG_DATA_HOME");
    sky_views_clear();

    evaluated = NULL;
    Con__evaluate = hotkey_probe;
    unsigned char key[16] = {2};
    unsigned sym = SKY_KEY_PANEL, mod = 0x40;
    memcpy(key + 8, &sym, 4); memcpy(key + 12, &mod, 4);
    sky_key_event(key);
    assert(!evaluated);                                   /* not installed yet */
    sky_panel_installed = 1;
    sky_key_event(key);
    assert(evaluated && !strcmp(evaluated, "t2launch_skyHotkey();"));
    evaluated = NULL;
    mod = 0; memcpy(key + 12, &mod, 4);
    sky_key_event(key);
    assert(!evaluated);                                   /* End without Ctrl */
    mod = 0x40; sym = 278; memcpy(key + 8, &sym, 4); memcpy(key + 12, &mod, 4);
    sky_key_event(key);
    assert(!evaluated);                                   /* Ctrl+Home belongs to ReShade */
    key[0] = 3; sym = SKY_KEY_PANEL; memcpy(key + 8, &sym, 4);
    sky_key_event(key);
    assert(!evaluated);                                   /* key up */
    assert(strstr(sky_panel_script, "t2launch_skyHotkey") && strstr(sky_panel_script, "T2LSkyDlg"));

    /* terminal and console specs: only a profile name, never a command */
    struct sky_media text;
    assert(sky_parse_media("terminal", &text) && text.term == 1 && !strcmp(text.layout, "default"));
    assert(sky_parse_media("terminal:htop-2", &text) && text.term == 1 && !strcmp(text.layout, "htop-2"));
    assert(sky_parse_media("console", &text) && text.term == 2);
    assert(!sky_parse_media("terminal:", &text) && !sky_parse_media("terminal:rm -rf ~", &text) &&
           !sky_parse_media("terminal:../x", &text) && !sky_parse_media("terminal:a;b", &text) &&
           !sky_parse_media("terminal:waytoolongprofilename", &text) && !sky_parse_media("terminals", &text));
    assert(sky_parse_spec("terminal", color) == SKY_VIDEO && sky_parse_spec("console", color) == SKY_VIDEO);

    /* spaces in a spec survive a layout line */
    char escaped[200], plain[200];
    sky_escape_spec("video:/tmp/my 100% clip.mp4", escaped, sizeof escaped);
    assert(!strcmp(escaped, "video:/tmp/my%20100%25%20clip.mp4"));
    sky_unescape_spec(escaped, plain, sizeof plain);
    assert(!strcmp(plain, "video:/tmp/my 100% clip.mp4"));
    sky_unescape_spec("a%zzb%2", plain, sizeof plain);
    assert(!strcmp(plain, "a%zzb%2"));                                   /* not an escape: kept as is */

    /* key_sym translation */
    unsigned char bytes[16];
    assert(sky_translate_key('a', 0, 0, bytes) == 1 && bytes[0] == 'a');
    assert(sky_translate_key('a', 1, 0, bytes) == 1 && bytes[0] == 'A');
    assert(sky_translate_key('1', 1, 0, bytes) == 1 && bytes[0] == '!');
    assert(sky_translate_key('-', 1, 0, bytes) == 1 && bytes[0] == '_');
    assert(sky_translate_key('c', 0x40, 0, bytes) == 1 && bytes[0] == 3);
    assert(sky_translate_key('[', 0x40, 0, bytes) == 1 && bytes[0] == 0x1b);
    assert(sky_translate_key('x', 0x100, 0, bytes) == 2 && bytes[0] == 0x1b && bytes[1] == 'x');
    assert(sky_translate_key(13, 0, 13, bytes) == 1 && bytes[0] == '\r');
    assert(sky_translate_key(8, 0, 8, bytes) == 1 && bytes[0] == 0x7f);
    assert(sky_translate_key(9, 0, 9, bytes) == 1 && bytes[0] == '\t');
    assert(sky_translate_key(9, 1, 9, bytes) == 3 && !memcmp(bytes, "\x1b[Z", 3));
    assert(sky_translate_key(273, 0, 0, bytes) == 3 && !memcmp(bytes, "\x1b[A", 3));
    assert(sky_translate_key(282, 0, 0, bytes) == 3 && !memcmp(bytes, "\x1bOP", 3));
    assert(sky_translate_key(127, 0, 127, bytes) == 4 && !memcmp(bytes, "\x1b[3~", 4));
    assert(sky_translate_key('e', 0, 0xE9, bytes) == 2 && bytes[0] == 0xC3 && bytes[1] == 0xA9);
    assert(sky_translate_key(304, 0, 0, bytes) == 0);                      /* a modifier alone */

    /* a text helper: fake "python" (sh) running a fake helper that records its arguments and keystrokes */
    char tdir[] = "/tmp/sky_text_test_XXXXXX";
    assert(mkdtemp(tdir));
    char helper[300];
    snprintf(helper, sizeof helper, "%s/helper.py", tdir);
    FILE *fake_helper = fopen(helper, "w");
    assert(fake_helper);
    fprintf(fake_helper, "printf '%%s\n' \"$@\" > %s/argv.rec\nhead -c 3145728 /dev/zero\ncat >> %s/keys.rec\n", tdir, tdir);
    fclose(fake_helper);
    setenv("T2LAUNCH_SKY_TERMINAL_HELPER", helper, 1);
    setenv("T2LAUNCH_SKY_PYTHON", "/bin/sh", 1);
    memset(&sky, 0, sizeof sky);
    for (int i = 0; i < SKY_MAX_SOURCES; i++) sky.sources[i].fd = sky.sources[i].input_fd = -1;
    registered = 1;
    sky_log_mode_fn = log_mode_stub;
    Con__evaluate = hotkey_probe;
    evaluated = NULL;
    int shell_view = sky_add_view(0, "terminal:work", 0, 0, 1, 1, 1);
    assert(shell_view);
    struct sky_view *shell = sky_find_view(shell_view);
    struct sky_source *terminal = &sky.sources[shell->source];
    assert(terminal->media.term == 1 && terminal->width == 1024 && terminal->height == 768 && terminal->input_fd >= 0);
    assert(terminal->audio_pid <= 0);
    for (int i = 0; i < 100 && terminal->seq < 1; i++) usleep(50000);
    assert(terminal->seq >= 1);
    assert(sky_first_terminal_source() == shell->source);
    sky_set_capture(shell->source);
    assert(sky.capture_source == shell->source + 1);
    unsigned char press[24] = {2};
    unsigned key_sym = 'h', no_mod = 0;
    memcpy(press + 8, &key_sym, 4); memcpy(press + 12, &no_mod, 4);
    assert(sky_key_event(press) == 1);                                      /* consumed by the terminal */
    press[0] = 3;
    assert(sky_key_event(press) == 1);                                      /* key_sym up too */
    press[0] = 2; key_sym = 280; no_mod = 0x40;
    memcpy(press + 8, &key_sym, 4); memcpy(press + 12, &no_mod, 4);
    assert(sky_key_event(press) == 1 && sky.capture_source == 0);           /* Ctrl+PageUp releases */
    press[0] = 2; key_sym = 'q'; no_mod = 0;
    memcpy(press + 8, &key_sym, 4); memcpy(press + 12, &no_mod, 4);
    assert(sky_key_event(press) == 0);                                      /* the game gets keys again */
    key_sym = 280; no_mod = 0x40;
    memcpy(press + 8, &key_sym, 4); memcpy(press + 12, &no_mod, 4);
    assert(sky_key_event(press) == 1 && sky.capture_source == shell->source + 1);   /* and it takes them back */
    usleep(300000);
    sky_remove_view(shell);
    assert(sky.capture_source == 0 && !terminal->in_use);                   /* removing the view ends the capture */
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    char rec_path[400], content[600];
    snprintf(rec_path, sizeof rec_path, "%s/keys.rec", tdir);
    FILE *record_file = fopen(rec_path, "rb");
    assert(record_file);
    size_t got = fread(content, 1, sizeof content, record_file);
    fclose(record_file);
    static const char expected_keys[] = "f\x01" "1" "k\x01" "h" "f\x01" "0" "f\x01" "1";
    assert(got == sizeof expected_keys - 1 && !memcmp(content, expected_keys, got));
    snprintf(rec_path, sizeof rec_path, "%s/argv.rec", tdir);
    record_file = fopen(rec_path, "r");
    assert(record_file);
    got = fread(content, 1, sizeof content - 1, record_file);
    fclose(record_file);
    content[got] = 0;
    assert(strstr(content, "--mode\nshell\n--profile\nwork\n") && strstr(content, "--width\n1024\n--height\n768\n"));

    /* the console view tails <cwd>/console.log and switches the engine's logging on and off with the view */
    int console_view = sky_add_view(5, "console", 0, 0, 1, 1, 1);
    assert(console_view && log_mode_seen == 1 && !evaluated);          /* logging on, and no script was run for it */
    for (int i = 0; i < 100 && sky.sources[sky_find_view(console_view)->source].seq < 1; i++) usleep(50000);
    snprintf(rec_path, sizeof rec_path, "%s/argv.rec", tdir);
    record_file = fopen(rec_path, "r");
    got = fread(content, 1, sizeof content - 1, record_file);
    fclose(record_file);
    content[got] = 0;
    assert(strstr(content, "--mode\nconsole\n--tail\n") && strstr(content, "/console.log\n"));
    assert(sky_view_count() == 1);
    int console_source = sky_find_view(console_view)->source;
    sky_set_capture(console_source);
    assert(sky.capture_source == 0);                                        /* a console is read only */
    sky_remove_view(sky_find_view(console_view));
    assert(log_mode_seen == 0 && !evaluated);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    /* no helper: the add fails cleanly */
    unsetenv("T2LAUNCH_SKY_TERMINAL_HELPER");
    assert(sky_add_view(0, "terminal", 0, 0, 1, 1, 1) == 0);
    registered = 0;
    sky_log_mode_fn = sky_log_mode_native;
    char remove_text[400];
    snprintf(remove_text, sizeof remove_text, "rm -rf %s", tdir);
    assert(system(remove_text) == 0);
    unsetenv("T2LAUNCH_SKY_PYTHON");

    /* layout names stay inside the folder */
    assert(sky_layout_name_ok("matrix") && sky_layout_name_ok("a-b_1"));
    assert(!sky_layout_name_ok("../x") && !sky_layout_name_ok("a b") && !sky_layout_name_ok("") && !sky_layout_name_ok("a/b"));

    /* the texture mapping flips: walls and the top mirrored in x, the bottom in y */
    for (int i = 0; i < 5; i++) assert(sky_face_flip_x[i] == 1 && sky_face_flip_y[i] == 0);
    assert(sky_face_flip_x[5] == 0 && sky_face_flip_y[5] == 1);

    /* the full-box patch: both stores change together and revert; wrong bytes are refused */
    size_t size;
    /* both stores are on the same page (0x08221000) */
    unsigned char *page = map_site(SKY_FULL_BOX_ADDRESS_A, sky_full_box_a_original, sizeof sky_full_box_a_original, &size);
    memcpy((void *)SKY_FULL_BOX_ADDRESS_B, sky_full_box_b_original, sizeof sky_full_box_b_original);
    unsigned char *site_a = (unsigned char *)SKY_FULL_BOX_ADDRESS_A, *site_b = (unsigned char *)SKY_FULL_BOX_ADDRESS_B;
    static const struct game_target client_target = {.executable = "tribes2.dynamic", .dedicated = 0};
    target = &client_target;
    site_b[0] = 0x00;                                                   /* an unexpected byte at the second site */
    assert(mprotect(page, size, PROT_READ | PROT_EXEC) == 0);
    assert(!sky_set_full_box(1));
    assert(memcmp(site_a, sky_full_box_a_original, sizeof sky_full_box_a_original) == 0);   /* nothing written */
    assert(mprotect(page, size, PROT_READ | PROT_WRITE) == 0);
    memcpy(site_b, sky_full_box_b_original, sizeof sky_full_box_b_original);
    assert(mprotect(page, size, PROT_READ | PROT_EXEC) == 0);
    assert(sky_set_full_box(1));
    assert(memcmp(site_a, sky_full_box_a_patched, sizeof sky_full_box_a_patched) == 0);
    assert(memcmp(site_b, sky_full_box_b_patched, sizeof sky_full_box_b_patched) == 0);
    assert(sky_set_full_box(1));                                        /* already on: nothing to do */
    assert(sky_set_full_box(0));
    assert(memcmp(site_a, sky_full_box_a_original, sizeof sky_full_box_a_original) == 0);
    assert(memcmp(site_b, sky_full_box_b_original, sizeof sky_full_box_b_original) == 0);

    /* the same bytes in the real client, when it is installed */
    int checked = 0;
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/t2-linux/tribes2.dynamic", home ? home : "");
    FILE *client = fopen(path, "rb");
    if (client) {
        unsigned char found[16];
        assert(fseek(client, (long)(SKY_FULL_BOX_ADDRESS_A - 0x08048000), SEEK_SET) == 0 && fread(found, 1, 10, client) == 10);
        assert(memcmp(found, sky_full_box_a_original, 10) == 0);
        assert(fseek(client, (long)(SKY_FULL_BOX_ADDRESS_B - 0x08048000), SEEK_SET) == 0 && fread(found, 1, 12, client) == 12);
        assert(memcmp(found, sky_full_box_b_original, 12) == 0);
        assert(fseek(client, (long)(SKY_SET_LOG_MODE_CALLBACK - 0x08048000), SEEK_SET) == 0 && fread(found, 1, sizeof sky_set_log_mode_prologue, client) == sizeof sky_set_log_mode_prologue);
        assert(memcmp(found, sky_set_log_mode_prologue, sizeof sky_set_log_mode_prologue) == 0);
        fclose(client);
        checked = 1;
    }
    printf("sky views: specs, views, layouts, face flips, full-box patch and %d real-client check passed\n", checked);
    return 0;
}
