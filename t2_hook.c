#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <math.h>
#include <ctype.h>

typedef int S32;

/* next_symbol(name), but through glibc's own dlsym. glibc resolves
   RTLD_NEXT from its caller; an overlay that interposes dlsym (MangoHud's
   shim does) becomes that caller, so the lookup would start after the
   overlay instead of after the hook and miss what lies between, such as the
   game's SDL. dlvsym, which those overlays leave alone, finds the real one. */
static void *next_symbol(const char *name) {
    static void *(*libc_dlsym)(void *, const char *);
    if (!libc_dlsym) {
        static const char *const owners[] = {"libc.so.6", "libdl.so.2"};
        static const char *const versions[] = {"GLIBC_2.34", "GLIBC_2.0"};
        for (size_t i = 0; i < 2 && !libc_dlsym; i++) {
            void *library = dlopen(owners[i], RTLD_NOW | RTLD_NOLOAD);
            for (size_t v = 0; library && v < 2 && !libc_dlsym; v++)
                *(void **)&libc_dlsym = dlvsym(library, "dlsym", versions[v]);
        }
        if (!libc_dlsym) libc_dlsym = dlsym;
    }
    return libc_dlsym(RTLD_NEXT, name);
}

typedef void (*con_addMethodS_t)(const char *nsName, const char *name,
    const char *(*cb)(void *obj, S32 argc, const char **argv),
    const char *usage, S32 minArgs, S32 maxArgs);
typedef const char *(*con_evaluate_t)(const char *code, unsigned char echo,
    const char *fileName, unsigned char noCalls);
typedef void (*sdl_gl_swap_buffers_t)(void);
typedef void *(*sdl_set_video_mode_t)(int width, int height, int bpp, uint32_t flags);
typedef struct { int16_t x, y; uint16_t w, h; } sdl_rect_t;
typedef sdl_rect_t **(*sdl_list_modes_t)(void *format, uint32_t flags);

#define SDL_FULLSCREEN_FLAG 0x80000000u
#define SDL_NOFRAME_FLAG 0x00000020u
#define SDL_OPENGL_FLAG 0x00000002u

/* ── Executables ─────────────────────────────────────────────────────────
 * The hook runs in the Loki #25034 client (tribes2.dynamic) and in its
 * separate dedicated-server build (tribes2d.dynamic), which places the same
 * engine code at other addresses. The server has no SDL, GL, browser or
 * account login, so the client-only patches below keep their client
 * addresses and run only in the client. */
struct game_target {
    const char *executable;
    const char *description;
    int dedicated;
    uintptr_t con_add_method;
    uintptr_t con_evaluate;
    uintptr_t string_table;          /* StringTable global; set once the console exists */
    uintptr_t real_milliseconds;     /* Platform::getRealMilliseconds */
    uintptr_t won_bootstrap[3];      /* "tribes2.mN.sierra.com:15104" */
    uintptr_t won_verifier;          /* 817-byte DER slot read by the WON global constructor */
    uintptr_t net_rate_variable;     /* $pref::Net::PacketRateToClient */
    uintptr_t net_size_variable;     /* $pref::Net::PacketSize */
    uintptr_t net_rate_compare[3];   /* see install_net_packet_limits() */
    uintptr_t build_version_helper;  /* getT2VersionNumber() return stub */
    uintptr_t master_list_version_flag; /* client list request's OR 0x80 */
    uintptr_t demo_tag_writer;       /* recorder's 32-bit .rec header tag */
    uintptr_t demo_tag_compare;      /* playback's strict .rec tag compare */
    uintptr_t demo_tag_helper;       /* getDemoVersion() result */
};

static const struct game_target game_targets[] = {
    {"tribes2.dynamic", "client", 0, 0x080E85C0, 0x080E88F4, 0x08493DEC, 0x081CFA44,
     {0x084846B1, 0x084846CD, 0x084846E9}, 0x0849A860, 0x08495CEC, 0x08495CF0,
     {0x081F8EAE, 0x081FB382, 0x081FB683}, 0x08309168, 0x0823E9B3,
     0x081F9AF8, 0x081F9EEC, 0x081FB978},
    {"tribes2d.dynamic", "dedicated server", 1, 0x080E74B0, 0x080E77E4, 0x0848902C, 0x081CD014,
     {0x08479751, 0x0847976D, 0x08479789}, 0x0848F520, 0x0848ADCC, 0x0848ADD0,
     {0x081EDB4E, 0x081F0022, 0x081F0323}, 0x082FDE08, 0, 0, 0, 0},
};
#define GAME_TARGET_COUNT (sizeof(game_targets) / sizeof(game_targets[0]))
static const struct game_target *target = &game_targets[0];

static con_addMethodS_t Con__addMethodS = NULL;
static con_evaluate_t Con__evaluate = (con_evaluate_t)0x080E88F4;
static sdl_gl_swap_buffers_t real_sdl_gl_swap_buffers = NULL;
static sdl_set_video_mode_t real_sdl_set_video_mode = NULL;
static sdl_list_modes_t real_sdl_list_modes = NULL;
static pthread_t game_main_thread;
static volatile int registered = 0;
static volatile int game_main_thread_known = 0;
static int register_hook_commands(void);
static int register_early_compat_commands(void);
static int install_console_evaluate_gate(void);
static void update_review_model_diagnostic(int active);
static int review_model_hud_seen;
static uint32_t review_model_hud_id;
static int set_swap_interval(int interval);
static volatile int vsync_requested = -1;   /* -1: never set by the game */
static volatile int vsync_reapply = 0;
static volatile int profile_browser_wake_seen = 0;
static volatile int core_runtime_installed = 0;
static volatile int tribesnext_runtime_installed = 0;
static volatile int shell_audio_setup_done = 0;
static int voice_capture_fix_done = 0; /* voice capture re-initialised after the launch audio setup */
static volatile int player_pane_override_installed = 0;
static volatile int filter_max_bots_override_installed = 0;
static volatile int net_options_override_installed = 0;
static volatile int depot_downloads_installed = 0;
static volatile int vsync_support_installed = 0;
static volatile int discord_presence_installed = 0;
static char tribesnext_certificate[20001];
static char tribesnext_modulus[2049];
static char tribesnext_exponent[2049];
static char tribesnext_private_exponent[4097];
static char tribesnext_username[128];
static char tribesnext_password[512];
static time_t hook_started_at = 0;
static time_t last_shell_audio_setup = 0;
static int sdl_gl_swap_hook_reported = 0;
static volatile unsigned int video_mode_repaint_frames = 0;
static volatile int xrandr_mode_list_reported = 0;

/* ── Hook options ─────────────────────────────────────────────────────────
 * The launcher writes one key=value per line to the file named by
 * T2LAUNCH_HOOK_CONFIG; t2options.py declares the same keys. A value comes
 * from that file, else the option's legacy environment variable, else its
 * default. Keys the hook does not know yet are kept for hook_option(). */
struct hook_option {
    const char *key;
    const char *fallback;
    const char *legacy_env;   /* NULL when there is none */
    int legacy_inverted;      /* the variable's "1" turns the option off */
    char value[256];
    int set;
};
static struct hook_option hook_options[] = {
    {"hook.console_commands", "1", "T2LAUNCH_DISABLE_HOOK_COMMANDS", 1, "", 0},
    {"hook.mem_patch", "warn", NULL, 0, "", 0},
    {"hook.tribesnext", "1", NULL, 0, "", 0},
    {"hook.tribes_tab", "1", NULL, 0, "", 0},
    {"video.full_redraw", "1", "T2LAUNCH_FULL_REDRAW", 0, "", 0},
    {"video.xrandr_modes", "1", NULL, 0, "", 0},
    {"audio.launch_reset", "1", NULL, 0, "", 0},
    {"runtime.thread_join_guard", "1", NULL, 0, "", 0},
    {"runtime.monotonic_clock", "1", NULL, 0, "", 0},
    {"browser.many_bots", "1", NULL, 0, "", 0},
    {"browser.web_links", "1", NULL, 0, "", 0},
    {"net.packet_limits", "1", NULL, 0, "", 0},
    {"net.build_identifier", "25034", NULL, 0, "", 0},
    {"demo.protocol_tag", "3342340", NULL, 0, "", 0},
    {"telemetry.shazbot", "0", NULL, 0, "", 0},
    {"telemetry.shazbot_socket", "/tmp/shazbot.sock", NULL, 0, "", 0},
    {"telemetry.shazbot_packet_capture", "0", NULL, 0, "", 0},
    {"telemetry.shazbot_notification_socket", "/tmp/shazbot-notify.sock", NULL, 0, "", 0},
    {"telemetry.shazbot_actions", "0", NULL, 0, "", 0},
    {"telemetry.shazbot_action_socket", "/tmp/shazbot-action.sock", NULL, 0, "", 0},
    {"telemetry.shazbot_command_labels", "0", NULL, 0, "", 0},
    {"telemetry.shazbot_auto_weapon_slot_test", "0", NULL, 0, "", 0},
    {"telemetry.shazbot_weapon_slot_test_script", "", NULL, 0, "", 0},
    {"video.fps_limit", "0", NULL, 0, "", 0},
    {"video.borderless", "0", NULL, 0, "", 0},
    {"video.vsync", "1", NULL, 0, "", 0},
    {"video.gamma", "1", NULL, 0, "", 0},
    {"video.render_scale", "off", NULL, 0, "", 0},
    {"video.sharpen", "0.0", NULL, 0, "", 0},
    {"video.msaa", "0", NULL, 0, "", 0},
    {"video.disable_locked_arrays", "0", NULL, 0, "", 0},
    {"net.depot_downloads", "0", NULL, 0, "", 0},
    {"net.native_auth", "0", NULL, 0, "", 0},
    {"discord.presence", "0", NULL, 0, "", 0},
    {"discord.app_id", "488764170460725249", NULL, 0, "", 0},
    {"ui.hud_get_value", "1", NULL, 0, "", 0},
    {"ui.vertical_fill", "1", NULL, 0, "", 0},
    {"ui.bar_flip", "1", NULL, 0, "", 0},
    {"ui.observer_iff", "0", NULL, 0, "", 0},
    {"ui.lan_join_anywhere", "0", NULL, 0, "", 0},
    {"ui.no_fog", "0", NULL, 0, "", 0},
    {"ui.visible_distance", "0", NULL, 0, "", 0},
    {"ui.no_whiteout", "0", NULL, 0, "", 0},
    {"render.datablock_rules", "", NULL, 0, "", 0},
    {"render.object_rules", "", NULL, 0, "", 0},
    {"review.cheat_view", "0", NULL, 0, "", 0},
    {"review.replace_models", "0", "T2LAUNCH_REVIEW_MODELS", 0, "", 0},
    {"review.ffi", "1", NULL, 0, "", 0},
    {"review.no_cloak", "1", NULL, 0, "", 0},
    {"review.autobeacon", "1", NULL, 0, "", 0},
    {"review.extended_range", "1", NULL, 0, "", 0},
    {"review.global_beacons", "1", NULL, 0, "", 0},
    {"review.shortcuts", "1", NULL, 0, "", 0},
    {"ui.fov_range", "0", NULL, 0, "", 0},
    {"ui.client_datablocks", "0", NULL, 0, "", 0},
    {"server.tribesnext_auth", "1", NULL, 0, "", 0},
    {"server.public_address", "", NULL, 0, "", 0},
    {"security.safe_soundpacks", "1", NULL, 0, "", 0},
    {"security.max_soundpack_kbytes", "8192", NULL, 0, "", 0},
};
#define HOOK_OPTION_COUNT (sizeof(hook_options) / sizeof(hook_options[0]))
#define HOOK_EXTRA_OPTION_LIMIT 64
static struct { char key[64]; char value[256]; } hook_extra_options[HOOK_EXTRA_OPTION_LIMIT];
static int hook_extra_option_count;
static pthread_once_t hook_options_once = PTHREAD_ONCE_INIT;

static char *trim_space(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                      text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = 0;
    return text;
}

static void store_hook_option(const char *key, const char *value) {
    for (size_t i = 0; i < HOOK_OPTION_COUNT; i++) {
        if (strcmp(hook_options[i].key, key) == 0) {
            snprintf(hook_options[i].value, sizeof(hook_options[i].value), "%s", value);
            hook_options[i].set = 1;
            return;
        }
    }
    for (int i = 0; i < hook_extra_option_count; i++) {
        if (strcmp(hook_extra_options[i].key, key) == 0) {
            snprintf(hook_extra_options[i].value, sizeof(hook_extra_options[i].value), "%s", value);
            return;
        }
    }
    if (hook_extra_option_count < HOOK_EXTRA_OPTION_LIMIT && strlen(key) < sizeof(hook_extra_options[0].key)) {
        snprintf(hook_extra_options[hook_extra_option_count].key, sizeof(hook_extra_options[0].key), "%s", key);
        snprintf(hook_extra_options[hook_extra_option_count].value, sizeof(hook_extra_options[0].value),
                 "%s", value);
        hook_extra_option_count++;
    }
}

static void load_hook_options(void) {
    for (size_t i = 0; i < HOOK_OPTION_COUNT; i++) {
        const char *legacy = hook_options[i].legacy_env ? getenv(hook_options[i].legacy_env) : NULL;
        if (!legacy || !legacy[0]) continue;
        if (hook_options[i].legacy_inverted)
            store_hook_option(hook_options[i].key, strcmp(legacy, "1") == 0 ? "0" : "1");
        else
            store_hook_option(hook_options[i].key, legacy);
    }
    const char *path = getenv("T2LAUNCH_HOOK_CONFIG");
    if (!path || !path[0]) return;
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "[t2_hook] Hook config unavailable: %s\n", strerror(errno));
        return;
    }
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), file)) {
        char *text = trim_space(line);
        char *equals = strchr(text, '=');
        if (!*text || *text == '#' || !equals) continue;
        *equals = 0;
        char *key = trim_space(text);
        if (*key) {
            store_hook_option(key, trim_space(equals + 1));
            count++;
        }
    }
    fclose(file);
    fprintf(stderr, "[t2_hook] Loaded %d hook options\n", count);
}

static const char *hook_option_str(const char *key) {
    pthread_once(&hook_options_once, load_hook_options);
    for (size_t i = 0; i < HOOK_OPTION_COUNT; i++)
        if (strcmp(hook_options[i].key, key) == 0)
            return hook_options[i].set ? hook_options[i].value : hook_options[i].fallback;
    for (int i = 0; i < hook_extra_option_count; i++)
        if (strcmp(hook_extra_options[i].key, key) == 0) return hook_extra_options[i].value;
    return NULL;
}

static int hook_option_bool(const char *key) {
    const char *value = hook_option_str(key);
    while (value && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) value++;
    if (value && !*value) {
        /* A blank bool entry means "use this option's declared default";
           explicit off is represented by 0/false/no/off. */
        for (size_t i = 0; i < HOOK_OPTION_COUNT; i++) {
            if (strcmp(hook_options[i].key, key) == 0) {
                value = hook_options[i].fallback;
                break;
            }
        }
    }
    while (value && (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n')) value++;
    return value && (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "on") == 0 || strcasecmp(value, "yes") == 0);
}

/* The native server-browser IP column formats "IP:<address>:<port>" and then
 * strips the prefix with strcpy(dst, dst + 3).  That overlapping copy is
 * undefined and corrupts the address with the 32-bit libc used by the game.
 * Preserve normal strcpy behavior while making overlapping copies well-defined.
 */
char *strcpy(char *destination, const char *source)
{
    char *result = destination;
    size_t length = 0;
    while (source[length] != '\0')
        ++length;
    ++length;

    uintptr_t destination_address = (uintptr_t)destination;
    uintptr_t source_address = (uintptr_t)source;
    if ((destination_address <= source_address &&
         source_address - destination_address < length) ||
        (source_address < destination_address &&
         destination_address - source_address < length))
        return memmove(destination, source, length);

    volatile unsigned char *output = (volatile unsigned char *)destination;
    for (size_t i = 0; i < length; ++i)
        output[i] = (unsigned char)source[i];
    return result;
}

/* Loki's gotoWebPage() formats $LOKI_BROWSER with the URL and passes it to
 * system(), or looks for netscape/mozilla and gives up when neither exists.
 * The hook points LOKI_BROWSER at a marker that system() below turns into
 * xdg-open with the URL as one argument, so no shell ever parses the URL. */
#define WEB_LINK_MARKER "T2LAUNCH_OPEN_URL "
extern char **environ;

/* The URL from a marker command, or NULL when it is not one we open. */
static const char *web_link_url(const char *command) {
    size_t marker = strlen(WEB_LINK_MARKER);
    if (!command || strncmp(command, WEB_LINK_MARKER, marker) != 0) return NULL;
    const char *url = command + marker;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0 &&
        strncmp(url, "ftp://", 6) != 0)
        return NULL;
    for (const unsigned char *c = (const unsigned char *)url; *c; c++)
        if (*c <= ' ' || *c == 0x7f || *c == '\'' || *c == '"' || *c == '\\' || *c == '`')
            return NULL;
    return url;
}

/* The environment without the game's loader settings, for 64-bit helpers.
   The caller frees the array, not its strings. */
static char **helper_environment(void) {
    size_t count = 0;
    while (environ[count]) count++;
    char **env = malloc((count + 1) * sizeof(*env));
    if (!env) return NULL;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(environ[i], "LD_PRELOAD=", 11) != 0 && strncmp(environ[i], "LD_AUDIT=", 9) != 0 &&
            strncmp(environ[i], "LD_LIBRARY_PATH=", 16) != 0)
            env[kept++] = environ[i];
    env[kept] = NULL;
    return env;
}

static int open_web_link(const char *url) {
    /* xdg-open and the browser are 64-bit; leave the game's loader settings behind. */
    char **env = helper_environment();
    if (!env) return -1;
    char *argv[] = {"xdg-open", (char *)url, NULL};
    pid_t child = fork();
    if (child == 0) {
        setsid();
        if (fork() == 0) {
            execvpe("xdg-open", argv, env);
            _exit(127);
        }
        _exit(0);
    }
    free(env);
    if (child < 0) return -1;
    waitpid(child, NULL, 0);
    fprintf(stderr, "[t2_hook] Opened web link with xdg-open: %s\n", url);
    return 0;
}

int system(const char *command) {
    static int (*real_system)(const char *);
    const char *url = web_link_url(command);
    if (url) return open_web_link(url);
    if (command && strncmp(command, WEB_LINK_MARKER, strlen(WEB_LINK_MARKER)) == 0) {
        fprintf(stderr, "[t2_hook] Refused web link with an unsupported or unsafe URL\n");
        return -1;
    }
    if (!real_system) real_system = (int (*)(const char *))next_symbol("system");
    return real_system ? real_system(command) : -1;
}

/* Installed whenever the hook runs: helpers the hook itself evaluates, the
 * server-browser repaint, and a provider entry for an empty audio menu. */
static const char core_runtime_script[] =
    "function t2launch_launchGuiReady() {\n"
    " if (isObject(LaunchGui)) return \"1\";\n"
    " return \"0\";\n"
    "}\n"
    "function t2launch_hudDemoRadarEnabled() {"
    " if (!$T2Launch::HudDemo::Radar || !isObject(PlayGui) || !PlayGui.isAwake()) return \"0\";"
    " if (!isObject(navHud) || !navHud.isAwake() || !navHud.visible) return \"0\";"
    " return \"1\";"
    "}\n"
    "function t2launch_hudDemoCompassEnabled() {"
    " if (!isObject(PlayGui) || !PlayGui.isAwake() || !isObject(ServerConnection)) return \"\";"
    " %mode = $T2Launch::HudDemo::CompassMode;"
    " if (%mode !$= \"horizontal\" && %mode !$= \"both\") return \"\";"
    " return \"1\";"
    "}\n"
    "function t2launch_skyRotationSpeed() { return $pref::skyRotationSpeed; }\n"
    "function t2launch_audioState() {\n"
    " return $Audio::initialized @ \"|\" @ $pref::Audio::activeDriver @ \"|\" @ $pref::Audio::drivers;\n"
    "}\n"
    "function t2launch_refreshServerBrowser() {\n"
    " if (isObject(Canvas) && isObject(GMJ_Browser) && GMJ_Browser.isAwake()) Canvas.repaint();\n"
    " schedule(500, 0, \"t2launch_refreshServerBrowser\");\n"
    "}\n"
    "function t2launch_audioMenuPoll() {\n"
    " if ($Audio::initialized && isObject(OptionsDlg) && OptionsDlg.isAwake() &&\n"
    "     isObject(OP_AudioProviderMenu) && OP_AudioProviderMenu.size() == 0) {\n"
    "  OP_AudioProviderMenu.add(\"OpenAL\", 0); OP_AudioProviderMenu.setSelected(0);\n"
    " }\n"
    " schedule(100, 0, \"t2launch_audioMenuPoll\");\n"
    "}\n"
    "t2launch_refreshServerBrowser();\n"
    "t2launch_audioMenuPoll();\n";

/* Mirror the stock HUD's weapon inventory/selection events. Torque does not
   replicate Player dynamic inventory fields to client ghosts; the HUD messages
   are the client-visible record of weaponSlot changes. */
static const char shazbot_hud_telemetry_script[] =
    "package T2LaunchShazbotHudTelemetry {\n"
    " function clientCmdSetWeaponsHudClearAll() {\n"
    "  shazbot_hud_weapon_clear();\n"
    "  Parent::clientCmdSetWeaponsHudClearAll();\n"
    " }\n"
     " function clientCmdSetWeaponsHudItem(%slot, %ammoAmount, %addItem) {\n"
     "  %item = \"\";\n"
     "  if (%slot >= 0 && %slot < $WeaponsHudCount) {\n"
     "   %item = $WeaponNames[%slot];\n"
     "   if (%item $= \"\") %item = $WeaponsHudData[%slot, itemDataName];\n"
     "  }\n"
     "  shazbot_hud_weapon_item(%slot, %item, %addItem);\n"
    "  Parent::clientCmdSetWeaponsHudItem(%slot, %ammoAmount, %addItem);\n"
    " }\n"
    " function clientCmdSetWeaponsHudActive(%slot, %ret, %vis) {\n"
    "  %item = \"\";\n"
    "  if (%slot >= 0 && %slot < $WeaponsHudCount)\n"
    "   %item = $WeaponsHudData[%slot, itemDataName];\n"
    "  shazbot_hud_weapon_active(%item);\n"
    "  Parent::clientCmdSetWeaponsHudActive(%slot, %ret, %vis);\n"
    " }\n"
     "};\n"
     "activatePackage(T2LaunchShazbotHudTelemetry);\n";

static const char autoplay_demo_exit_package[] =
    "package T2LaunchAutoplayDemoExit {\n"
    " function demoPlaybackComplete() {\n"
    "  Parent::demoPlaybackComplete();\n"
    "  echo(\"t2launch: autoplay demo complete; exiting\");\n"
    "  schedule(1, 0, \"quit\");\n"
    " }\n"
    "};\n"
    "activatePackage(T2LaunchAutoplayDemoExit);\n";

/* TribesNext account unlock and T2CSRI client handlers (hook.tribesnext). */
static const char tribesnext_runtime_script[] =
     "function t2launch_tnUnlockPoll() {\n"
       " if ($T2Launch::TNReady && trim($LoginName) !$= \"\" && stricmp($T2Launch::TNUsername, $LoginName) != 0) {\n"
       "  hook_tn_clear(); $T2Launch::TNReady = 0; $T2Launch::TNLastAttempt = \"\";\n"
      " }\n"
      " if (!$T2Launch::TNReady && trim($LoginName) !$= \"\" && $LoginPassword !$= \"\") {\n"
     "  %fingerprint = hook_sha1($LoginName @ \"\\t\" @ $LoginPassword);\n"
     "  if (%fingerprint !$= $T2Launch::TNLastAttempt) {\n"
     "   $T2Launch::TNLastAttempt = %fingerprint;\n"
     "   schedule(350, 0, \"t2launch_tnTryUnlock\", %fingerprint);\n"
     "  }\n"
     " }\n"
     " schedule(100, 0, \"t2launch_tnUnlockPoll\");\n"
     "}\n"
    "function t2launch_tnTryUnlock(%fingerprint) {\n"
    " if ($T2Launch::TNReady || %fingerprint !$= hook_sha1($LoginName @ \"\\t\" @ $LoginPassword)) return;\n"
       " if (hook_tn_unlock($LoginName, $LoginPassword) $= \"ok\") {\n"
         "  $T2Launch::TNReady = 1; $T2Launch::TNUsername = $LoginName;\n"
       " }\n"
    "}\n"
    "function t2launch_tnReject(%message) {\n"
    " error(%message); schedule(1000, 0, \"disconnect\");\n"
    "}\n"
    "function clientCmdt2csri_pokeClient(%version) {\n"
    " hook_tn_trace(\"poke\");\n"
    " if (!$T2Launch::TNReady) { t2launch_tnReject(\"T2CSRI: use your TribesNext account password in the login screen to unlock the stored key\"); return; }\n"
    " $T2Launch::TNEncryptedChallenge = \"\";\n"
    " %certLength = hook_tn_certificate_length();\n"
    " if (%certLength <= 0 || %certLength > 20000) { t2launch_tnReject(\"T2CSRI: no account certificate is loaded\"); return; }\n"
    " echo(\"T2CSRI: sending existing account certificate\");\n"
    " for (%i = 0; %i < %certLength; %i += 200) {\n"
    "  %certChunk = hook_tn_certificate_chunk(%i);\n"
    "  if (strLen(%certChunk) == 0) { t2launch_tnReject(\"T2CSRI: certificate chunk was empty\"); return; }\n"
    "  commandToServer('t2csri_sendCertChunk', %certChunk);\n"
    " } hook_tn_trace(\"certificate_sent\");\n"
    " %address = $T2Launch::TNJoinAddress;\n"
    " if (trim(%address) $= \"\") %address = ServerConnection.getAddress();\n"
    " %firstColon = strstr(%address, \":\");\n"
    " if (%firstColon < 0) { t2launch_tnReject(\"T2CSRI: invalid game-server address\"); return; }\n"
    " %afterFirst = getSubStr(%address, %firstColon + 1, strLen(%address));\n"
    " %secondColon = strstr(%afterFirst, \":\");\n"
    " if (%secondColon >= 0) %serverIP = getSubStr(%afterFirst, 0, %secondColon);\n"
    " else %serverIP = getSubStr(%address, 0, %firstColon);\n"
    " echo(\"T2CSRI: challenge target IP \" @ %serverIP);\n"
    " %serverIPHex = hook_tn_ipv4_hex(%serverIP);\n"
    " if (strLen(%serverIPHex) != 8) { t2launch_tnReject(\"T2CSRI: could not encode game-server IPv4 address\"); return; }\n"
    " hook_tn_ip_trace(%serverIP, %serverIPHex);\n"
    " $T2Launch::TNClientChallenge = hook_rand64() @ %serverIPHex;\n"
    " schedule(1000, 0, \"t2launch_tnSendChallenge\");\n"
    "}\n"
    "function t2launch_tnSendChallenge() {\n"
    " if ($T2Launch::TNClientChallenge !$= \"\") {\n"
    "  commandToServer('t2csri_sendChallenge', $T2Launch::TNClientChallenge);\n"
    "  hook_tn_trace(\"client_challenge_sent\");\n"
    "  echo(\"T2CSRI: client challenge sent\");\n"
    " }\n"
    "}\n"
    "function clientCmdt2csri_getChallengeChunk(%chunk) {\n"
    " if (strLen($T2Launch::TNEncryptedChallenge) + strLen(%chunk) > 8192) { t2launch_tnReject(\"T2CSRI: server challenge exceeded the size limit\"); return; }\n"
    " if ($T2Launch::TNEncryptedChallenge $= \"\") echo(\"T2CSRI: received encrypted server challenge\");\n"
    " hook_tn_trace(\"challenge_chunk\");\n"
    " $T2Launch::TNEncryptedChallenge = $T2Launch::TNEncryptedChallenge @ strLwr(%chunk);\n"
    "}\n"
    "function clientCmdt2csri_decryptChallenge() {\n"
    " hook_tn_trace(\"decrypt_challenge\");\n"
    " %decrypted = hook_tn_decrypt($T2Launch::TNEncryptedChallenge);\n"
    " %clientLen = strLen($T2Launch::TNClientChallenge);\n"
    " if (strLwr(getSubStr(%decrypted, 0, %clientLen)) !$= strLwr($T2Launch::TNClientChallenge)) { t2launch_tnReject(\"T2CSRI: server challenge did not echo client nonce\"); return; }\n"
    " %serverChallenge = getSubStr(%decrypted, %clientLen, strLen(%decrypted));\n"
    " if (trim(%serverChallenge) $= \"\") { t2launch_tnReject(\"T2CSRI: server challenge was empty\"); return; }\n"
    " schedule(getRandom(128, 512), 0, \"t2launch_tnRespond\", %serverChallenge);\n"
    " $T2Launch::TNEncryptedChallenge = \"\";\n"
    "}\n"
    "function t2launch_tnRespond(%challenge) {\n"
    " commandToServer('t2csri_challengeResponse', %challenge);\n"
    " hook_tn_trace(\"response_sent\");\n"
    "}\n"
      "t2launch_tnUnlockPoll();\n";

/* Dedicated server: the helper the hook polls until the server scripts have
   loaded, then the server half of T2CSRI (server.tribesnext_auth). */
static const char server_core_script[] =
    "function t2launch_serverReady() {\n"
    " if (isObject(ServerGroup)) return \"1\";\n"
    " return \"0\";\n"
    "}\n";

/* TribesNext players carry no WON certificate. This is TribesNext's own
 * server protocol (serverSide.cs, T2CSRI 1.1), with the hook verifying the
 * account certificate against the TribesNext root key and doing the RSA in
 * native code: the server pokes the client, receives its certificate in
 * chunks and a client challenge, encrypts the client challenge and its own
 * to the account key, and admits the player once the client returns the
 * server half. Players with a WON certificate (wonemu accounts) skip it. */
static const char server_tribesnext_script[] =
    "function t2launch_tnClientIP(%client) {\n"
    " %ip = %client.getAddress();\n"
    " %ip = getSubStr(%ip, strstr(%ip, \":\") + 1, strlen(%ip));\n"
    " return getSubStr(%ip, 0, strstr(%ip, \":\"));\n"
    "}\n"
    "function t2launch_tnIsHex(%text) {\n"
    " %text = strlwr(%text);\n"
    " if (strlen(%text) == 0) return 0;\n"
    " for (%i = 0; %i < strlen(%text); %i++)\n"
    "  if (strstr(\"0123456789abcdef\", getSubStr(%text, %i, 1)) < 0) return 0;\n"
    " return 1;\n"
    "}\n"
    "function t2launch_tnSameLan(%source, %destination) {\n"
    " if (getSubStr(%destination, 0, 3) $= \"10.\") return getSubStr(%source, 0, 3) $= \"10.\";\n"
    " if (getSubStr(%destination, 0, 4) $= \"172.\" && getSubStr(%destination, 4, 2) > 15 && getSubStr(%destination, 4, 2) < 32)\n"
    "  return getSubStr(%source, 0, 4) $= \"172.\" && getSubStr(%source, 4, 2) > 15 && getSubStr(%source, 4, 2) < 32;\n"
    " if (getSubStr(%destination, 0, 8) $= \"192.168.\") return getSubStr(%source, 0, 8) $= \"192.168.\";\n"
    " if (getSubStr(%destination, 0, 8) $= \"169.254.\") return getSubStr(%source, 0, 8) $= \"169.254.\";\n"
    " if (getSubStr(%destination, 0, 4) $= \"127.\") return getSubStr(%source, 0, 4) $= \"127.\";\n"
    " return 0;\n"
    "}\n"
    /* The client names the address it connected to, so a proof it gives one
       server is no good on another. Accept our public address (configured,
       or looked up at launch as TribesNext servers do), the bind address, or
       the player's own LAN; without a public address only the last two. */
    "function t2launch_tnReasonable(%source, %destination) {\n"
    " if (%destination $= \"\") return 0;\n"
    " %public = hook_option(\"server.public_address\");\n"
    " if (%public !$= \"\" && %destination $= %public) return 1;\n"
    " if ($Host::BindAddress !$= \"\" && %destination $= $Host::BindAddress) return 1;\n"
    " return t2launch_tnSameLan(%source, %destination);\n"
    "}\n"
    "function t2launch_tnReject(%client, %reason) {\n"
    " echo(\"T2CSRI: refused \" @ %client.getAddress() @ \": \" @ %reason);\n"
    " %client.setDisconnectReason(%reason);\n"
    " %client.delete();\n"
    "}\n"
    "function t2launch_tnExpire(%client) {\n"
    " if (isObject(%client) && !%client.doneAuthenticating)\n"
    "  t2launch_tnReject(%client, \"This is a TribesNext server. You must install the TribesNext client to play. See www.tribesnext.com for info.\");\n"
    "}\n"
    "function serverCmdt2csri_sendCertChunk(%client, %chunk) {\n"
    " if (%client.doneAuthenticating || !%client.t2launchTNPending) return;\n"
    " %client.t2csri_cert = %client.t2csri_cert @ %chunk;\n"
    " if (strlen(%client.t2csri_cert) > 20000)\n"
    "  t2launch_tnReject(%client, \"Account certificate too long. Check your account key for corruption.\");\n"
    "}\n"
    "function serverCmdt2csri_sendChallenge(%client, %clientChallenge) {\n"
    " if (%client.doneAuthenticating || !%client.t2launchTNPending || %client.t2csri_serverChallenge !$= \"\") return;\n"
    " %cert = %client.t2csri_cert;\n"
    " if (hook_tn_verify_cert(%cert) !$= \"1\") { t2launch_tnReject(%client, \"Invalid account certificate.\"); return; }\n"
    " if (!t2launch_tnIsHex(%clientChallenge) || strlen(%clientChallenge) < 9 || strlen(%clientChallenge) > 64) {\n"
    "  t2launch_tnReject(%client, \"Invalid characters in client challenge.\"); return;\n"
    " }\n"
    " %claimed = hook_tn_hex_ipv4(getSubStr(%clientChallenge, strlen(%clientChallenge) - 8, 8));\n"
    " %source = t2launch_tnClientIP(%client);\n"
    " if (!t2launch_tnReasonable(%source, %claimed)) {\n"
    "  t2launch_tnReject(%client, \"Potential man in the middle attack detected. Your client claims it connected to: \" @ %claimed @ \", but the server does not consider this reasonable.\");\n"
    "  return;\n"
    " }\n"
    " %client.t2csri_clientChallenge = %clientChallenge;\n"
    " %client.t2csri_serverChallenge = hook_rand64() @ hook_tn_ipv4_hex(%source);\n"
    " %encrypted = hook_rsa_mod_exp(%clientChallenge @ %client.t2csri_serverChallenge, getField(%cert, 2), getField(%cert, 3));\n"
    " if (!t2launch_tnIsHex(%encrypted)) { t2launch_tnReject(%client, \"The server could not encrypt your challenge.\"); return; }\n"
    " for (%i = 0; %i < strlen(%encrypted); %i += 200)\n"
    "  commandToClient(%client, 't2csri_getChallengeChunk', getSubStr(%encrypted, %i, 200));\n"
    " commandToClient(%client, 't2csri_decryptChallenge');\n"
    " %client.t2launchTNAuthInfo = getField(%cert, 0) @ \"\\t\\t0\\t\" @ getField(%cert, 1) @ \"\\n0\\n\";\n"
    "}\n"
    "function serverCmdt2csri_challengeResponse(%client, %serverChallenge) {\n"
    " if (%client.doneAuthenticating || !%client.t2launchTNPending) return;\n"
    " if (%client.t2csri_serverChallenge $= \"\" || %client.t2csri_serverChallenge !$= %serverChallenge) {\n"
    "  t2launch_tnReject(%client, \"Invalid server challenge. Check your account key for corruption.\"); return;\n"
    " }\n"
    " %client.t2launchTNVerified = 1;\n"
    " echo(\"T2CSRI: \" @ getField(%client.t2launchTNAuthInfo, 0) @ \" (\" @ getField(%client.t2launchTNAuthInfo, 3) @ \") authenticated\");\n"
    " %client.onConnect(%client.tname, %client.trgen, %client.tskin, %client.tvoic, %client.tvopi);\n"
    "}\n"
    "package T2LaunchTribesNextServer {\n"
    " function GameConnection::onConnect(%client, %name, %raceGender, %skin, %voice, %voicePitch) {\n"
    "  if (!%client.t2launchTNVerified && !%client.isAIControlled() && getField(%client.getAuthInfo(), 3) <= 0) {\n"
    "   if (%client.t2launchTNPending) return;\n"
    "   %client.t2launchTNPending = 1;\n"
    "   %client.tname = %name; %client.trgen = %raceGender; %client.tskin = %skin;\n"
    "   %client.tvoic = %voice; %client.tvopi = %voicePitch;\n"
    "   %client.tterm = schedule(15000, 0, t2launch_tnExpire, %client);\n"
    "   commandToClient(%client, 't2csri_pokeClient', \"T2CSRI 1.1 - 03/18/2009\");\n"
    "   return;\n"
    "  }\n"
    "  if (isEventPending(%client.tterm)) cancel(%client.tterm);\n"
    "  Parent::onConnect(%client, %name, %raceGender, %skin, %voice, %voicePitch);\n"
    "  %client.doneAuthenticating = 1;\n"
    " }\n"
    /* A player dropped while authenticating never joined, so there is no
       leaving message or score to clean up. */
    " function GameConnection::onDrop(%client, %reason) {\n"
    "  if (!isObject(%client) || (%client.t2launchTNPending && !%client.doneAuthenticating)) return;\n"
    "  Parent::onDrop(%client, %reason);\n"
    " }\n"
    " function GameConnection::getAuthInfo(%client) {\n"
    "  if (%client.t2launchTNVerified) return %client.t2launchTNAuthInfo;\n"
    "  return Parent::getAuthInfo(%client);\n"
    " }\n"
    "};\n"
    "activatePackage(T2LaunchTribesNextServer);\n";

/* ChooseFilterDlg.cs clamps a filter's Max Bots field to 16; allow the full
   0-255 a server can report, matching install_browser_bot_limit(). */
static const char filter_max_bots_override[] =
    "function FilterEditDlg::setMaxBots(%this) {\n"
    " %newMax = FilterEditMaxBots.getValue();\n"
    " if (%newMax < 0) FilterEditMaxBots.setValue(0);\n"
    " else if (%newMax > 255) FilterEditMaxBots.setValue(255);\n"
    "}\n";

/* The Network options sliders stop at 32 packets/s and 450 bytes; match
   install_net_packet_limits() and offer the rates QoL servers recommend. */
static const char net_options_override[] =
    "if (isObject(OP_PacketRateSlider)) OP_PacketRateSlider.range = \"2 64\";\n"
    "if (isObject(OP_PacketSizeSlider)) OP_PacketSizeSlider.range = \"100 1000\";\n"
    "$NetworkPreset[$NetworkPresetCount] = \"TribesNext QoL\\t64\\t32\\t1000\";\n"
    "if (isObject(OP_NetworkPresetsMenu) && OP_NetworkPresetsMenu.size())\n"
    " OP_NetworkPresetsMenu.add(\"TribesNext QoL\", $NetworkPresetCount);\n"
    "$NetworkPresetCount++;\n";

/* The stock client disconnects when a mission needs an interior, shape or
   terrain it doesn't have. Fetch that file from the TribesNext depot with
   hook_depot_fetch(), rescan the mod paths so the game sees it, and rejoin;
   repeat for each further missing file. Anything else, a file already tried
   on this server, or a failed download keeps the stock DISCONNECTED box. */
static const char depot_downloads_script[] =
    "function t2launch_depotFile(%msg) {\n"
    " for (%i = 0; %i < 3; %i++) {\n"
    "  %prefix = getField(\"Unable to load interior: \\tUnable to load shape: \\t"
    "You are missing a file needed to play this mission: \", %i);\n"
    "  %at = strstr(%msg, %prefix);\n"
    "  if (%at >= 0) {\n"
    "   %file = trim(getSubStr(%msg, %at + strlen(%prefix), 1000));\n"
    "   if (getSubStr(%file, strlen(%file) - 1, 1) $= \".\") %file = getSubStr(%file, 0, strlen(%file) - 1);\n"
    "   return %file;\n"
    "  }\n"
    " }\n"
    " return \"\";\n"
    "}\n"
    "function t2launch_depotPoll() {\n"
    " %status = hook_depot_status();\n"
    " if (%status $= \"running\") { schedule(250, 0, \"t2launch_depotPoll\"); return; }\n"
    " CloseMessagePopup();\n"
    " %detail = getSubStr(%status, strlen(getWord(%status, 0)) + 1, 1000);\n"
    " if (getWord(%status, 0) $= \"ok\") {\n"
    "  echo(\"TribesNext depot: saved \" @ %detail);\n"
    "  setModPaths(getModPaths());\n"
    "  $T2Launch::DepotRetries = 12;\n"
    "  JoinGame($T2Launch::DepotAddress);\n"
    "  return;\n"
    " }\n"
    " MessageBoxOK(\"DISCONNECTED\", $T2Launch::DepotMessage NL \"\" NL \"The TribesNext depot download failed: \" @ %detail);\n"
    "}\n"
    /* The server may be changing maps when the rejoin arrives; it refuses
       connections until the new map is loaded, so try again for a minute. */
    "function t2launch_depotRetry(%msg) {\n"
    " if ($T2Launch::DepotRetries <= 0 || strstr(%msg, \"cycling missions\") < 0) return 0;\n"
    " $T2Launch::DepotRetries--;\n"
    " echo(\"TribesNext depot: \" @ %msg @ \" Retrying in 5 seconds.\");\n"
    " CloseMessagePopup();\n"
    " DisconnectedCleanup();\n"
    " MessagePopup(\"REJOINING\", \"The server is changing maps. Rejoining in a moment...\");\n"
    " schedule(5000, 0, \"JoinGame\", $T2Launch::DepotAddress);\n"
    " return 1;\n"
    "}\n"
    "package T2LaunchDepot {\n"
    "function onChallengeRequestRejected(%msg) {\n"
    " if (!t2launch_depotRetry(%msg)) Parent::onChallengeRequestRejected(%msg);\n"
    "}\n"
    "function onConnectRequestRejected(%msg) {\n"
    " if (!t2launch_depotRetry(%msg)) Parent::onConnectRequestRejected(%msg);\n"
    "}\n"
    "function JoinGame(%address) {\n"
    " if (%address !$= $T2Launch::DepotAddress) {\n"
    "  $T2Launch::DepotAddress = %address; $T2Launch::DepotTried = \"\"; $T2Launch::DepotCount = 0;\n"
    "  $T2Launch::DepotRetries = 0;\n"
    " }\n"
    " Parent::JoinGame(%address);\n"
    "}\n"
    "function onConnectionToServerLost(%msg) {\n"
    " %file = t2launch_depotFile(%msg);\n"
    " if ($T2Launch::DepotAddress $= \"\") $T2Launch::DepotAddress = $JoinGameAddress;\n"
    " if (%file $= \"\" || $T2Launch::DepotAddress $= \"\" || $T2Launch::DepotCount >= 32 ||\n"
    "     strstr(\"\\t\" @ $T2Launch::DepotTried @ \"\\t\", \"\\t\" @ %file @ \"\\t\") >= 0 ||\n"
    "     hook_depot_fetch(%file) !$= \"1\") {\n"
    "  Parent::onConnectionToServerLost(%msg);\n"
    "  return;\n"
    " }\n"
    " $T2Launch::DepotTried = $T2Launch::DepotTried @ \"\\t\" @ %file;\n"
    " $T2Launch::DepotCount++;\n"
    " $T2Launch::DepotMessage = %msg;\n"
    " DisconnectedCleanup();\n"
    " MessagePopup(\"DOWNLOADING\", \"Fetching \" @ %file @ \" from the TribesNext depot...\");\n"
    " schedule(250, 0, \"t2launch_depotPoll\");\n"
    "}\n"
    "};\n"
    "activatePackage(T2LaunchDepot);\n";

/* ── Discord presence (discord.presence) ─────────────────────────────────
 * The stock client keeps the server, map and game type it was told on
 * joining in $clServerName, $clMissionName and $clMissionType. Every few
 * seconds the hook asks t2launch_presence() for the state and passes each
 * change as a line to $T2LAUNCH_DISCORD_HELPER, which talks to Discord. The
 * helper exits when the game closes its end of the socket. */
static const char discord_presence_script[] =
    "function t2launch_presence() {\n"
    " if (!isObject(ServerConnection)) {\n"
    "  $clMissionName = \"\";\n"
    "  return \"menu\";\n"
    " }\n"
    " if (isDemo()) return \"demo\";\n"
    " if ($clMissionName $= \"\" || (isObject(LoadingGui) && LoadingGui.isAwake())) return \"joining\";\n"
    " %players = 0;\n"
    " if (isObject(PlayerListGroup)) %players = PlayerListGroup.getCount();\n"
    " return \"game\\t\" @ $clServerName @ \"\\t\" @ $clMissionName @ \"\\t\" @ $clMissionType @ \"\\t\" @ %players;\n"
    "}\n";

static int discord_socket = -1;
static pid_t discord_child = -1;
static time_t discord_last_poll;
static char discord_last_state[512];

static int start_discord_presence(void) {
    const char *helper = getenv("T2LAUNCH_DISCORD_HELPER");
    const char *app_id = hook_option_str("discord.app_id");
    if (discord_socket >= 0) return 1;
    if (!helper || !helper[0] || access(helper, X_OK) != 0) {
        fprintf(stderr, "[t2_hook] Discord presence: no helper (T2LAUNCH_DISCORD_HELPER)\n");
        return 0;
    }
    if (!app_id || !app_id[0] || strspn(app_id, "0123456789") != strlen(app_id)) {
        fprintf(stderr, "[t2_hook] Discord presence: the application ID must be a number\n");
        return 0;
    }
    char **env = helper_environment();
    int pair[2];
    if (!env) return 0;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) { free(env); return 0; }
    char pid_text[16];
    snprintf(pid_text, sizeof(pid_text), "%d", (int)getpid());
    char *args[] = {(char *)helper, "--app-id", (char *)app_id, "--pid", pid_text, NULL};
    pid_t child = fork();
    if (child == 0) {
        dup2(pair[1], STDIN_FILENO);
        execve(helper, args, env);
        _exit(127);
    }
    free(env);
    close(pair[1]);
    if (child < 0) { close(pair[0]); return 0; }
    discord_socket = pair[0];
    discord_child = child;
    discord_last_state[0] = 0;
    fprintf(stderr, "[t2_hook] Started Discord presence for application %s\n", app_id);
    return 1;
}

/* Send the state if it changed; a closed helper stops presence for the session. */
static void send_discord_presence(const char *state) {
    char line[sizeof(discord_last_state) + 1];
    if (discord_socket < 0 || !state) return;
    snprintf(line, sizeof(discord_last_state), "%s", state);
    line[strcspn(line, "\r\n")] = 0;
    if (strcmp(line, discord_last_state) == 0) return;
    memcpy(discord_last_state, line, strlen(line) + 1);
    size_t length = strlen(line);
    line[length++] = '\n';
    if (send(discord_socket, line, length, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)length) {
        fprintf(stderr, "[t2_hook] Discord presence helper stopped\n");
        close(discord_socket);
        discord_socket = -1;
        waitpid(discord_child, NULL, WNOHANG);
    }
}

static void poll_discord_presence(time_t now) {
    if (discord_socket < 0 || now - discord_last_poll < 5) return;
    discord_last_poll = now;
    send_discord_presence(Con__evaluate("t2launch_presence();", 0, NULL, 0));
}

#define CONSOLE_TABLE_SIZE_OFFS 4

/* PlayerPane::ButtonClick(2) trusts memberships embedded in AuthInfo, so the
 * local account's branch never sends query 13 and leaves the wait cursor active.
 * Preserve the other tabs but always fetch the tribe list through DBProxy. */
static const char player_pane_button_click_override[] =
    "function PlayerPane::ButtonClick(%this, %senderid) {\n"
    " canvas.SetCursor(ArrowWaitCursor);\n"
    " %this.key = LaunchGui.key++; %this.tabstate = \"WARRIOR\";\n"
    " if (isEventPending(TribeAndWarriorBrowserGui.eid)) cancel(TribeAndWarriorBrowserGui.eid);\n"
    " switch (%senderid) {\n"
    " case 0:\n"
    "  W_Text.setValue(\"\"); %this.state = \"getWarriorProfile\"; %this.qrystatus = 0;\n"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());\n"
    "  if (TWBTitle.OldText $= TWBTitle.name || W_MemberList.rowCount() == 0)\n"
    "   %this.NeedRefresh = 0; else %this.NeedRefresh = 1;\n"
    "  TWBTitle.OldText = TWBTitle.name;\n"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQuery,\n"
    "   23, %playerName, %this, %this.key);\n"
    " case 1:\n"
    "  W_Text.setValue(\"\"); %this.state = \"getWarriorHistory\"; %this.qrystatus = 1;\n"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());\n"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQueryArray,\n"
    "   12, 0, %playerName, %this, %this.key, true);\n"
    " case 2:\n"
    "  W_MemberList.Clear(); W_MemberList.ClearColumns(); W_MemberList.clearList();\n"
    "  W_MemberList.CID = 0; %this.wtid = 0; %this.state = \"getWarriorTribeList\";\n"
    "  W_MemberList.addColumn(0, \"TRIBE\", 94, 0, 330);\n"
    "  W_MemberList.addColumn(1, \"TITLE\", 80, 0, 300);\n"
    "  W_MemberList.addColumn(2, \"RNK\", 38, 0, 50, \"numeric center\");\n"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());\n"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQueryArray,\n"
    "   13, 0, %playerName, %this, %this.key, true);\n"
    " case 3:\n"
    "  W_MemberList.Clear(); W_MemberList.ClearColumns(); W_MemberList.clearList();\n"
    "  W_MemberList.CID = 1; W_MemberList.addColumn(0, \"BUDDY\", 100, 0, 250);\n"
    "  W_MemberList.addColumn(1, \"SINCE\", 112, 0, 250);\n"
    "  %this.key = LaunchGui.key++; %this.state = \"getWarriorBuddyList\";\n"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());\n"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQueryArray,\n"
    "   5, 0, %playerName, %this, %this.key, true);\n"
    " case 4:\n"
    "  W_Text.setValue(\"\"); %this.state = \"getVisitorOptions\";\n"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());\n"
    "  TWBTitle.OldText = TWBTitle.name;\n"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQuery,\n"
    "   23, %playerName, %this, %this.key, true);\n"
    " case 5:\n"
    "  if (trim(W_Text.getText()) !$= \"\") {\n"
    "   WarriorPropertiesDlg.pendingChanges = \"\"; Canvas.PushDialog(WarriorPropertiesDlg);\n"
    "  } else {\n"
    "   W_Profile.setValue(1);\n"
    "   messageBoxOK(\"ERROR\", \"Your Profile was not loaded properly, Please wait a moment and try again\");\n"
    "  }\n"
    " }\n"
    "}\n"
    "function PlayerPane::onDatabaseRow(%this, %row, %isLastRow, %key) {\n"
    " if (%this.key != %key) return;\n"
    " switch$(%this.state) {\n"
    " case \"warriorHistory\":\n"
    "  W_Text.setText(W_Text.getText() @ %row @ \"\\n\");\n"
    "  if (%isLastRow) { %this.state = \"done\";\n"
    "   if (%this.needRefresh) { %this.needRefresh = 0; W_tribes.setValue(1); }\n"
    "  }\n"
    " case \"warriorTribeList\":\n"
    "  %wid = getField(%row, 2); %name = getField(%row, 0);\n"
    "  %title = getField(%row, 5); %adminLevel = getField(%row, 3);\n"
    "  %editkick = getField(%row, 4);\n"
    "  if (%title $= \"\") %title = \"Not Shown\";\n"
    "  W_MemberList.AddMember(%this.wtid, %wid, %name, %adminLevel, %editkick, %row);\n"
    "  W_MemberList.AddRow(%wid, %name TAB %title TAB %adminLevel TAB %this.wtid);\n"
    "  %this.wtid++; Canvas.repaint();\n"
    "  if (%isLastRow) { %this.state = \"done\"; Canvas.setCursor(DefaultCursor); }\n"
    " case \"warriorBuddyList\":\n"
    "  W_MemberList.AddInvite(%this.blid, getField(%row, 3), getFields(%row, 0, 3),\n"
    "   getFields(%row, 0, 3), 4, %row);\n"
    "  W_MemberList.AddRow(getField(%row, 3), getField(%row, 0) TAB\n"
    "   getField(%row, 4) TAB %this.blid); %this.blid++;\n"
    "  if (%isLastRow) W_MemberList.getOnlineStatus();\n"
    " }\n"
    "}\n"
    "function TribeAndWarriorBrowserGui::onWake(%this) {\n"
    " MemberList.ClearColumns(); W_MemberList.ClearColumns();\n"
    " MemberList.Clear(); W_MemberList.clear(); Canvas.pushDialog(LaunchToolbarDlg);\n"
    " if (TWBTabView.tabCount() == 0) {\n"
    "  %info = WONGetAuthInfo();\n"
    "  %warrior = getField(%info, 0);\n"
    "  if (trim(%warrior) $= \"\") %warrior = $pref::LastLoginName;\n"
    "  if (trim(%warrior) $= \"\") %warrior = $LoginName;\n"
    "  TWBTabView.view(%warrior, \"Warrior\"); w_profile.setValue(1);\n"
    "  %tribeCount = getField(getRecord(%info, 1), 0);\n"
    "  for (%i = 0; %i < %tribeCount; %i++) {\n"
    "   %tribe = getField(getRecord(%info, %i + 2), 0);\n"
    "   if (trim(%tribe) !$= \"\") TWBTabView.addTab(%i + 1, %tribe, 1);\n"
    "  }\n"
    " } else if (PlayerPane.visible) PlayerPane.onWake();\n"
    " else TribePane.onWake();\n"
    " %selectedTab = TWBTabView.getSelectedID();\n"
    " if (TWBTabView.getTabText(%selectedTab) $= \"\") {\n"
    "  %warrior = $pref::LastLoginName;\n"
    "  if (trim(%warrior) $= \"\") %warrior = $LoginName;\n"
    "  if (trim(%warrior) !$= \"\") TWBTabView.setTabText(%selectedTab, %warrior);\n"
    " }\n"
    " %warrior = $pref::LastLoginName;\n"
    " if (trim(%warrior) $= \"\") {\n"
    "  %info = WONGetAuthInfo();\n"
    "  %warrior = getField(%info, 0);\n"
    " }\n"
    " if (trim(%warrior) $= \"\") %warrior = $LoginName;\n"
    " if (!isObject(T2LaunchTribeCache)) {\n"
    "  new ScriptObject(T2LaunchTribeCache) { class = \"T2LaunchTribeCache\"; };\n"
    " }\n"
    " if (trim(%warrior) !$= \"\") {\n"
    "  T2LaunchTribeCache.tribeCount = 0;\n"
    "  T2LaunchTribeCache.key = LaunchGui.key++;\n"
    "  schedule(500, 0, DatabaseQueryArray, 13, 0, %warrior,\n"
    "   T2LaunchTribeCache, T2LaunchTribeCache.key, true);\n"
    " }\n"
    " hook_profile_browser_wake();\n"
    "}\n"
    "function T2LaunchTribeCache::onDatabaseQueryResult(%this, %status, %result, %key) {\n"
    " if (%this.key != %key) return;\n"
    " %this.loading = 0;\n"
    " if (getField(%status, 0) != 0) %this.failed = 1;\n"
    " Canvas.repaint();\n"
    "}\n"
    "function T2LaunchTribeCache::onDatabaseRow(%this, %row, %isLastRow, %key) {\n"
    " if (%this.key != %key) return;\n"
    " %name = getField(%row, 0);\n"
    " if (trim(%name) !$= \"\") {\n"
    "  %this.tribeName[%this.tribeCount] = %name;\n"
    "  %this.tribeTag[%this.tribeCount] = getField(%row, 1);\n"
    "  %this.tribeId[%this.tribeCount] = getField(%row, 2);\n"
    "  %this.tribeAccess[%this.tribeCount] = getField(%row, 3);\n"
    "  %this.tribeTitle[%this.tribeCount] = getField(%row, 5);\n"
    "  %this.tribeCount++; %exists = false;\n"
    "  for (%i = 0; %i < TWBTabView.tabCount(); %i++) {\n"
    "   if (stricmp(TWBTabView.getTabText(%i), %name) == 0) %exists = true;\n"
    "  }\n"
    "  if (!%exists) TWBTabView.addTab(TWBTabView.tabCount(), %name, 1);\n"
    " }\n"
    " if (%isLastRow) { %this.loading = 0; Canvas.repaint(); }\n"
    "}\n"
    "function GetTribeMember(%tribeName) {\n"
    " %info = WONGetAuthInfo();\n"
    " %count = getField(getRecord(%info, 1), 0);\n"
    " for (%i = 0; %i < %count; %i++) {\n"
    "  if (stricmp(getField(getRecord(%info, %i + 2), 0), %tribeName) == 0) return 1;\n"
    " }\n"
    " if (isObject(T2LaunchTribeCache)) {\n"
    "  for (%i = 0; %i < T2LaunchTribeCache.tribeCount; %i++) {\n"
    "   if (stricmp(T2LaunchTribeCache.tribeName[%i], %tribeName) == 0) return 1;\n"
    "  }\n"
    " }\n"
    " return 0;\n"
    "}\n"
    "function GetProfileHdr(%type, %line) {\n"
    " %info = WONGetAuthInfo();\n"
    " $GuidTribes = getRecords(%info, 1);\n"
    " if (%type == 0) {\n"
    "  TProfileHdr.tribeID = getField(%line, 0); TProfileHdr.tribeName = getField(%line, 1);\n"
    "  TProfileHdr.tribeTag = getField(%line, 2); TProfileHdr.appending = getField(%line, 3);\n"
    "  TProfileHdr.recruiting = getField(%line, 4); TProfileHdr.tribegfx = getField(%line, 5);\n"
    "  TProfileHdr.twa = 0; TProfileHdr.Desc = \"\";\n"
    "  TL_Profile.setVisible(1); TL_Roster.setVisible(1); TL_News.setVisible(1);\n"
    "  for (%i = 0; %i < getField(getRecord($GuidTribes, 0), 0); %i++) {\n"
    "   %authRow = getRecord($GuidTribes, %i + 1);\n"
    "   if (getField(%authRow, 3) == 1401) TProfileHdr.twa = 4;\n"
    "   else if (TProfileHdr.tribeID == getField(%authRow, 3) && TProfileHdr.twa == 0)\n"
    "    TProfileHdr.twa = getField(%authRow, 4);\n"
    "  }\n"
    "  if (isObject(T2LaunchTribeCache)) {\n"
    "   for (%i = 0; %i < T2LaunchTribeCache.tribeCount; %i++) {\n"
    "    if (stricmp(T2LaunchTribeCache.tribeName[%i], TProfileHdr.tribeName) == 0)\n"
    "     TProfileHdr.twa = T2LaunchTribeCache.tribeAccess[%i];\n"
    "   }\n"
    "  }\n"
    "  if (TProfileHdr.twa > 1) { TW_Admin.setVisible(1); TL_Invites.setVisible(1); }\n"
    "  else { TW_Admin.setVisible(0); TL_Invites.setVisible(0); }\n"
    " } else {\n"
    "  TProfileHdr.playerName = getField(%line, 0); TProfileHdr.playerTag = getField(%line, 1);\n"
    "  TProfileHdr.appending = getField(%line, 2); TProfileHdr.playerID = getField(%line, 3);\n"
    "  TProfileHdr.registered = getField(%line, 4); TProfileHdr.onLine = getField(%line, 5);\n"
    "  TProfileHdr.playerURL = getField(%line, 6); TProfileHdr.playerGFX = getField(%line, 7);\n"
    "  TProfileHdr.twa = 0; TProfileHdr.Desc = \"\";\n"
    "  if (getField(getRecord(%info, 0), 3) == getField(%line, 3)) TProfileHdr.twa = 1;\n"
    "  for (%i = 0; %i < getField(getRecord($GuidTribes, 0), 0); %i++) {\n"
    "   %authRow = getRecord($GuidTribes, %i + 1);\n"
    "   if (getField(%authRow, 3) == 1401 && getField(%authRow, 4) >= 2) TProfileHdr.twa = 1;\n"
    "  }\n"
    "  W_Profile.setVisible(1); W_History.setVisible(1); W_Tribes.setVisible(1);\n"
    "  %localName = $pref::LastLoginName;\n"
    "  if (trim(%localName) $= \"\") %localName = getField(getRecord(%info, 0), 0);\n"
    "  %isMe = (stricmp(TProfileHdr.playerName, %localName) == 0) ||\n"
    "   (stricmp(getField(getRecord(%info, 0), 0), TWBTitle.name) == 0);\n"
    "  %authID = getField(getRecord(%info, 0), 3);\n"
    "  if (trim(%authID) !$= \"\" && %authID == TProfileHdr.playerID) %isMe = 1;\n"
    "  TProfileHdr.isMe = %isMe;\n"
    "  TProfileHdr.twa = TProfileHdr.isMe;\n"
    "  if (TProfileHdr.twa) {\n"
    "   W_BuddyList.setText(\"BUDDYLIST\"); W_BuddyList.setVisible(1);\n"
    "   W_BuddyList.command = \"PlayerPane.ButtonClick(3);\"; W_BuddyList.groupNum = 5; W_Admin.setVisible(1);\n"
    "  } else {\n"
    "   W_BuddyList.setText(\"OPTIONS\"); W_BuddyList.setVisible(1);\n"
    "   W_BuddyList.command = \"PlayerPane.ButtonClick(4);\"; W_BuddyList.groupNum = 4; W_Admin.setVisible(0);\n"
    "  }\n"
    " }\n"
    "}\n"
    "function CreateTribeDlg::onWake(%this) {\n"
    " %frame = %this.getObject(0);\n"
    " if (isObject(%frame)) {\n"
    "  if (%frame.getCount() > 3) {\n"
    "   %nameEdit = %frame.getObject(3);\n"
    "   if (isObject(%nameEdit)) %nameEdit.IRCName = false;\n"
    "  }\n"
    " }\n"
    " if (isObject(CT_TagText)) CT_TagText.IRCName = false;\n"
    " updateTribeTagPreview();\n"
    "}\n"
    "function CreateTribeDlg::onDatabaseQueryResult(%this, %status, %resultString, %key) {\n"
    " if (%this.key != %key) return;\n"
    " if (getField(%status, 0) == 0) {\n"
    "  if (%this.state $= \"createTribe\") {\n"
    "   if (isObject(T2LaunchTribeCache)) {\n"
    "    %name = $CreateTribeName; %found = false;\n"
    "    for (%i = 0; %i < T2LaunchTribeCache.tribeCount; %i++) {\n"
    "     if (stricmp(T2LaunchTribeCache.tribeName[%i], %name) == 0) %found = true;\n"
    "    }\n"
    "    if (!%found) {\n"
    "     %index = T2LaunchTribeCache.tribeCount;\n"
    "     T2LaunchTribeCache.tribeName[%index] = %name;\n"
    "     T2LaunchTribeCache.tribeTag[%index] = $CreateTribeTag;\n"
    "     T2LaunchTribeCache.tribeAccess[%index] = 4;\n"
    "     T2LaunchTribeCache.tribeTitle[%index] = \"Founder\";\n"
    "     T2LaunchTribeCache.tribeCount++;\n"
    "    }\n"
    "   }\n"
    "   TWBTabView.view($CreateTribeName, \"Tribe\"); Canvas.popDialog(CreateTribeDlg);\n"
    "   $CreateTribeName = \"\"; $CreateTribeTag = \"\";\n"
    "   if (isObject(CreateTribeDlg)) CreateTribeDlg.delete(); WonUpdateCertificate();\n"
    "  }\n"
    " } else if (getSubStr(getField(%status, 1), 0, 9) $= \"ORA-04061\") {\n"
    "  %this.state = \"error\";\n"
    "  MessageBoxOK(\"ERROR\", \"There was an error processing your request, please wait a few moments and try again.\");\n"
    " } else { %this.state = \"error\"; MessageBoxOK(\"WARNING\", getField(%status, 1)); }\n"
    " Canvas.setCursor(DefaultCursor);\n"
    "}\n"
    ;

/* Only the Loki #25034 client has the fixed addresses used by this hook. */
static int readable_range(uintptr_t address, size_t length, int *protection) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512], permissions[5];
    unsigned long start, end;
    int found = 0;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) == 3 &&
            address >= start && address + length <= end && permissions[0] == 'r') {
            *protection = PROT_READ;
            if (permissions[1] == 'w') *protection |= PROT_WRITE;
            if (permissions[2] == 'x') *protection |= PROT_EXEC;
            found = 1;
            break;
        }
    }
    fclose(maps);
    return found;
}

/* Fill the three bootstrap slots from T2LAUNCH_WON_ENDPOINTS
   ("host:port[,host:port[,host:port]]", fewer entries repeat the last), else
   127.0.0.1 with T2LAUNCH_WON_PORT or the default port. Each entry replaces a
   "tribes2.mN.sierra.com:15104" string in place, so it may use 27 bytes. */
#define WON_ENDPOINT_SIZE 28
static int parse_won_endpoints(const char *list, const char *port_text,
                               char endpoints[3][WON_ENDPOINT_SIZE]) {
    if (!list || !*list) {
        unsigned long port = 15104;
        if (port_text && *port_text) {
            char *end;
            port = strtoul(port_text, &end, 10);
            if (*end || port == 0 || port > 65535) return 0;
        }
        for (int i = 0; i < 3; i++)
            snprintf(endpoints[i], WON_ENDPOINT_SIZE, "127.0.0.1:%lu", port);
        return 1;
    }
    int count = 0;
    const char *start = list;
    while (*start) {
        const char *end = strchr(start, ',');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (count == 3 || length == 0 || length >= WON_ENDPOINT_SIZE) return 0;
        char entry[WON_ENDPOINT_SIZE];
        memcpy(entry, start, length);
        entry[length] = 0;
        char *colon = strrchr(entry, ':');
        if (!colon || colon == entry || !colon[1]) return 0;
        for (const char *c = entry; c < colon; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '.' || *c == '-'))
                return 0;
        char *port_end;
        unsigned long port = strtoul(colon + 1, &port_end, 10);
        if (*port_end || port == 0 || port > 65535) return 0;
        memcpy(endpoints[count++], entry, length + 1);
        if (!end) break;
        start = end + 1;
    }
    if (count == 0) return 0;
    for (int i = count; i < 3; i++) memcpy(endpoints[i], endpoints[count - 1], WON_ENDPOINT_SIZE);
    return 1;
}

static int redirect_won_bootstrap(void) {
    const uintptr_t *addresses = target->won_bootstrap;
    static const char *original[] = {
        "tribes2.m1.sierra.com:15104", "tribes2.m2.sierra.com:15104",
        "tribes2.m3.sierra.com:15104"
    };
    char endpoints[3][WON_ENDPOINT_SIZE];
    if (!parse_won_endpoints(getenv("T2LAUNCH_WON_ENDPOINTS"), getenv("T2LAUNCH_WON_PORT"), endpoints)) {
        fprintf(stderr, "[t2_hook] WON redirect refused: invalid T2LAUNCH_WON_ENDPOINTS or T2LAUNCH_WON_PORT\n");
        return 0;
    }
    int protection = 0;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 0;
    uintptr_t page = addresses[0] & ~((uintptr_t)page_size - 1);
    for (size_t i = 0; i < 3; i++) {
        int current_protection;
        if (!readable_range(addresses[i], strlen(original[i]) + 1, &current_protection) ||
            addresses[i] + strlen(original[i]) + 1 > page + (uintptr_t)page_size ||
            (i > 0 && current_protection != protection)) {
            fprintf(stderr, "[t2_hook] WON redirect refused: unsupported memory layout\n");
            return 0;
        }
        protection = current_protection;
        if (memcmp((void *)addresses[i], original[i], strlen(original[i]) + 1) != 0) {
            fprintf(stderr, "[t2_hook] WON redirect refused: string mismatch at 0x%08lx\n",
                (unsigned long)addresses[i]);
            return 0;
        }
    }
    if (mprotect((void *)page, (size_t)page_size, protection | PROT_WRITE) != 0) {
        fprintf(stderr, "[t2_hook] WON redirect: mprotect failed: %s\n", strerror(errno));
        return 0;
    }
    for (size_t i = 0; i < 3; i++) {
        memset((void *)addresses[i], 0, strlen(original[i]) + 1);
        memcpy((void *)addresses[i], endpoints[i], strlen(endpoints[i]) + 1);
    }
    if (mprotect((void *)page, (size_t)page_size, protection) != 0) {
        fprintf(stderr, "[t2_hook] WON redirect: restoring page permissions failed: %s\n", strerror(errno));
        return 0;
    }
    fprintf(stderr, "[t2_hook] WON bootstrap: redirected endpoints to %s, %s, %s (memory only)\n",
            endpoints[0], endpoints[1], endpoints[2]);
    return 1;
}

/* Platform::getRealMilliseconds divides rdtsc ticks by a rate it measures for
   100 ms at startup. On CPUs and VMs whose timestamp counters are not
   synchronized or constant across cores, game time runs fast or jumps as the
   thread migrates. Jump to a CLOCK_MONOTONIC replacement instead. */
#define REAL_MILLISECONDS_ADDRESS 0x081CFA44  /* the client's; see game_targets */
static const unsigned char real_milliseconds_original[] = {0x0f, 0x31, 0x55, 0x89, 0xe5, 0x83, 0xec, 0x14};
static struct timespec monotonic_clock_base;

static uint32_t monotonic_real_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t elapsed = (int64_t)(now.tv_sec - monotonic_clock_base.tv_sec) * 1000 +
        (now.tv_nsec - monotonic_clock_base.tv_nsec) / 1000000;
    return (uint32_t)elapsed;
}

static int install_monotonic_clock(void) {
    uintptr_t address = target->real_milliseconds;
    int protection;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 0;
    uintptr_t page = address & ~((uintptr_t)page_size - 1);
    if (!readable_range(address, sizeof(real_milliseconds_original), &protection) ||
        !(protection & PROT_EXEC) || address + 5 > page + (uintptr_t)page_size ||
        memcmp((void *)address, real_milliseconds_original, sizeof(real_milliseconds_original)) != 0) {
        fprintf(stderr, "[t2_hook] Game clock patch refused: unexpected code at 0x%08lx\n",
                (unsigned long)address);
        return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &monotonic_clock_base);
    unsigned char jump[5] = {0xe9};
    int32_t offset = (int32_t)((uintptr_t)monotonic_real_milliseconds - (address + sizeof(jump)));
    memcpy(jump + 1, &offset, sizeof(offset));
    if (mprotect((void *)page, (size_t)page_size, protection | PROT_WRITE) != 0) {
        fprintf(stderr, "[t2_hook] Game clock patch: mprotect failed: %s\n", strerror(errno));
        return 0;
    }
    memcpy((void *)address, jump, sizeof(jump));
    if (mprotect((void *)page, (size_t)page_size, protection) != 0) {
        fprintf(stderr, "[t2_hook] Game clock patch: restoring page permissions failed: %s\n", strerror(errno));
        return 0;
    }
    char clocksource[64] = "unknown";
    FILE *source = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
    if (source) {
        if (fgets(clocksource, sizeof(clocksource), source)) trim_space(clocksource);
        fclose(source);
    }
    fprintf(stderr, "[t2_hook] Game clock: CLOCK_MONOTONIC instead of rdtsc (kernel clocksource %s, memory only)\n",
            clocksource);
    return 1;
}

/* queryMasterServer() given only a port (the browser's "All servers")
   defaults its maximum bot count to 16 with `mov $0x10,%eax`, and the client
   drops any server reporting more. Raise the default to 255, the largest
   count a server can report. The context bytes (push %eax; cmpl $6,argc)
   pin the instruction inside the console wrapper. */
#define BROWSER_BOT_LIMIT_ADDRESS 0x082E6B23
static const unsigned char browser_bot_limit_original[] =
    {0xb8, 0x10, 0x00, 0x00, 0x00, 0x50, 0x83, 0x7d, 0x0c, 0x06};

/* One in-memory code change: the bytes expected at address, and the bytes
   written at address + offset once every site in the set has matched. */
struct code_patch {
    uintptr_t address;
    const unsigned char *expected;
    size_t expected_length;
    size_t offset;
    const unsigned char *replacement;
    size_t replacement_length;
};

/* Apply all patches or none: refuse unless every site holds its expected
   executable bytes, then write each page and restore its protection. */
static int apply_code_patches(const char *name, const struct code_patch *patches, size_t count) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return 0;
    for (size_t i = 0; i < count; i++) {
        const struct code_patch *patch = &patches[i];
        uintptr_t page = patch->address & ~((uintptr_t)page_size - 1);
        int protection;
        if (patch->offset + patch->replacement_length > patch->expected_length ||
            !readable_range(patch->address, patch->expected_length, &protection) ||
            !(protection & PROT_EXEC) ||
            patch->address + patch->offset + patch->replacement_length > page + (uintptr_t)page_size ||
            memcmp((void *)patch->address, patch->expected, patch->expected_length) != 0) {
            fprintf(stderr, "[t2_hook] %s patch refused: unexpected code at 0x%08lx\n", name,
                    (unsigned long)patch->address);
            return 0;
        }
    }
    for (size_t i = 0; i < count; i++) {
        const struct code_patch *patch = &patches[i];
        uintptr_t page = patch->address & ~((uintptr_t)page_size - 1);
        int protection;
        if (!readable_range(patch->address, patch->expected_length, &protection) ||
            mprotect((void *)page, (size_t)page_size, protection | PROT_WRITE) != 0) {
            fprintf(stderr, "[t2_hook] %s patch: mprotect failed: %s\n", name, strerror(errno));
            return 0;
        }
        volatile unsigned char *target = (volatile unsigned char *)(patch->address + patch->offset);
        for (size_t b = 0; b < patch->replacement_length; b++) target[b] = patch->replacement[b];
        if (mprotect((void *)page, (size_t)page_size, protection) != 0) {
            fprintf(stderr, "[t2_hook] %s patch: restoring page permissions failed: %s\n", name,
                    strerror(errno));
            return 0;
        }
    }
    return 1;
}

/* Register the compatibility command on the first game-thread Torque
   evaluation, before that script is executed. The ordinary swap/usleep poll
   remains a fallback if the console table is not ready yet. */
#define CON_EVALUATE_GATE_RESUME_OFFSET 8
static const unsigned char con_evaluate_gate_original[] = {
    0x55, 0x89, 0xe5, 0x83, 0xec, 0x0c, 0x57, 0x56
};
static con_evaluate_t real_con_evaluate;
static int con_evaluate_gate_installed;
static int early_compat_commands_registered;
static int early_compat_commands_registering;

static const char *con_evaluate_gate(const char *code, unsigned char echo,
                                     const char *file_name, unsigned char no_calls) {
    (void)register_early_compat_commands();
    const char *result = real_con_evaluate
        ? real_con_evaluate(code, echo, file_name, no_calls) : "";
    (void)register_early_compat_commands();
    return result;
}

static int install_console_evaluate_gate(void) {
    if (con_evaluate_gate_installed) return 1;
    int protection = 0;
    if (!target || !target->con_evaluate ||
        !readable_range(target->con_evaluate, sizeof con_evaluate_gate_original,
                        &protection) || !(protection & PROT_EXEC) ||
        memcmp((void *)target->con_evaluate, con_evaluate_gate_original,
               sizeof con_evaluate_gate_original) != 0) {
        fprintf(stderr, "[t2_hook] memPatch compatibility: console evaluate hook refused (unsupported prologue)\n");
        return 0;
    }

    unsigned char *trampoline = mmap(NULL, 32, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return 0;
    memcpy(trampoline, con_evaluate_gate_original, sizeof con_evaluate_gate_original);
    trampoline[CON_EVALUATE_GATE_RESUME_OFFSET] = 0xe9;
    int32_t back = (int32_t)((target->con_evaluate + CON_EVALUATE_GATE_RESUME_OFFSET) -
        ((uintptr_t)trampoline + CON_EVALUATE_GATE_RESUME_OFFSET + 5));
    memcpy(trampoline + CON_EVALUATE_GATE_RESUME_OFFSET + 1, &back, sizeof back);
    __builtin___clear_cache((char *)trampoline,
        (char *)trampoline + CON_EVALUATE_GATE_RESUME_OFFSET + 5);
    if (mprotect(trampoline, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(trampoline, 32);
        return 0;
    }

    unsigned char replacement[sizeof con_evaluate_gate_original];
    replacement[0] = 0xe9;
    int32_t to_gate = (int32_t)((uintptr_t)con_evaluate_gate -
                                (target->con_evaluate + 5));
    memcpy(replacement + 1, &to_gate, sizeof to_gate);
    memset(replacement + 5, 0x90, sizeof replacement - 5);
    const struct code_patch patch = {target->con_evaluate, con_evaluate_gate_original,
        sizeof con_evaluate_gate_original, 0, replacement, sizeof replacement};
    if (!apply_code_patches("Console evaluate command-registration gate", &patch, 1)) {
        munmap(trampoline, 32);
        return 0;
    }
    real_con_evaluate = (con_evaluate_t)trampoline;
    Con__evaluate = con_evaluate_gate;
    con_evaluate_gate_installed = 1;
    fprintf(stderr, "[t2_hook] Installed early console command-registration gate (memory only)\n");
    return 1;
}

/* getT2VersionNumber feeds browser matching and peer version checks; both
   executables expose the same helper. Override that build identifier without
   touching the engine's packet/protocol version (51). The client's master-list
   request normally sets flag 0x80, which asks a master to pre-filter by
   version; clear only that reserved bit so the client can perform its local
   match against the configured identifier after receiving all rows. */
static const unsigned char build_version_helper_original[] = {
    0x55, 0x89, 0xe5, 0xb8, 0xca, 0x61, 0x00, 0x00, 0x89, 0xec, 0x5d, 0xc3
};
static const unsigned char master_list_version_flag_original[] = {0x80, 0xca, 0x80};
static const unsigned char master_list_version_flag_any[] = {0x80, 0xe2, 0x7f};

/* Numeric hook settings may be blank in an older or hand-edited profile. Treat
   empty/whitespace as the declared safe default; reject malformed nonblank text
   rather than allowing strtoul's zero result to become a patch value. */
static int hook_u31_or_default(const char *key, uint32_t fallback, uint32_t *value) {
    const char *text = hook_option_str(key);
    const char *number = text;
    if (!number) {
        *value = fallback;
        return 1;
    }
    while (*number == ' ' || *number == '\t' || *number == '\r' || *number == '\n') number++;
    if (!*number) {
        *value = fallback;
        return 1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(number, &end, 10);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
    if (errno || end == number || !end || *end || parsed == 0 || parsed > 0x7ffffffful) {
        fprintf(stderr, "[t2_hook] Invalid %s '%s'; expected decimal 1..2147483647\n",
                key, text);
        return 0;
    }
    *value = (uint32_t)parsed;
    return 1;
}

/* security.max_soundpack_kbytes explicitly allows zero (no cap). Empty or
   malformed text must use the safe default, not silently disable that limit. */
static long hook_long_or_default(const char *key, long fallback, long minimum, long maximum) {
    const char *text = hook_option_str(key);
    const char *number = text;
    if (!number) return fallback;
    while (*number == ' ' || *number == '\t' || *number == '\r' || *number == '\n') number++;
    if (!*number) return fallback;
    char *end = NULL;
    errno = 0;
    long parsed = strtol(number, &end, 10);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
    if (errno || end == number || !end || *end || parsed < minimum || parsed > maximum) {
        fprintf(stderr, "[t2_hook] Invalid %s '%s'; using default %ld\n", key, text, fallback);
        return fallback;
    }
    return parsed;
}

static int install_build_identifier(void) {
    uint32_t identifier;
    if (!hook_u31_or_default("net.build_identifier", 25034, &identifier)) return 0;
    const struct code_patch helper = {
        target->build_version_helper, build_version_helper_original,
        sizeof(build_version_helper_original), 4, (const unsigned char *)&identifier,
        sizeof(identifier)
    };
    if (target->dedicated) {
        if (!apply_code_patches("Build identifier", &helper, 1)) return 0;
    } else {
        const struct code_patch patches[] = {
            helper,
            {target->master_list_version_flag, master_list_version_flag_original,
             sizeof(master_list_version_flag_original), 0, master_list_version_flag_any,
             sizeof(master_list_version_flag_any)},
        };
        if (!apply_code_patches("Build identifier and cross-version master list", patches,
                                sizeof(patches) / sizeof(patches[0]))) return 0;
    }
    fprintf(stderr, "[t2_hook] Build identifier set to %u; network protocol unchanged%s\n",
            identifier, target->dedicated ? "" : "; version filtering is local to this client");
    return 1;
}

/* .rec headers carry a recording-format tag, independent from the master
   build identifier and network protocol. Patch writer, strict reader gate,
   and getDemoVersion()'s reported tag together; packet/block parsing remains
   the engine's native implementation and is not changed by this option. */
static const unsigned char demo_tag_writer_original[] = {
    0xc7, 0x45, 0xd4, 0x04, 0x00, 0x33, 0x00
};
static const unsigned char demo_tag_compare_original[] = {
    0x3d, 0x04, 0x00, 0x33, 0x00
};
static const unsigned char demo_tag_helper_original[] = {
    0x04, 0x00, 0x33, 0x00
};

static int install_demo_protocol_tag(void) {
    if (target->dedicated || !target->demo_tag_writer || !target->demo_tag_compare ||
        !target->demo_tag_helper) return 1;

    uint32_t tag;
    if (!hook_u31_or_default("demo.protocol_tag", 3342340, &tag)) return 0;
    const unsigned char *value = (const unsigned char *)&tag;
    const struct code_patch patches[] = {
        {target->demo_tag_writer, demo_tag_writer_original,
         sizeof(demo_tag_writer_original), 3, value, sizeof(tag)},
        {target->demo_tag_compare, demo_tag_compare_original,
         sizeof(demo_tag_compare_original), 1, value, sizeof(tag)},
        {target->demo_tag_helper, demo_tag_helper_original,
         sizeof(demo_tag_helper_original), 0, value, sizeof(tag)},
    };
    if (!apply_code_patches("Demo recording header tag", patches,
                            sizeof(patches) / sizeof(patches[0]))) return 0;
    fprintf(stderr,
            "[t2_hook] Demo header tag set to 0x%08x; network protocol and payload parser unchanged\n",
            tag);
    return 1;
}

/* ---- Demo-review reconstruction (review.cheat_view) -----------------------
   Loki #25034 CLIENT ONLY. RESEARCH.md records executable hashes, Windows
   counterparts, disassembly evidence and rejected earlier candidates. These
   are static-verified equivalents; visual playback verification is separate.
   Apply/revert at each swap according to the native isPlayingDemo callback.
   Display only; no autoaim. Autobeacon borrows recorded beacons during the
   native beacon draw, then restores their positions and types before returning.

   Global beacons: remove the sensor-group comparison's skip in the beacon
   enumeration. Keep the projection/depth guard at 0x082FC844 intact. */
static const unsigned char rv_gbeacons_orig[] = {0x75, 0x43};
static const unsigned char rv_nops[] = {0x90, 0x90};
#define RV_GBEACONS_ADDR 0x082FC6BA

/* hm2's FFI visibility-byte check is split into bit 0 and bits 1..7 by GCC.
   A set target-mask bit can mark the object friendly without requiring the
   matching object-info visibility byte. Retain the zero-mask rejection. */
static const unsigned char rv_ffi_bit0_orig[] = {0x75, 0x19};
static const unsigned char rv_ffi_bit0_jump[] = {0xeb, 0x19};
static const unsigned char rv_ffi_loop_orig[] = {0x74, 0xe8};
#define RV_FFI_BIT0_ADDR 0x0819103D
#define RV_FFI_LOOP_ADDR 0x08191056

/* Camera-query far plane (the direct Windows ffi-far counterpart), plus the
   HUD's independent read of the same scene +0xe2c distance. Substitute fld
   [hook constant], preserving x87 stack depth and the existing fallbacks. */
static const unsigned char rv_far_orig[] = {0xd9, 0x80, 0x2c, 0x0e, 0x00, 0x00};
static const float rv_far_distance = 20000.0f;
static unsigned char rv_far_load[] = {0xd9, 0x05, 0, 0, 0, 0};
#define RV_CAMERA_FAR_ADDR 0x08232A68
#define RV_HUD_FAR_ADDR 0x081939D3

/* Shared mesh alpha setup, corresponding to Windows 0x006A56F4. Load 0.9f
   instead of the shape-instance alpha; leave the following fstp and all
   cloak state/interpolation untouched. The old mounted-image-only branch at
   0x08253E5E was not the Windows patch's counterpart. */
static const unsigned char rv_alpha_orig[] = {0xd9, 0x05, 0x94, 0xf8, 0x6b, 0x08};
static const float rv_alpha = 0.9f;
static unsigned char rv_alpha_load[] = {0xd9, 0x05, 0, 0, 0, 0};
#define RV_ALPHA_ADDR 0x0831EE13

#define RV_DEMO_CALLBACK_ADDR 0x082395EC
static const unsigned char rv_demo_callback_orig[] = {
    0x55, 0x8b, 0x15, 0xdc, 0x5c, 0x49, 0x08, 0x89, 0xe5, 0xb0, 0x00,
    0x85, 0xd2, 0x74, 0x0a, 0x83, 0xba, 0x20, 0x82, 0x00, 0x00, 0x00,
    0x0f, 0x95, 0xc0, 0x89, 0xec, 0x5d, 0xc3
};

#define RV_BEACON_CALL_ADDR 0x082FD00E
#define RV_BEACON_RENDER_ADDR 0x082FC49C
static const unsigned char rv_beacon_call_orig[] = {0xe8, 0x89, 0xf4, 0xff, 0xff};
static const unsigned char rv_beacon_render_orig[] = {
    0x55, 0x89, 0xe5, 0x81, 0xec, 0x8c, 0x00, 0x00, 0x00, 0x8b, 0x45, 0x08
};
static unsigned char rv_beacon_call[] = {0xe8, 0, 0, 0, 0};
static void review_beacon_render(void *hud);
typedef unsigned char (*rv_demo_callback_t)(void *, S32, const char **);
static int review_demo_playing(void) {
    static const char *args[] = {"isPlayingDemo"};
    return ((rv_demo_callback_t)RV_DEMO_CALLBACK_ADDR)(NULL, 1, args) != 0;
}

static const struct code_patch review_patches_on[] = {
    {RV_GBEACONS_ADDR, rv_gbeacons_orig, sizeof rv_gbeacons_orig, 0,
     rv_nops, sizeof rv_nops},
    {RV_FFI_BIT0_ADDR, rv_ffi_bit0_orig, sizeof rv_ffi_bit0_orig, 0,
     rv_ffi_bit0_jump, sizeof rv_ffi_bit0_jump},
    {RV_FFI_LOOP_ADDR, rv_ffi_loop_orig, sizeof rv_ffi_loop_orig, 0,
     rv_nops, sizeof rv_nops},
    {RV_CAMERA_FAR_ADDR, rv_far_orig, sizeof rv_far_orig, 0,
     rv_far_load, sizeof rv_far_load},
    {RV_HUD_FAR_ADDR, rv_far_orig, sizeof rv_far_orig, 0,
     rv_far_load, sizeof rv_far_load},
    {RV_ALPHA_ADDR, rv_alpha_orig, sizeof rv_alpha_orig, 0,
     rv_alpha_load, sizeof rv_alpha_load},
    {RV_BEACON_CALL_ADDR, rv_beacon_call_orig, sizeof rv_beacon_call_orig, 0,
     rv_beacon_call, sizeof rv_beacon_call},
};
static const struct code_patch review_patches_off[] = {
    {RV_GBEACONS_ADDR, rv_nops, sizeof rv_nops, 0,
     rv_gbeacons_orig, sizeof rv_gbeacons_orig},
    {RV_FFI_BIT0_ADDR, rv_ffi_bit0_jump, sizeof rv_ffi_bit0_jump, 0,
     rv_ffi_bit0_orig, sizeof rv_ffi_bit0_orig},
    {RV_FFI_LOOP_ADDR, rv_nops, sizeof rv_nops, 0,
     rv_ffi_loop_orig, sizeof rv_ffi_loop_orig},
    {RV_CAMERA_FAR_ADDR, rv_far_load, sizeof rv_far_load, 0,
     rv_far_orig, sizeof rv_far_orig},
    {RV_HUD_FAR_ADDR, rv_far_load, sizeof rv_far_load, 0,
     rv_far_orig, sizeof rv_far_orig},
    {RV_ALPHA_ADDR, rv_alpha_load, sizeof rv_alpha_load, 0,
     rv_alpha_orig, sizeof rv_alpha_orig},
    {RV_BEACON_CALL_ADDR, rv_beacon_call, sizeof rv_beacon_call, 0,
     rv_beacon_call_orig, sizeof rv_beacon_call_orig},
};
#define REVIEW_PATCH_COUNT (sizeof review_patches_on / sizeof review_patches_on[0])

static int review_patches_applied = 0;
static uint32_t review_applied_mask;
enum review_patch_group { REVIEW_CORE, REVIEW_GLOBAL_BEACONS, REVIEW_RANGE, REVIEW_FFI, REVIEW_NO_CLOAK };
static enum review_patch_group review_group(size_t index) {
    uintptr_t address = review_patches_on[index].address;
    if (address == RV_GBEACONS_ADDR) return REVIEW_GLOBAL_BEACONS;
    if (address == RV_CAMERA_FAR_ADDR || address == RV_HUD_FAR_ADDR) return REVIEW_RANGE;
    if (address == RV_FFI_BIT0_ADDR || address == RV_FFI_LOOP_ADDR) return REVIEW_FFI;
    if (address == RV_ALPHA_ADDR) return REVIEW_NO_CLOAK;
    return REVIEW_CORE;
}
static uint32_t review_group_mask(enum review_patch_group group) {
    uint32_t mask = 0;
    for (size_t i = 0; i < REVIEW_PATCH_COUNT; i++)
        if (review_group(i) == group) mask |= 1u << i;
    return mask;
}

/* Playback uses $globalActionMapOnly, so a console can open without accepting
   text. Use that supported input path for review A/B keys instead. Bind only
   vacant keys, and remove only bindings that still point to our functions.
   Loki separates modifier tokens with hyphens before the first space. */
static const char review_shortcuts_script[] =
    "function t2launch_reviewRangeKey(%pressed) {\n"
    " if (!%pressed || !isPlayingDemo()) return;\n"
    " %status = hook_review_range(!hook_option(\"review.extended_range\"));\n"
    " clientCmdBottomPrint(\"REVIEW: \" @ %status, 3, 2);\n"
    "}\n"
    "function t2launch_reviewBeaconKey(%pressed) {\n"
    " if (!%pressed || !isPlayingDemo()) return;\n"
    " %status = hook_review_global_beacons(!hook_option(\"review.global_beacons\"));\n"
    " clientCmdBottomPrint(\"REVIEW: \" @ %status, 3, 2);\n"
    "}\n"
    "function t2launch_reviewFFIKey(%pressed) {\n"
    " if (!%pressed || !isPlayingDemo()) return;\n"
    " %status = hook_review_ffi(!hook_option(\"review.ffi\"));\n"
    " clientCmdBottomPrint(\"REVIEW: \" @ %status, 3, 2);\n"
    "}\n"
    "function t2launch_reviewCloakKey(%pressed) {\n"
    " if (!%pressed || !isPlayingDemo()) return;\n"
    " %status = hook_review_no_cloak(!hook_option(\"review.no_cloak\"));\n"
    " clientCmdBottomPrint(\"REVIEW: \" @ %status, 3, 2);\n"
    "}\n"
    "function t2launch_reviewAutobeaconKey(%pressed) {\n"
    " if (!%pressed || !isPlayingDemo()) return;\n"
    " %status = hook_review_autobeacon(!hook_option(\"review.autobeacon\"));\n"
    " clientCmdBottomPrint(\"REVIEW: \" @ %status, 3, 2);\n"
    "}\n"
    "function t2launch_reviewKeysSet(%active) {\n"
    " if (!isObject(GlobalActionMap)) return \"global action map unavailable\";\n"
    " %keys = \"shift-ctrl f6\\tshift-ctrl f7\\tshift-ctrl f8\\tshift-ctrl f9\\tshift-ctrl f10\";\n"
    " %commands = \"t2launch_reviewRangeKey\\tt2launch_reviewBeaconKey\\tt2launch_reviewFFIKey\\tt2launch_reviewCloakKey\\tt2launch_reviewAutobeaconKey\";\n"
    " %labels = \"range\\tglobal_beacons\\tffi\\tno_cloak\\tautobeacon\";\n"
    " %result = \"\";\n"
    " for (%i = 0; %i < getFieldCount(%keys); %i++) {\n"
    "  %key = getField(%keys, %i); %command = getField(%commands, %i);\n"
    "  %existing = GlobalActionMap.getCommand(\"keyboard\", %key);\n"
    "  if (%active) {\n"
    "   if (%existing $= \"\") GlobalActionMap.bind(\"keyboard\", %key, %command);\n"
    "   else if (%existing !$= %command)\n"
    "    echo(\"[t2_hook] Review shortcut already bound, leaving it alone: \" @ %key);\n"
    "  } else if (%existing $= %command) GlobalActionMap.unbind(\"keyboard\", %key);\n"
    "  %after = GlobalActionMap.getCommand(\"keyboard\", %key);\n"
    "  if (%active) {\n"
    "   if (%after $= %command) %state = \"bound\";\n"
    "   else if (%existing !$= \"\") %state = \"occupied\";\n"
    "   else %state = \"failed\";\n"
    "  } else {\n"
    "   if (%after $= %command) %state = \"still-bound\"; else %state = \"released\";\n"
    "  }\n"
    "  %label = getField(%labels, %i);\n"
    "  %result = trim(%result @ \" \" @ %label @ \"=\" @ %state);\n"
    " }\n"
    " return %result;\n"
    "}\n";
static int review_shortcuts_installed, review_shortcuts_active;
static void update_review_shortcuts(int active) {
    if (!registered || !Con__evaluate || !game_main_thread_known ||
        !pthread_equal(pthread_self(), game_main_thread)) return;
    int wanted = active && !target->dedicated && hook_option_bool("review.shortcuts") &&
                 hook_option_bool("hook.console_commands");
    if (wanted == review_shortcuts_active) return;
    if (!review_shortcuts_installed) {
        if (!wanted) return;
        review_shortcuts_installed = 1;
        Con__evaluate(review_shortcuts_script, 0, "t2launch/review-keys.cs", 0);
    }
    review_shortcuts_active = wanted;
    const char *status = Con__evaluate(wanted ? "t2launch_reviewKeysSet(1);" : "t2launch_reviewKeysSet(0);",
                                       0, NULL, 0);
    fprintf(stderr, "[t2_hook] Demo review shortcuts %s: %s (Ctrl+Shift+F6 range, F7 global beacons, F8 IFF, F9 no-cloak, F10 autobeacon)\n",
            wanted ? "setup" : "cleanup", status && *status ? status : "no binding status returned");
}

/* A native bool callback through Con::evaluate need not produce a fresh string
   result. Verify and call the engine's callback directly instead. */
static void update_review_patches(void) {
    static int prepared;
    int enabled = hook_option_bool("review.cheat_view");
    if (target->dedicated || (!enabled && !review_patches_applied)) return;
    if (!prepared) {
        int protection;
        if (!readable_range(RV_DEMO_CALLBACK_ADDR, sizeof rv_demo_callback_orig, &protection) ||
            !(protection & PROT_EXEC) ||
            memcmp((void *)RV_DEMO_CALLBACK_ADDR, rv_demo_callback_orig,
                   sizeof rv_demo_callback_orig) != 0) {
            fprintf(stderr, "[t2_hook] Demo review refused: unsupported playback callback\n");
            prepared = -1;
            return;
        }
        if (!readable_range(RV_BEACON_RENDER_ADDR, sizeof rv_beacon_render_orig, &protection) ||
            !(protection & PROT_EXEC) ||
            memcmp((void *)RV_BEACON_RENDER_ADDR, rv_beacon_render_orig,
                   sizeof rv_beacon_render_orig) != 0) {
            fprintf(stderr, "[t2_hook] Demo review refused: unsupported beacon renderer\n");
            prepared = -1;
            return;
        }
        uint32_t far_address = (uint32_t)(uintptr_t)&rv_far_distance;
        uint32_t alpha_address = (uint32_t)(uintptr_t)&rv_alpha;
        memcpy(rv_far_load + 2, &far_address, sizeof far_address);
        memcpy(rv_alpha_load + 2, &alpha_address, sizeof alpha_address);
        int32_t call_offset = (int32_t)((uintptr_t)review_beacon_render -
                                       (RV_BEACON_CALL_ADDR + sizeof rv_beacon_call));
        memcpy(rv_beacon_call + 1, &call_offset, sizeof call_offset);
        prepared = 1;
    }
    if (prepared < 0) return;
    uint32_t wanted = 0;
    if (enabled && review_demo_playing()) {
        wanted = review_group_mask(REVIEW_CORE);
        if (hook_option_bool("review.global_beacons")) wanted |= review_group_mask(REVIEW_GLOBAL_BEACONS);
        if (hook_option_bool("review.extended_range")) wanted |= review_group_mask(REVIEW_RANGE);
        if (hook_option_bool("review.ffi")) wanted |= review_group_mask(REVIEW_FFI);
        if (hook_option_bool("review.no_cloak")) wanted |= review_group_mask(REVIEW_NO_CLOAK);
    }
    update_review_shortcuts(wanted != 0 && review_patches_applied);
    if (wanted == review_applied_mask) return;
    /* A refused patch must not flood the log at the frame rate. State checks
       remain per-frame; only a failed write/verification delays its retry. */
    static time_t retry_after;
    time_t now = time(NULL);
    if (now != (time_t)-1 && now < retry_after) return;
    retry_after = now == (time_t)-1 ? 0 : now + 1;
    struct code_patch transitions[REVIEW_PATCH_COUNT];
    size_t count = 0;
    for (size_t i = 0; i < REVIEW_PATCH_COUNT; i++) {
        uint32_t bit = 1u << i;
        if ((wanted ^ review_applied_mask) & bit)
            transitions[count++] = wanted & bit ? review_patches_on[i] : review_patches_off[i];
    }
    if (apply_code_patches(wanted ? "Demo review overlay" : "Demo review overlay (revert)",
                           transitions, count)) {
        review_applied_mask = wanted;
        review_patches_applied = wanted != 0;
        retry_after = 0;
        if (wanted)
            fprintf(stderr, "[t2_hook] Demo review: reconstruction patches enabled (range=%d, global_beacons=%d, ffi=%d, no_cloak=%d)\n",
                    (wanted & review_group_mask(REVIEW_RANGE)) != 0,
                    (wanted & review_group_mask(REVIEW_GLOBAL_BEACONS)) != 0,
                    (wanted & review_group_mask(REVIEW_FFI)) != 0,
                    (wanted & review_group_mask(REVIEW_NO_CLOAK)) != 0);
        else
            fprintf(stderr, "[t2_hook] Demo review: original rendering restored\n");
    }
    update_review_shortcuts(wanted != 0 && review_patches_applied);
}

/* ---- hm2 autobeacon ------------------------------------------------------
   Engine-owned registries are read on the render/game thread. Cache IDs, not
   unguarded position pointers: a ghost can disappear between ranking frames.
   Windows' 64 players + 64 objects, 32 beacons and 20-frame ranking cadence
   are retained. RESEARCH.md documents every field and the render-call ABI. */
#define RV_ID_DICTIONARY 0x084E3320
#define RV_CLIENT_TARGET_SET 0x084E32DC
#define RV_CONNECTION 0x08495CDC
#define RV_PLAYER_LIMIT 64
#define RV_OBJECT_LIMIT 64
#define RV_TARGET_LIMIT (RV_PLAYER_LIMIT + RV_OBJECT_LIMIT)
#define RV_BEACON_LIMIT 32
#define RV_REFRESH_FRAMES 20
#define RV_OTHER_TEAM 69

struct rv_map { uintptr_t start, end; int protection; };
struct rv_memory { struct rv_map maps[512]; size_t count; };
enum rv_kind { RV_PLAYER, RV_SHAPE, RV_WAYPOINT };
struct rv_target {
    void *object;
    uint32_t id;
    enum rv_kind kind;
    int type;
    double rank;
};
static struct {
    uintptr_t connection, stream;
    uint32_t self_id;
    int team;
    unsigned int frame;
    struct rv_target targets[RV_TARGET_LIMIT];
    size_t count;
    int reported, last_beacons, last_targets;
} rv_tracking;
typedef void (*rv_beacon_render_t)(void *);
static rv_beacon_render_t rv_native_beacon_render = (rv_beacon_render_t)RV_BEACON_RENDER_ADDR;

/* One /proc read per draw, rather than opening maps for every object field. */
static int rv_memory_snapshot(struct rv_memory *memory) {
    memory->count = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512], permissions[5];
    unsigned long start, end;
    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) != 3 || permissions[0] != 'r')
            continue;
        if (memory->count == sizeof memory->maps / sizeof memory->maps[0]) {
            fclose(maps);
            return 0;
        }
        struct rv_map *map = &memory->maps[memory->count++];
        map->start = start; map->end = end; map->protection = PROT_READ;
        if (permissions[1] == 'w') map->protection |= PROT_WRITE;
        if (permissions[2] == 'x') map->protection |= PROT_EXEC;
    }
    fclose(maps);
    return 1;
}

static const struct rv_map *rv_memory_range(const struct rv_memory *memory,
                                           const void *pointer, size_t length, int protection) {
    uintptr_t address = (uintptr_t)pointer;
    if (!address || length > UINTPTR_MAX - address) return NULL;
    /* /proc/self/maps is address-ordered; field checks are frequent while
       walking the ID dictionary, so avoid a linear scan per field. */
    size_t low = 0, high = memory->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const struct rv_map *map = &memory->maps[middle];
        if (address < map->start) high = middle;
        else if (address >= map->end) low = middle + 1;
        else return address + length <= map->end &&
                    (map->protection & protection) == protection ? map : NULL;
    }
    return NULL;
}

static uint32_t rv_word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof value);
    return value;
}
static void *rv_pointer(const void *object, size_t offset) {
    return (void *)(uintptr_t)rv_word(object, offset);
}
static float rv_float(const void *object, size_t offset) {
    float value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof value);
    return value;
}
static const char *rv_string(const struct rv_memory *memory, const void *pointer, size_t limit) {
    const struct rv_map *map = rv_memory_range(memory, pointer, 1, PROT_READ);
    if (!map) return NULL;
    size_t available = map->end - (uintptr_t)pointer;
    if (available > limit) available = limit;
    return memchr(pointer, 0, available) ? pointer : NULL;
}

#define SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET 0x2C
#define SHAZBOT_SIM_FIELD_BUCKET_COUNT 19
#define SHAZBOT_SIM_FIELD_ENTRY_STRIDE 12

/* SimObject's virtual getClassRep plus its class-rep name at +0x18. */
static void *rv_class_rep(const struct rv_memory *memory, void *object) {
    if (!rv_memory_range(memory, object, 0x30, PROT_READ)) return NULL;
    void *vtable = rv_pointer(object, 0);
    if (!rv_memory_range(memory, vtable, 12, PROT_READ)) return NULL;
    void *getter = rv_pointer(vtable, 8);
    if (!rv_memory_range(memory, getter, 1, PROT_EXEC)) return NULL;
    void *class_rep = ((void *(*)(void *))getter)(object);
    if (!rv_memory_range(memory, class_rep, 0x1c, PROT_READ)) return NULL;
    return class_rep;
}

static const char *rv_class_name(const struct rv_memory *memory, void *object) {
    void *class_rep = rv_class_rep(memory, object);
    if (!class_rep) return NULL;
    return rv_string(memory, rv_pointer(class_rep, 0x18), 64);
}

/* Locate the registered Field array without assuming this client's concrete
   AbstractClassRep/Vector layout. Torque Field entries are six 32-bit words;
   require an exact registered field-name match before trusting its offset. */
static int rv_persist_field_offset(const struct rv_memory *memory, void *object,
                                   const char *field_name, uint32_t *field_offset) {
    if (!memory || !object || !field_name || !field_offset) return 0;
    void *class_rep = rv_class_rep(memory, object);
    if (!class_rep || !rv_memory_range(memory, class_rep, 0x40, PROT_READ)) return 0;
    for (size_t array_slot = 4; array_slot < 0x40; array_slot += sizeof(uint32_t)) {
        void *fields = rv_pointer(class_rep, array_slot);
        if (!rv_memory_range(memory, fields, 24, PROT_READ)) continue;
        for (size_t count_slot = 4; count_slot < 0x40; count_slot += sizeof(uint32_t)) {
            if (count_slot == array_slot) continue;
            uint32_t field_count = rv_word(class_rep, count_slot);
            if (field_count == 0 || field_count > 512 ||
                !rv_memory_range(memory, fields, (size_t)field_count * 24, PROT_READ))
                continue;
            for (uint32_t index = 0; index < field_count; ++index) {
                void *field = (unsigned char *)fields + (size_t)index * 24;
                const char *name = rv_string(memory, rv_pointer(field, 0), 128);
                if (name && strcmp(name, field_name) == 0) {
                    uint32_t offset = rv_word(field, 8);
                    if (offset > 0x10000) return 0;
                    *field_offset = offset;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* SimObject dynamic fields live in a 19-bucket SimFieldDictionary; each Entry
   is {StringTableEntry name, char *value, Entry *next}. */
static const char *rv_dynamic_field(const struct rv_memory *memory, void *object,
                                    const char *field_name) {
    if (!memory || !object || !field_name ||
        !rv_memory_range(memory, (unsigned char *)object + SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET,
                         sizeof(uint32_t), PROT_READ)) return NULL;
    void *dictionary = rv_pointer(object, SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET);
    if (!rv_memory_range(memory, dictionary,
                         SHAZBOT_SIM_FIELD_BUCKET_COUNT * sizeof(uint32_t), PROT_READ)) return NULL;
    for (size_t bucket = 0; bucket < SHAZBOT_SIM_FIELD_BUCKET_COUNT; bucket++) {
        void *entry = rv_pointer(dictionary, bucket * sizeof(uint32_t));
        void *visited[128];
        size_t visited_count = 0;
        while (entry && visited_count < sizeof visited / sizeof visited[0]) {
            if (!rv_memory_range(memory, entry, SHAZBOT_SIM_FIELD_ENTRY_STRIDE, PROT_READ))
                return NULL;
            for (size_t index = 0; index < visited_count; index++)
                if (visited[index] == entry) return NULL;
            visited[visited_count++] = entry;
            const char *name = rv_string(memory, rv_pointer(entry, 0), 128);
            if (!name) return NULL;
            if (strcmp(name, field_name) == 0)
                return rv_string(memory, rv_pointer(entry, 4), 128);
            entry = rv_pointer(entry, 8);
        }
        if (entry) return NULL; /* Refuse cyclic or excessively long chains. */
    }
    return NULL;
}

static int rv_dynamic_float(const struct rv_memory *memory, void *object,
                            const char *field_name, float *value) {
    if (!value) return 0;
    const char *text = rv_dynamic_field(memory, object, field_name);
    if (!text) return 0;
    char *end = NULL;
    float parsed = strtof(text, &end);
    if (end == text || *end || !isfinite(parsed)) return 0;
    *value = parsed;
    return 1;
}

static void *rv_find_object(const struct rv_memory *memory, void *dictionary, uint32_t id) {
    void *object = rv_pointer(dictionary, (id & 0xfff) * 4);
    for (unsigned int visited = 0; object && visited < 4096; visited++) {
        if (!rv_memory_range(memory, object, 0x24, PROT_READ)) return NULL;
        if (rv_word(object, 0x20) == id) return object;
        object = rv_pointer(object, 0x10);
    }
    return NULL;
}

static int rv_position(const struct rv_memory *memory, void *object, enum rv_kind kind, float xyz[3]) {
    size_t base = kind == RV_WAYPOINT ? 0x38 : 0xa8;
    size_t stride = kind == RV_WAYPOINT ? 4 : 16;
    if (!rv_memory_range(memory, (unsigned char *)object + base, stride * 2 + 4, PROT_READ)) return 0;
    for (int i = 0; i < 3; i++) {
        xyz[i] = rv_float(object, base + i * stride);
        if (!isfinite(xyz[i])) return 0;
    }
    return 1;
}

/* ---- Project Shazbot IPC v1 client telemetry and packet mirroring (opt-in) --
   The exact #25034 client identity and spatial fields are documented in
   RESEARCH.md. This emits Player position, the versioned 32-float Observation,
   actual-radius view cones, and mounted-image muzzle-vector rays plus the
   existing native projectile aim marker's source/point when drawn. Separately,
   the opt-in packet capture mirrors received ConnectionProtocol data datagrams.
   No action/input state is read or applied. The receiver is a separate process;
   sends are non-blocking and samples are dropped on any socket error. */
#define SHAZBOT_CONNECTION 0x08495CDC
#define SHAZBOT_ID_DICTIONARY 0x084E3320
#define SHAZBOT_TARGET_MANAGER 0x08497DA0
#define SHAZBOT_LOCAL_CONTROL_OFFSET 0x825C
#define SHAZBOT_POSITION_X_OFFSET 0xA8
#define SHAZBOT_POSITION_Y_OFFSET 0xB8
#define SHAZBOT_POSITION_Z_OFFSET 0xC8
#define SHAZBOT_TARGET_ID_OFFSET 0x270
#define SHAZBOT_TARGET_INFO_OFFSET 0x26C
#define SHAZBOT_TARGET_ARRAY_OFFSET 0x1A58
#define SHAZBOT_TARGET_STRIDE 0x44
#define SHAZBOT_TARGET_INFO_STRIDE 0x44
#define SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET 0xA258
#define SHAZBOT_TARGET_SENSOR_DATA_OFFSET 0x40
#define SHAZBOT_LOCAL_SENSOR_GROUP_OFFSET 0x8434
#define SHAZBOT_PLAYER_VELOCITY_X_OFFSET 0x958
#define SHAZBOT_PLAYER_VELOCITY_Y_OFFSET 0x95C
#define SHAZBOT_PLAYER_VELOCITY_Z_OFFSET 0x960
#define SHAZBOT_PLAYER_DATABLOCK_OFFSET 0x2D0
#define SHAZBOT_PLAYER_MAX_FORWARD_SPEED_OFFSET 0x380
#define SHAZBOT_PLAYER_ENERGY_OFFSET 0x77C
#define SHAZBOT_PLAYER_DAMAGE_OFFSET 0x7F0
#define SHAZBOT_DATABLOCK_MAX_ENERGY_OFFSET 0x80
#define SHAZBOT_DATABLOCK_MAX_DAMAGE_OFFSET 0x84
#define SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET 0x32B
#define SHAZBOT_ENERGY_PERCENT_CALLBACK 0x0826CEA8
#define SHAZBOT_TARGET_INFO_COUNT 512
#define SHAZBOT_VISIBLE_SHAPEBASE_SET 0x084E32EC
#define SHAZBOT_CLIENT_TARGET_LIST 0x08497DA4
#define SHAZBOT_CLIENT_TARGET_LIST_ENTRIES_OFFSET 0x10
#define SHAZBOT_CLIENT_TARGET_LIST_COUNT_OFFSET 0x210
#define SHAZBOT_CLIENT_TARGET_ENTRY_STRIDE 0x10
#define SHAZBOT_CLIENT_TARGET_LIST_CAPACITY 32
#define SHAZBOT_CLIENT_TARGET_TYPE_OFFSET 0x30
#define SHAZBOT_CLIENT_TARGET_POSITION_OFFSET 0x38
#define SHAZBOT_SENSOR_DETECT_RADIUS_OFFSET 0x4C
#define SHAZBOT_EYE_TRANSFORM_VTABLE_OFFSET 0x160
#define SHAZBOT_CAMERA_FOV_VTABLE_OFFSET 0x1D8
#define SHAZBOT_EYE_CALLBACK 0x0826CD7C
#define SHAZBOT_FOV_CALLBACK 0x0826D4C8
#define SHAZBOT_MUZZLE_VECTOR_CALLBACK 0x0826C96C
#define SHAZBOT_MUZZLE_POINT_CALLBACK 0x0826CA04
#define SHAZBOT_MOUNTED_IMAGE_GETTER 0x082733AC
#define SHAZBOT_TARGET_SENSOR_CALLBACK 0x082E2040
#define SHAZBOT_SENSOR_RADIUS_REGISTRATION 0x082D768E
#define SHAZBOT_AIM_BILLBOARD_CALL 0x082FC32F
#define SHAZBOT_NATIVE_BILLBOARD 0x0810F240
#define SHAZBOT_IMAGE_PROJECTILE_OFFSET 0xCD8
#define SHAZBOT_MOUNTED_IMAGES_OFFSET 0x384
#define SHAZBOT_MOUNTED_IMAGE_STRIDE 0x74
#define SHAZBOT_MOUNTED_IMAGE_COUNT 8
#define SHAZBOT_HEADER_SIZE 28
#define SHAZBOT_PAYLOAD_SIZE 16
#define SHAZBOT_PACKET_SIZE (SHAZBOT_HEADER_SIZE + SHAZBOT_PAYLOAD_SIZE)
#define SHAZBOT_VIEW_PAYLOAD_SIZE 36
#define SHAZBOT_VIEW_PACKET_SIZE (SHAZBOT_HEADER_SIZE + SHAZBOT_VIEW_PAYLOAD_SIZE)
#define SHAZBOT_MUZZLE_AIM_PAYLOAD_SIZE 40
#define SHAZBOT_MUZZLE_AIM_PACKET_SIZE (SHAZBOT_HEADER_SIZE + SHAZBOT_MUZZLE_AIM_PAYLOAD_SIZE)
#define SHAZBOT_HOOK_SERVER_PACKET_KIND 21
#define SHAZBOT_MAX_GAME_PACKET_SIZE 1500
#define SHAZBOT_HOOK_SERVER_PACKET_FRAME_SIZE \
    (SHAZBOT_HEADER_SIZE + 2 + SHAZBOT_MAX_GAME_PACKET_SIZE)
#define SHAZBOT_OBSERVATION_DIMENSIONS 32
#define SHAZBOT_OBSERVATION_PAYLOAD_SIZE (SHAZBOT_OBSERVATION_DIMENSIONS * 4)
#define SHAZBOT_OBSERVATION_PACKET_SIZE (SHAZBOT_HEADER_SIZE + SHAZBOT_OBSERVATION_PAYLOAD_SIZE)
#define SHAZBOT_COMMAND_OBSERVATION_V5_KIND 7
#define SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS 95
#define SHAZBOT_COMMAND_OBSERVATION_V5_PAYLOAD_SIZE (SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS * 4)
#define SHAZBOT_COMMAND_OBSERVATION_V5_PACKET_SIZE \
    (SHAZBOT_HEADER_SIZE + SHAZBOT_COMMAND_OBSERVATION_V5_PAYLOAD_SIZE)
#define SHAZBOT_COMMAND_OBSERVATION_V6_KIND 10
#define SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS 95
#define SHAZBOT_COMMAND_OBSERVATION_V6_PAYLOAD_SIZE (SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS * 4)
#define SHAZBOT_COMMAND_OBSERVATION_V6_PACKET_SIZE \
    (SHAZBOT_HEADER_SIZE + SHAZBOT_COMMAND_OBSERVATION_V6_PAYLOAD_SIZE)
#define SHAZBOT_COMMAND_OBSERVATION_V7_KIND 11
#define SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS 95
#define SHAZBOT_COMMAND_OBSERVATION_V7_PAYLOAD_SIZE (SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS * 4)
#define SHAZBOT_COMMAND_OBSERVATION_V7_PACKET_SIZE \
    (SHAZBOT_HEADER_SIZE + SHAZBOT_COMMAND_OBSERVATION_V7_PAYLOAD_SIZE)
#define SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_BASE 61
#define SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_PER_SLOT 5
#define SHAZBOT_V7_WEAPON_SLOT_COUNT 6
#define SHAZBOT_NOTIFICATION_KIND 8
#define SHAZBOT_NOTIFICATION_PAYLOAD_SIZE 4
#define SHAZBOT_NOTIFICATION_PACKET_SIZE (SHAZBOT_HEADER_SIZE + SHAZBOT_NOTIFICATION_PAYLOAD_SIZE)
#define SHAZBOT_NOTIFICATION_CAPTURE_SAVED 1
#define SHAZBOT_NOTIFICATION_CAPTURE_START 2
#define SHAZBOT_NOTIFICATION_SOCKET "/tmp/shazbot-notify.sock"
#define SHAZBOT_V5_MOUNTED_WEAPON_ID_BITS_BASE 61
#define SHAZBOT_V5_MOUNTED_WEAPON_ID_VALID 77
#define SHAZBOT_V5_MOUNTED_PACK_ID_BITS_BASE 78
#define SHAZBOT_V5_MOUNTED_PACK_ID_VALID 94
#define SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_BITS_BASE 61
#define SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_BITS_COUNT 16
#define SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_VALID 77
#define SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_BITS_BASE 78
#define SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_BITS_COUNT 16
#define SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_VALID 94
#define SHAZBOT_MOVE_ACTION_DIMENSIONS 13
#define SHAZBOT_COMMAND_ACTION_BASE SHAZBOT_MOVE_ACTION_DIMENSIONS
#define SHAZBOT_COMMAND_ACTIONS 26
#define SHAZBOT_ACTION_DIMENSIONS (SHAZBOT_MOVE_ACTION_DIMENSIONS + SHAZBOT_COMMAND_ACTIONS)
#define SHAZBOT_ACTION_PAYLOAD_SIZE (SHAZBOT_ACTION_DIMENSIONS * 4)
#define SHAZBOT_ACTION_PACKET_SIZE (SHAZBOT_HEADER_SIZE + SHAZBOT_ACTION_PAYLOAD_SIZE)
#define SHAZBOT_COMMAND_USE_BACKPACK 8
#define SHAZBOT_SAMPLE_PERIOD_NS 50000000ULL  /* 20 Hz maximum */

typedef ssize_t (*shazbot_sendto_fn)(int, const void *, size_t, int,
                                     const struct sockaddr *, socklen_t);
static int shazbot_socket_fd = -1;
static char shazbot_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static struct sockaddr_un shazbot_address;
static socklen_t shazbot_address_length;
static uint64_t shazbot_sequence;
static uint64_t shazbot_last_attempt_ns;
static int shazbot_socket_error_reported;
static int shazbot_spatial_anchor_reported;
static int shazbot_observation_anchor_reported;
static int shazbot_target_reported;
static int shazbot_poll_gate_reported;
static int shazbot_memory_reported;
static int shazbot_globals_reported;
static int shazbot_socket_open_reported;
static int shazbot_telemetry_sent_reported;
static int shazbot_observation_collect_reported;
static int shazbot_observation_sent_reported;
static int shazbot_command_context_reported;
static int shazbot_semantic_command_context_reported;
static int shazbot_weapon_slot_context_reported;
static unsigned shazbot_semantic_name_diagnostic_reported;
static int shazbot_hud_weapon_context_valid;
static char shazbot_hud_weapon_order[16][64];
static size_t shazbot_hud_weapon_count;
static char shazbot_hud_active_weapon[64];
static int shazbot_hud_runtime_script_loaded;
static int shazbot_hud_weapon_clear_reported;
static int shazbot_hud_weapon_item_reported;
static int shazbot_hud_weapon_active_reported;
static unsigned shazbot_hud_weapon_item_events_logged;
static int shazbot_notification_socket_fd = -1;
static int shazbot_notification_socket_error_reported;
static int shazbot_notification_sequence_valid;
static uint64_t shazbot_notification_last_sequence;
static int shazbot_auto_command_capture_active;
static int shazbot_auto_command_capture_reported;
static int shazbot_command_label_capture_active;
static int shazbot_autoplay_demo_active;
static int shazbot_action_socket_fd = -1;
static char shazbot_action_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int shazbot_action_socket_error_reported;
static int shazbot_action_connected_reported;
static int shazbot_action_sequence_valid;
static int shazbot_action_trigger_known;
static uint64_t shazbot_action_last_sequence;
static uint64_t shazbot_action_last_receive_ns;
static int shazbot_action_trigger_state[7];
static int shazbot_action_command_known;
static int shazbot_action_command_state = -1;
static int shazbot_local_player_valid;
static char shazbot_player_failure_state[160];
static uint64_t shazbot_player_status_time_ns;
static int shazbot_demo_state_initialized;
static int shazbot_demo_state;
static int shazbot_aim_hook_attempted;
static int shazbot_aim_hook_installed;
static int shazbot_spatial_context_valid;
static struct rv_memory shazbot_spatial_memory;
static void *shazbot_spatial_player;
static uint32_t shazbot_spatial_player_id;
static float shazbot_spatial_detection_radius;
static uint64_t shazbot_spatial_epoch;
struct shazbot_aim_key { uint32_t target_id, image_slot; uint64_t epoch; };
static struct shazbot_aim_key shazbot_aim_keys[64];

static void shazbot_report_once(int *reported, const char *message) {
    if (!*reported) {
        fprintf(stderr, "[t2_hook] Shazbot: %s\n", message);
        *reported = 1;
    }
}

static uint16_t shazbot_get_u16le(const unsigned char *data) {
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

static uint32_t shazbot_get_u32le(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t shazbot_get_u64le(const unsigned char *data) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; i++) value |= (uint64_t)data[i] << (i * 8);
    return value;
}

static float shazbot_get_f32le(const unsigned char *data) {
    uint32_t bits = shazbot_get_u32le(data);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int shazbot_decode_action_frame(const unsigned char *frame, size_t length,
                                       uint64_t *sequence,
                                       float action[SHAZBOT_ACTION_DIMENSIONS]) {
    if (!frame || !sequence || !action || length != SHAZBOT_ACTION_PACKET_SIZE ||
        memcmp(frame, "SHZB", 4) != 0 || shazbot_get_u16le(frame + 4) != 1 ||
        shazbot_get_u16le(frame + 6) != 2 ||
        shazbot_get_u32le(frame + 8) != SHAZBOT_ACTION_PAYLOAD_SIZE) return 0;
    *sequence = shazbot_get_u64le(frame + 12);
    for (size_t index = 0; index < SHAZBOT_ACTION_DIMENSIONS; index++) {
        action[index] = shazbot_get_f32le(frame + SHAZBOT_HEADER_SIZE + index * 4);
        if (!isfinite(action[index])) return 0;
        if (index < 3 && (action[index] < -1.0f || action[index] > 1.0f)) return 0;
        if (index >= 3 && index < 6 && fabsf(action[index]) > 6.2831855f) return 0;
        if (index >= 6 && action[index] != 0.0f && action[index] != 1.0f) return 0;
    }
    return 1;
}

static int shazbot_format_action_script(const float action[SHAZBOT_ACTION_DIMENSIONS],
                                        const int previous[7], int previous_known,
                                        char *script, size_t script_size,
                                        int next_triggers[7]) {
    if (!action || !script || !script_size || !next_triggers) return 0;
    static const char *const trigger_functions[6] = {
        "mouseFire", "altTrigger", "jump", "mouseJet", NULL, NULL
    };
    next_triggers[0] = action[6] >= 0.5f; /* free-look */
    for (size_t index = 0; index < 6; index++)
        next_triggers[index + 1] = action[7 + index] >= 0.5f;
    int used = snprintf(script, script_size,
                        "$mvPitch += %.8g; $mvYaw += %.8g; $mvRoll += %.8g; "
                        "moveleft(%.8g); moveright(%.8g); movebackward(%.8g); "
                        "moveforward(%.8g); movedown(%.8g); moveup(%.8g);",
                        (double)action[3], (double)action[4], (double)action[5],
                         (double)(action[0] < 0.0f ? -action[0] : 0.0f),
                         (double)(action[0] > 0.0f ? action[0] : 0.0f),
                         (double)(action[1] < 0.0f ? -action[1] : 0.0f),
                         (double)(action[1] > 0.0f ? action[1] : 0.0f),
                         (double)(action[2] < 0.0f ? -action[2] : 0.0f),
                         (double)(action[2] > 0.0f ? action[2] : 0.0f));
    if (used < 0 || (size_t)used >= script_size) return 0;
    if (!previous_known || next_triggers[0] != previous[0]) {
        int amount = snprintf(script + used, script_size - (size_t)used,
                              " toggleFreeLook(%d);", next_triggers[0]);
        if (amount < 0 || (size_t)amount >= script_size - (size_t)used) return 0;
        used += amount;
    }
    for (size_t index = 0; index < 6; index++) {
        if (previous_known && next_triggers[index + 1] == previous[index + 1]) continue;
        if (trigger_functions[index]) {
            int amount = snprintf(script + used, script_size - (size_t)used,
                                  " %s(%d);", trigger_functions[index], next_triggers[index + 1]);
            if (amount < 0 || (size_t)amount >= script_size - (size_t)used) return 0;
            used += amount;
        } else {
            const size_t trigger_number = index;
            int amount = snprintf(script + used, script_size - (size_t)used,
                                  " $mvTriggerCount%zu += (($mvTriggerCount%zu & 1) == %d) ? 2 : 1;",
                                  trigger_number, trigger_number, next_triggers[index + 1]);
            if (amount < 0 || (size_t)amount >= script_size - (size_t)used) return 0;
            used += amount;
        }
    }
    return used > 0;
}

static const char *const shazbot_command_scripts[SHAZBOT_COMMAND_ACTIONS] = {
    "useFirstWeaponSlot(1);", "useSecondWeaponSlot(1);", "useThirdWeaponSlot(1);",
    "useFourthWeaponSlot(1);", "useFifthWeaponSlot(1);", "useSixthWeaponSlot(1);",
    "nextWeapon(1);", "prevWeapon(1);", NULL,
    "useRepairKit(1);", "useTargetingLaser(1);",
    "quickPackEnergyPack(1);", "quickPackRepairPack(1);", "quickPackShieldPack(1);",
    "quickPackCloakPack(1);", "quickPackJammerPack(1);", "quickPackAmmoPack(1);",
    "quickPackSatchelCharge(1);", "quickPackDeployableStation(1);",
    "quickPackIndoorTurret(1);", "quickPackOutdoorTurret(1);",
    "quickPackMotionSensor(1);", "quickPackPulse(1);",
    "throwWeapon(1);", "throwPack(1);", "throwFlag(1);",
};
static const char *const shazbot_command_names[SHAZBOT_COMMAND_ACTIONS] = {
    "weapon_slot_1", "weapon_slot_2", "weapon_slot_3", "weapon_slot_4",
    "weapon_slot_5", "weapon_slot_6", "weapon_next", "weapon_previous",
    "backpack_use", "repair_kit", "targeting_laser", "pack_energy",
    "pack_repair", "pack_shield", "pack_cloak", "pack_jammer", "pack_ammo",
    "pack_satchel", "pack_station", "pack_indoor_turret", "pack_outdoor_turret",
    "pack_motion_sensor", "pack_pulse", "drop_weapon", "drop_pack", "drop_flag",
};

static void shazbot_release_command_action(void) {
    if (shazbot_action_command_known &&
        shazbot_action_command_state == SHAZBOT_COMMAND_USE_BACKPACK && Con__evaluate)
        (void)Con__evaluate("useBackPack(0);", 0, "t2launch/shazbot_command.cs", 0);
    shazbot_action_command_state = -1;
    shazbot_action_command_known = 0;
}

static void shazbot_apply_command_action(const float action[SHAZBOT_ACTION_DIMENSIONS]) {
    if (!action) return;
    int selected = -1;
    for (size_t index = 0; index < SHAZBOT_COMMAND_ACTIONS; index++) {
        if (action[SHAZBOT_COMMAND_ACTION_BASE + index] >= 0.5f) {
            selected = (int)index;
            break;
        }
    }
    if (shazbot_action_command_known && selected == shazbot_action_command_state) return;
    if (shazbot_action_command_known &&
        shazbot_action_command_state == SHAZBOT_COMMAND_USE_BACKPACK && Con__evaluate)
        (void)Con__evaluate("useBackPack(0);", 0, "t2launch/shazbot_command.cs", 0);
    shazbot_action_command_state = selected;
    shazbot_action_command_known = 1;
    if (selected < 0 || !Con__evaluate) return;
    if (selected == SHAZBOT_COMMAND_USE_BACKPACK) {
        (void)Con__evaluate("useBackPack(1);", 0, "t2launch/shazbot_command.cs", 0);
        fprintf(stderr, "[t2_hook] Shazbot command action: %s\n",
                shazbot_command_names[selected]);
        return;
    }
    (void)Con__evaluate(shazbot_command_scripts[selected], 0,
                        "t2launch/shazbot_command.cs", 0);
    fprintf(stderr, "[t2_hook] Shazbot command action: %s\n",
            shazbot_command_names[selected]);
}

static int shazbot_open_action_socket(void) {
    const char *configured = hook_option_str("telemetry.shazbot_action_socket");
    if (!configured || !configured[0]) configured = "/tmp/shazbot-action.sock";
    size_t length = strnlen(configured, sizeof shazbot_address.sun_path);
    if (!length || length >= sizeof shazbot_address.sun_path) {
        errno = ENAMETOOLONG;
        return 0;
    }
    if (shazbot_action_socket_fd >= 0 && strcmp(configured, shazbot_action_socket_path) == 0)
        return 1;
    if (shazbot_action_socket_fd >= 0) close(shazbot_action_socket_fd);
    shazbot_action_socket_fd = -1;

    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, configured, length + 1);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    if (bind(fd, (const struct sockaddr *)&address, address_length) != 0) {
        int bind_error = errno;
        if (bind_error == EADDRINUSE) {
            struct stat info;
            if (lstat(configured, &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == geteuid()) {
                int probe = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
                int active = probe >= 0 &&
                    connect(probe, (const struct sockaddr *)&address, address_length) == 0;
                if (probe >= 0) close(probe);
                if (!active && unlink(configured) == 0 &&
                    bind(fd, (const struct sockaddr *)&address, address_length) == 0) {
                    bind_error = 0;
                } else {
                    errno = bind_error;
                }
            }
        }
        if (bind_error != 0) {
            close(fd);
            errno = bind_error;
            return 0;
        }
    }
    (void)chmod(configured, S_IRUSR | S_IWUSR);
    shazbot_action_socket_fd = fd;
    memcpy(shazbot_action_socket_path, configured, length + 1);
    return 1;
}

static void shazbot_apply_client_action(const float action[SHAZBOT_ACTION_DIMENSIONS],
                                        uint64_t sequence, uint64_t now_ns) {
    char script[512];
    int next_triggers[7];
    if (!shazbot_format_action_script(action, shazbot_action_trigger_state,
                                      shazbot_action_trigger_known,
                                      script, sizeof script, next_triggers)) return;
    if (Con__evaluate)
        (void)Con__evaluate(script, 0, "t2launch/shazbot_action.cs", 0);
    memcpy(shazbot_action_trigger_state, next_triggers, sizeof next_triggers);
    shazbot_action_trigger_known = 1;
    shazbot_apply_command_action(action);
    shazbot_action_last_sequence = sequence;
    shazbot_action_sequence_valid = 1;
    shazbot_action_last_receive_ns = now_ns;
    shazbot_report_once(&shazbot_action_connected_reported,
                        "same-client action input is active");
}

static void shazbot_release_client_triggers(void) {
    if (shazbot_action_trigger_known && Con__evaluate) {
        (void)Con__evaluate(
            "moveleft(0); moveright(0); movebackward(0); moveforward(0); movedown(0); moveup(0); "
            "toggleFreeLook(0); mouseFire(0); altTrigger(0); jump(0); mouseJet(0); "
            "$mvTriggerCount4 += (($mvTriggerCount4 & 1) == 0) ? 2 : 1; "
            "$mvTriggerCount5 += (($mvTriggerCount5 & 1) == 0) ? 2 : 1;",
            0, "t2launch/shazbot_action.cs", 0);
        memset(shazbot_action_trigger_state, 0, sizeof shazbot_action_trigger_state);
    }
    shazbot_action_trigger_known = 0;
    shazbot_release_command_action();
}

static void shazbot_poll_actions(void) {
    if (!hook_option_bool("telemetry.shazbot_actions")) {
        shazbot_release_client_triggers();
        return;
    }
    if (target != &game_targets[0] || target->dedicated ||
        !hook_option_bool("telemetry.shazbot")) {
        shazbot_release_client_triggers();
        return;
    }
    if (!shazbot_local_player_valid || shazbot_demo_state) {
        unsigned char discard[256];
        if (shazbot_action_socket_fd >= 0)
            while (recv(shazbot_action_socket_fd, discard, sizeof discard, 0) >= 0) {}
        shazbot_release_client_triggers();
        return;
    }

    /* Delay binding the action input socket until a live local Player exists.
       This keeps opt-in command polling out of menus and online join setup. */
    if (!shazbot_open_action_socket()) {
        if (!shazbot_action_socket_error_reported) {
            fprintf(stderr, "[t2_hook] Shazbot action socket unavailable: %s\n", strerror(errno));
            shazbot_action_socket_error_reported = 1;
        }
        return;
    }

    unsigned char frame[256];
    for (;;) {
        ssize_t received = recv(shazbot_action_socket_fd, frame, sizeof frame, 0);
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
                !shazbot_action_socket_error_reported) {
                fprintf(stderr, "[t2_hook] Shazbot action receive failed: %s\n", strerror(errno));
                shazbot_action_socket_error_reported = 1;
            }
            if (errno == EINTR) continue;
            break;
        }
        uint64_t sequence = 0;
        float action[SHAZBOT_ACTION_DIMENSIONS];
        if (!shazbot_decode_action_frame(frame, (size_t)received, &sequence, action)) {
            shazbot_report_once(&shazbot_action_socket_error_reported,
                                "discarded malformed or out-of-range action frame");
            continue;
        }
        if (shazbot_action_sequence_valid && sequence <= shazbot_action_last_sequence) continue;
        struct timespec now;
        uint64_t now_ns = 0;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0)
            now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
        shazbot_apply_client_action(action, sequence, now_ns);
    }

    if (shazbot_action_sequence_valid && shazbot_action_last_receive_ns) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0) {
            uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
            if (now_ns >= shazbot_action_last_receive_ns &&
                now_ns - shazbot_action_last_receive_ns > 300000000ULL) {
                shazbot_release_client_triggers();
                shazbot_action_sequence_valid = 0;
            }
        }
    }
}

static size_t shazbot_aim_key_cursor;
static unsigned char shazbot_aim_billboard_call_patch[5] = {0xe8};
static void shazbot_aim_billboard_wrapper(const float *hit, float width, float rotation);
static void (*shazbot_native_billboard)(const float *, float, float) =
    (void (*)(const float *, float, float))SHAZBOT_NATIVE_BILLBOARD;

static void shazbot_put_u16le(unsigned char *out, uint16_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
}

static void shazbot_put_u32le(unsigned char *out, uint32_t value) {
    for (unsigned int i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static void shazbot_put_u64le(unsigned char *out, uint64_t value) {
    for (unsigned int i = 0; i < 8; i++) out[i] = (unsigned char)(value >> (i * 8));
}

static void shazbot_put_f32le(unsigned char *out, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    shazbot_put_u32le(out, bits);
}

/* IPC v1 telemetry_point: SHZB, version 1, kind 3, 16-byte payload, seq/timestamp,
   then little-endian x/y/z/intensity IEEE-754 FP32. */
static size_t shazbot_encode_telemetry(unsigned char out[SHAZBOT_PACKET_SIZE],
                                       uint64_t sequence, uint64_t monotonic_time_ns,
                                       const float xyz[3], float intensity) {
    if (!out || !xyz || !isfinite(xyz[0]) || !isfinite(xyz[1]) ||
        !isfinite(xyz[2]) || !isfinite(intensity)) return 0;
    memcpy(out, "SHZB", 4);
    shazbot_put_u16le(out + 4, 1);
    shazbot_put_u16le(out + 6, 3);
    shazbot_put_u32le(out + 8, SHAZBOT_PAYLOAD_SIZE);
    shazbot_put_u64le(out + 12, sequence);
    shazbot_put_u64le(out + 20, monotonic_time_ns);
    shazbot_put_f32le(out + 28, xyz[0]);
    shazbot_put_f32le(out + 32, xyz[1]);
    shazbot_put_f32le(out + 36, xyz[2]);
    shazbot_put_f32le(out + 40, intensity);
    return SHAZBOT_PACKET_SIZE;
}

static size_t shazbot_encode_view_cone(unsigned char out[SHAZBOT_VIEW_PACKET_SIZE],
                                       uint64_t sequence, uint64_t monotonic_time_ns,
                                       uint32_t object_id, const float origin[3],
                                       const float forward[3], float fov_degrees,
                                       float detection_radius) {
    if (!out || !origin || !forward || !object_id ||
        !isfinite(fov_degrees) || fov_degrees <= 0.0f || fov_degrees >= 180.0f ||
        !isfinite(detection_radius) || detection_radius <= 0.0f) return 0;
    float direction_length = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (!isfinite(origin[i]) || !isfinite(forward[i])) return 0;
        direction_length += forward[i] * forward[i];
    }
    if (!isfinite(direction_length) || direction_length < 1.0e-8f) return 0;

    memcpy(out, "SHZB", 4);
    shazbot_put_u16le(out + 4, 1);
    shazbot_put_u16le(out + 6, 4); /* view_cone */
    shazbot_put_u32le(out + 8, SHAZBOT_VIEW_PAYLOAD_SIZE);
    shazbot_put_u64le(out + 12, sequence);
    shazbot_put_u64le(out + 20, monotonic_time_ns);
    shazbot_put_u32le(out + 28, object_id);
    for (int i = 0; i < 3; i++) shazbot_put_f32le(out + 32 + i * 4, origin[i]);
    for (int i = 0; i < 3; i++) shazbot_put_f32le(out + 44 + i * 4, forward[i]);
    shazbot_put_f32le(out + 56, fov_degrees);
    shazbot_put_f32le(out + 60, detection_radius);
    return SHAZBOT_VIEW_PACKET_SIZE;
}

static size_t shazbot_encode_muzzle_aim(unsigned char out[SHAZBOT_MUZZLE_AIM_PACKET_SIZE],
                                        uint64_t sequence, uint64_t monotonic_time_ns,
                                        uint32_t object_id, uint32_t image_slot,
                                        uint32_t target_id,
                                        const float muzzle[3], const float aim[3],
                                        float detection_radius) {
    if (!out || !muzzle || !aim || !object_id || image_slot >= SHAZBOT_MOUNTED_IMAGE_COUNT ||
        !isfinite(detection_radius) || detection_radius <= 0.0f) return 0;
    for (int i = 0; i < 3; i++)
        if (!isfinite(muzzle[i]) || !isfinite(aim[i])) return 0;

    memcpy(out, "SHZB", 4);
    shazbot_put_u16le(out + 4, 1);
    shazbot_put_u16le(out + 6, 5); /* muzzle_aim */
    shazbot_put_u32le(out + 8, SHAZBOT_MUZZLE_AIM_PAYLOAD_SIZE);
    shazbot_put_u64le(out + 12, sequence);
    shazbot_put_u64le(out + 20, monotonic_time_ns);
    shazbot_put_u32le(out + 28, object_id);
    shazbot_put_u32le(out + 32, image_slot);
    shazbot_put_u32le(out + 36, target_id);
    for (int i = 0; i < 3; i++) shazbot_put_f32le(out + 40 + i * 4, muzzle[i]);
    for (int i = 0; i < 3; i++) shazbot_put_f32le(out + 52 + i * 4, aim[i]);
    shazbot_put_f32le(out + 64, detection_radius);
    return SHAZBOT_MUZZLE_AIM_PACKET_SIZE;
}

/* The sender parameter is injectable so tests can deterministically model an
   absent receiver (ENOENT) and a full socket (EAGAIN). */
static int shazbot_send_datagram(int enabled, int fd,
                                 const struct sockaddr_un *address,
                                 socklen_t address_length,
                                 const unsigned char *packet, size_t length,
                                 shazbot_sendto_fn sender) {
    if (!enabled || fd < 0 || !address || !packet || !length || !sender) return 0;
    ssize_t sent = sender(fd, packet, length, MSG_DONTWAIT | MSG_NOSIGNAL,
                          (const struct sockaddr *)address, address_length);
    return sent == (ssize_t)length;
}

/* Validate the active client connection's readable control-object slot, then
   require an exact local Player class, object-dictionary identity and the
   researched world-transform reads before touching player fields. A spectator
   camera or stale ghost emits nothing. */
static int shazbot_local_player_position_detail(const struct rv_memory *memory,
                                                void *connection, void *dictionary,
                                                float xyz[3], uint32_t *player_id,
                                                void **player_out,
                                                const char **failure_reason) {
#define SHAZBOT_PLAYER_FAIL(reason) do { \
    if (failure_reason) *failure_reason = reason; \
    return 0; \
} while (0)
    if (!memory || !xyz || !player_id)
        SHAZBOT_PLAYER_FAIL("invalid local Player validation arguments");
    if (!connection)
        SHAZBOT_PLAYER_FAIL("waiting for an active server connection");
    if (!rv_memory_range(memory, connection,
            SHAZBOT_LOCAL_CONTROL_OFFSET + sizeof(uint32_t), PROT_READ))
        SHAZBOT_PLAYER_FAIL("active connection pointer does not reference readable memory");
    void *player = rv_pointer(connection, SHAZBOT_LOCAL_CONTROL_OFFSET);
    if (!rv_memory_range(memory, player, 0x30, PROT_READ))
        SHAZBOT_PLAYER_FAIL("GameConnection has no readable control object");
    const char *class_name = rv_class_name(memory, player);
    if (!class_name || strcmp(class_name, "Player") != 0)
        SHAZBOT_PLAYER_FAIL("control object is not a Player");
    uint32_t id = rv_word(player, 0x20);
    if (!id || !rv_memory_range(memory, dictionary, 4096 * sizeof(uint32_t), PROT_READ) ||
        rv_find_object(memory, dictionary, id) != player)
        SHAZBOT_PLAYER_FAIL("Player is not present in the client object dictionary");
    if (!rv_memory_range(memory, (unsigned char *)player + SHAZBOT_POSITION_X_OFFSET,
                         SHAZBOT_POSITION_Z_OFFSET - SHAZBOT_POSITION_X_OFFSET + sizeof(float),
                         PROT_READ))
        SHAZBOT_PLAYER_FAIL("Player world position is unreadable");
    xyz[0] = rv_float(player, SHAZBOT_POSITION_X_OFFSET);
    xyz[1] = rv_float(player, SHAZBOT_POSITION_Y_OFFSET);
    xyz[2] = rv_float(player, SHAZBOT_POSITION_Z_OFFSET);
    if (!isfinite(xyz[0]) || !isfinite(xyz[1]) || !isfinite(xyz[2]))
        SHAZBOT_PLAYER_FAIL("Player world position is non-finite");
    *player_id = id;
    if (player_out) *player_out = player;
    if (failure_reason) *failure_reason = NULL;
#undef SHAZBOT_PLAYER_FAIL
    return 1;
}

static int shazbot_local_player_position(const struct rv_memory *memory,
                                          void *connection, void *dictionary,
                                          float xyz[3], uint32_t *player_id,
                                          void **player_out) {
    return shazbot_local_player_position_detail(memory, connection, dictionary,
                                                 xyz, player_id, player_out, NULL);
}

static const unsigned char shazbot_eye_callback_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x74,0x53,0x8b,0x55,0x08,0x8d,0x5d,0xc0,
    0x83,0xc4,0xf8,0x8b,0x02,0x53,0x52
};
static const unsigned char shazbot_fov_callback_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x08,0x8b,0x55,0x08,0xf6,0x42,0x40,0x02,
    0x75,0x10,0x8b,0x02,0x83,0xc4,0xf4,0x52,0x8b,0x80,0xd8,0x01,0x00,0x00
};
static const unsigned char shazbot_muzzle_vector_callback_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x24,0x53,0x8b,0x45,0x10,0x8b,0x5d,0x08,
    0x83,0xc4,0xf4,0x8b,0x40,0x08,0x50,0xe8,0x07,0x2d,0xf6,0xff
};
static const unsigned char shazbot_muzzle_point_callback_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x24,0x53,0x8b,0x45,0x10,0x83,0xc4,0xf4,
    0x8b,0x40,0x08,0x50,0xe8,0x72,0x2c,0xf6,0xff
};
static const unsigned char shazbot_muzzle_vector_slot_bytes[] = {
    0x8b,0x13,0x83,0xc4,0xfc,0x8d,0x45,0xf4,0x50,0x51,0x53,
    0x8b,0x82,0x54,0x01,0x00,0x00,0xff,0xd0
};
static const unsigned char shazbot_muzzle_point_helper_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x50,0x8b,0x4d,0x08,0x56,0x53,0x8b,0x75,0x0c,
    0x8b,0x5d,0x10
};
static const unsigned char shazbot_mounted_image_getter_bytes[] = {
    0x55,0x89,0xe5,0x6b,0x55,0x0c,0x74,0x8b,0x45,0x08,0x05,0x84,0x03,
    0x00,0x00,0x89,0xec,0x5d,0x8b,0x04,0x02,0xc3
};
static const unsigned char shazbot_target_sensor_bytes[] = {
    0x89,0xd0,0xc1,0xe0,0x04,0x01,0xd0,0x8d,0x04,0x85,0x58,0x1a,0x00,0x00,
    0x03,0x05,0xa0,0x7d,0x49,0x08,0x8b,0x40,0x40,0x85,0xc0,0x75,0x07,
    0xb8,0xff,0xff,0xff,0xff,0xeb,0x03,0x8b,0x40,0x20,0x89,0xec,0x5d,0xc3
};
static const unsigned char shazbot_sensor_radius_field_bytes[] = {
    0x6a,0x00,0x6a,0x01,0x6a,0x4c,0x6a,0x05,0x68,0xa2,0xf7,0x46,0x08,
    0xe8,0x6c,0xe8,0xe0,0xff
};
static const unsigned char shazbot_velocity_getter_callback_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x20,0x56,0x53,0x8b,0x45,0x08,0x8d,0x5d,0xf4,
    0x83,0xc4,0xf8,0x8b,0x10,0x50,0x53,0x8b,0x82,0xec,0x00,0x00,0x00,0xff,
    0xd0,0x6a,0x64,0xe8,0xac,0x50,0xe8,0xff,0xd9,0x43,0x08,0x83,0xc4,0xf4,
    0x83,0xec,0x08,0xdd,0x1c,0x24,0xd9,0x43,0x04,0x83,0xec,0x08,0xdd,0x1c,
    0x24,0xd9,0x45,0xf4,0x89,0xc6,0x83,0xec,0x08,0xdd,0x1c,0x24,0x68,0x9e,
    0x9b,0x45,0x08,0x6a,0x64,0x56,0xe8,0x4f,0x27,0xf6,0xff,0x89,0xf0,0x8d,
    0x65,0xd8,0x5b,0x5e,0x89,0xec,0x5d,0xc3
};
static const unsigned char shazbot_velocity_x_init_bytes[] = {
    0xc7,0x82,0x58,0x09,0x00,0x00,0x00,0x00,0x00,0x00
};
static const unsigned char shazbot_velocity_y_init_bytes[] = {
    0xc7,0x82,0x5c,0x09,0x00,0x00,0x00,0x00,0x00,0x00
};
static const unsigned char shazbot_velocity_z_init_bytes[] = {
    0xc7,0x82,0x60,0x09,0x00,0x00,0x00,0x00,0x00,0x00
};
static const unsigned char shazbot_energy_percent_callback_bytes[] = {
    0x55,0x89,0xe5,0x8b,0x4d,0x08,0x8b,0x91,0xd0,0x02,0x00,0x00,
    0x80,0xba,0x2b,0x03,0x00,0x00,0x00,0x75,0x23,0xd9,0xee,0xd9,0x82,
    0x80,0x00,0x00,0x00,0xd8,0xd1,0xdf,0xe0,0x80,0xe4,0x45,0xfe,
    0xcc,0x80,0xfc,0x40,0xdd,0xd9,0x72,0x5f,0xd9,0x81,0x7c,0x07,
    0x00,0x00
};
static const unsigned char shazbot_damage_value_method_bytes[] = {
    0x55,0x89,0xe5,0x8b,0x45,0x08,0x89,0xec,0x8b,0x90,0xd0,0x02,0x00,0x00,0x5d,
    0xd9,0x80,0xf0,0x07,0x00,0x00,0xd8,0xb2,0x84,0x00,0x00,0x00,0xc3
};
static const unsigned char shazbot_max_forward_speed_field_bytes[] = {
    0x68,0x80,0x03,0x00,0x00,0x6a,0x05,0x68,0x34,0x67,0x45,0x08
};
static const unsigned char shazbot_energy_mount_path_bytes[] = {
    0x8b,0x91,0xe4,0x07,0x00,0x00,0x85,0xd2,0x74,0x4c,
    0x8b,0x82,0xd0,0x02,0x00,0x00,0xd9,0xee,0xd9,0x80,0x80,0x00,0x00,0x00,
    0xd8,0xd1,0xdf,0xe0,0x80,0xe4,0x45,0xfe,0xcc,0x80,0xfc,0x40,0xdd,0xd9,
    0x72,0x2c,0x8b,0x82,0xd0,0x02,0x00,0x00,0x80,0xb8,0x2b,0x03,0x00,0x00,
    0x00,0x75,0x09,0xd9,0x82,0x7c,0x07,0x00,0x00,0xeb,0xbc,0x90
};
static const unsigned char shazbot_energy_mount_loop_bytes[] = {
    0x8b,0x82,0xe4,0x07,0x00,0x00,0x85,0xc0,0x74,0x06,0x89,0xc2,0xeb,0xda,
    0x89,0xf6,0xd9,0xee,0xeb,0xa7,0xdd,0xd8,0xd9,0xee,0x89,0xec,0x5d,0xc3
};
static const unsigned char shazbot_jump_normal_x_init_bytes[] = {
    0xc7,0x82,0x90,0x09,0x00,0x00,0x00,0x00,0x00,0x00
};
static const unsigned char shazbot_jump_normal_y_init_bytes[] = {
    0xc7,0x82,0x94,0x09,0x00,0x00,0x00,0x00,0x00,0x00
};
static const unsigned char shazbot_jump_normal_z_init_bytes[] = {
    0xc7,0x82,0x98,0x09,0x00,0x00,0x00,0x00,0x80,0x3f
};
static const unsigned char shazbot_contact_info_init_bytes[] = {
    0xc7,0x86,0x40,0x0a,0x00,0x00,0x00,0x00,0x80,0x3f,
    0xc6,0x86,0x3e,0x0a,0x00,0x00,0x00,
    0xc6,0x86,0x3d,0x0a,0x00,0x00,0x00,
    0xc6,0x86,0x3c,0x0a,0x00,0x00,0x00,
    0xc7,0x40,0x04,0x00,0x00,0x80,0x3f,
    0xc7,0x40,0x08,0x00,0x00,0x80,0x3f
};
static const unsigned char shazbot_jump_normal_store_bytes[] = {
    0x80,0xbd,0x37,0xff,0xff,0xff,0x00,0x74,0x1b,
    0x8b,0x45,0x88,0x89,0x83,0x90,0x09,0x00,0x00,
    0x8b,0x45,0x8c,0x89,0x83,0x94,0x09,0x00,0x00,
    0x8b,0x45,0x90,0x89,0x83,0x98,0x09,0x00,0x00
};
static const unsigned char shazbot_find_contact_gate_bytes[] = {
    0x83,0xbb,0xe4,0x07,0x00,0x00,0x00,0x75,0x1b,0x8d,0x45,0x88,0x50,
    0x8d,0x85,0x37,0xff,0xff,0xff,0x50,0x8d,0x85,0x36,0xff,0xff,0xff,
    0x50,0x53,0xe8,0x97,0x52,0x00,0x00,0x83,0xc4,0x10
};
static const unsigned char shazbot_contact_info_contact_flag_bytes[] = {
    0x88,0x87,0x3c,0x0a,0x00,0x00
};
static const unsigned char shazbot_contact_info_jump_flag_bytes[] = {
    0x8b,0x55,0x10,0x8a,0x02,0x88,0x87,0x3d,0x0a,0x00,0x00
};
static const unsigned char shazbot_visibility_membership_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x10,0x56,0x53,0x8b,0x5d,0x08,0x8b,0x75,0x0c,
    0xf6,0x43,0x40,0x02,0x74,0x29,0x85,0xf6,0x74,0x11,0xa1,0xec,0x32,0x4e,
    0x08,0x83,0xc4,0xf8,0x8b,0x10,0x53,0x50,0x8b,0x42,0x4c,0xeb,0x0f,0xa1,
    0xec,0x32,0x4e,0x08
};
static const unsigned char shazbot_client_target_index_bytes[] = {
    0x55,0x89,0xe5,0x8b,0x55,0x08,0x8b,0x45,0x0c,0x3b,0x82,0x10,0x02,0x00,
    0x00,0x73,0x0f,0xc1,0xe0,0x04,0x8d,0x44,0x02,0x10
};
static const unsigned char shazbot_client_target_ctor_bytes[] = {
    0x55,0x89,0xe5,0x83,0xec,0x0c,0x57,0x8b,0x7d,0x08,0x56,0x53,0x8b,0x75,
    0x0c,0x8b,0x5d,0x14,0x83,0xc4,0xf4,0x57,0xe8
};
static const unsigned char shazbot_assigned_task_type_bytes[] = {
    0x80,0x7b,0x20,0x00,0x0f,0x94,0xc0,0x89,0xc2,0x83,0xe2,0x01,0x89,0x57,0x30
};
static const unsigned char shazbot_waypoint_type_bytes[] = {
    0xc7,0x43,0x30,0x02,0x00,0x00,0x00
};
static const unsigned char shazbot_client_target_list_init_bytes[] = {
    0x68,0xa0,0x02,0x00,0x00,0xe8,0xec,0x18,0xd7,0xff,0x50,0xe8,0xee,0x12,
    0x00,0x00,0xa3,0xa4,0x7d,0x49,0x08
};
static const unsigned char shazbot_client_target_live_object_bytes[] = {
    0xf6,0x40,0x3c,0x01,0x74,0x49,0x8b,0x50,0x24,0x85,0xd2,0x74,0x42
};
static const unsigned char shazbot_sensor_group_read_bytes[] = {
    0x8b,0x81,0x34,0x84,0x00,0x00,0x89,0x86,0xd8,0x00,0x00,0x00
};
static const unsigned char shazbot_aim_billboard_call_original[] = {
    0xe8,0x0c,0x2f,0xe1,0xff
};
static int shazbot_spatial_anchors_valid(const struct rv_memory *memory) {
    return rv_memory_range(memory, (void *)SHAZBOT_EYE_CALLBACK,
                           sizeof shazbot_eye_callback_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)SHAZBOT_EYE_CALLBACK, shazbot_eye_callback_bytes,
                  sizeof shazbot_eye_callback_bytes) == 0 &&
           rv_memory_range(memory, (void *)SHAZBOT_FOV_CALLBACK,
                           sizeof shazbot_fov_callback_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)SHAZBOT_FOV_CALLBACK, shazbot_fov_callback_bytes,
                  sizeof shazbot_fov_callback_bytes) == 0 &&
           rv_memory_range(memory, (void *)SHAZBOT_MUZZLE_VECTOR_CALLBACK,
                           sizeof shazbot_muzzle_vector_callback_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)SHAZBOT_MUZZLE_VECTOR_CALLBACK, shazbot_muzzle_vector_callback_bytes,
                  sizeof shazbot_muzzle_vector_callback_bytes) == 0 &&
           rv_memory_range(memory, (void *)SHAZBOT_MUZZLE_POINT_CALLBACK,
                           sizeof shazbot_muzzle_point_callback_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)SHAZBOT_MUZZLE_POINT_CALLBACK, shazbot_muzzle_point_callback_bytes,
                  sizeof shazbot_muzzle_point_callback_bytes) == 0 &&
           rv_memory_range(memory, (void *)0x0826C9A4,
                           sizeof shazbot_muzzle_vector_slot_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)0x0826C9A4, shazbot_muzzle_vector_slot_bytes,
                  sizeof shazbot_muzzle_vector_slot_bytes) == 0 &&
           rv_memory_range(memory, (void *)0x08273758,
                           sizeof shazbot_muzzle_point_helper_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)0x08273758, shazbot_muzzle_point_helper_bytes,
                  sizeof shazbot_muzzle_point_helper_bytes) == 0 &&
           rv_memory_range(memory, (void *)SHAZBOT_MOUNTED_IMAGE_GETTER,
                           sizeof shazbot_mounted_image_getter_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)SHAZBOT_MOUNTED_IMAGE_GETTER, shazbot_mounted_image_getter_bytes,
                  sizeof shazbot_mounted_image_getter_bytes) == 0 &&
           rv_memory_range(memory, (void *)SHAZBOT_TARGET_SENSOR_CALLBACK + 0x1f,
                           sizeof shazbot_target_sensor_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)(SHAZBOT_TARGET_SENSOR_CALLBACK + 0x1f),
                  shazbot_target_sensor_bytes, sizeof shazbot_target_sensor_bytes) == 0 &&
           rv_memory_range(memory, (void *)SHAZBOT_SENSOR_RADIUS_REGISTRATION,
                           sizeof shazbot_sensor_radius_field_bytes, PROT_READ | PROT_EXEC) &&
           memcmp((void *)SHAZBOT_SENSOR_RADIUS_REGISTRATION,
                   shazbot_sensor_radius_field_bytes,
                   sizeof shazbot_sensor_radius_field_bytes) == 0;
}

/* Anchors for the separate 32-float Observation producer, including the
   current ContactInfo/jump-surface guard for the non-terrain-specific normal. */
static const char *shazbot_observation_anchor_failure(const struct rv_memory *memory) {
#define SHAZBOT_CHECK_ANCHOR(address, bytes, label) do { \
    if (!rv_memory_range(memory, (void *)(address), sizeof bytes, PROT_READ | PROT_EXEC)) \
        return "unreadable anchor: " label; \
    if (memcmp((void *)(address), bytes, sizeof bytes) != 0) \
        return "mismatched anchor: " label; \
} while (0)
    SHAZBOT_CHECK_ANCHOR(SHAZBOT_ENERGY_PERCENT_CALLBACK, shazbot_energy_percent_callback_bytes,
                         "energy-percent callback 0x0826cea8");
    SHAZBOT_CHECK_ANCHOR(0x0826D000, shazbot_damage_value_method_bytes,
                         "damage fraction method 0x0826d000");
    SHAZBOT_CHECK_ANCHOR(0x082465B6, shazbot_max_forward_speed_field_bytes,
                         "maxForwardSpeed field 0x082465b6");
    SHAZBOT_CHECK_ANCHOR(0x0826CEE0, shazbot_energy_mount_path_bytes,
                         "energy mount path 0x0826cee0");
    SHAZBOT_CHECK_ANCHOR(0x0826CF20, shazbot_energy_mount_loop_bytes,
                         "energy mount loop 0x0826cf20");
    SHAZBOT_CHECK_ANCHOR(0x0826CB50, shazbot_velocity_getter_callback_bytes,
                         "velocity getter 0x0826cb50");
    SHAZBOT_CHECK_ANCHOR(0x08248D98, shazbot_velocity_x_init_bytes, "velocity X init 0x08248d98");
    SHAZBOT_CHECK_ANCHOR(0x08248DA2, shazbot_velocity_y_init_bytes, "velocity Y init 0x08248da2");
    SHAZBOT_CHECK_ANCHOR(0x08248DAC, shazbot_velocity_z_init_bytes, "velocity Z init 0x08248dac");
    SHAZBOT_CHECK_ANCHOR(0x08248DB6, shazbot_jump_normal_x_init_bytes, "jump normal X init 0x08248db6");
    SHAZBOT_CHECK_ANCHOR(0x08248DC0, shazbot_jump_normal_y_init_bytes, "jump normal Y init 0x08248dc0");
    SHAZBOT_CHECK_ANCHOR(0x08248DCA, shazbot_jump_normal_z_init_bytes, "jump normal Z init 0x08248dca");
    SHAZBOT_CHECK_ANCHOR(0x08248A23, shazbot_contact_info_init_bytes, "ContactInfo init 0x08248a23");
    SHAZBOT_CHECK_ANCHOR(0x0824B27C, shazbot_jump_normal_store_bytes, "jump normal store 0x0824b27c");
    SHAZBOT_CHECK_ANCHOR(0x0824B258, shazbot_find_contact_gate_bytes, "contact gate 0x0824b258");
    SHAZBOT_CHECK_ANCHOR(0x08250DC6, shazbot_contact_info_contact_flag_bytes, "contact flag 0x08250dc6");
    SHAZBOT_CHECK_ANCHOR(0x08250DF6, shazbot_contact_info_jump_flag_bytes, "jump flag 0x08250df6");
    SHAZBOT_CHECK_ANCHOR(0x0825C8F8, shazbot_visibility_membership_bytes, "visibility membership 0x0825c8f8");
    SHAZBOT_CHECK_ANCHOR(0x082E2850, shazbot_client_target_index_bytes, "client target index 0x082e2850");
    SHAZBOT_CHECK_ANCHOR(0x082E2878, shazbot_client_target_ctor_bytes, "client target constructor 0x082e2878");
    SHAZBOT_CHECK_ANCHOR(0x0827B066, shazbot_assigned_task_type_bytes, "assigned task type 0x0827b066");
    SHAZBOT_CHECK_ANCHOR(0x082E2A94, shazbot_waypoint_type_bytes, "waypoint type 0x082e2a94");
    SHAZBOT_CHECK_ANCHOR(0x082DC13A, shazbot_client_target_list_init_bytes,
                         "client target list init 0x082dc13a");
    SHAZBOT_CHECK_ANCHOR(0x082DD8FE, shazbot_client_target_live_object_bytes,
                         "client target live object 0x082dd8fe");
    SHAZBOT_CHECK_ANCHOR(0x0819401A, shazbot_sensor_group_read_bytes,
                         "sensor group read 0x0819401a");
#undef SHAZBOT_CHECK_ANCHOR
    return NULL;
}

/* Read eye origin/forward via the researched Player virtual and camera FOV via
   ShapeBase::getCameraFov's virtual slot. SensorData comes from this target's
   TargetManager server-target record; no guessed fallback radius is emitted. */
static int shazbot_local_view_cone(const struct rv_memory *memory, void *player,
                                   uint32_t object_id, void *target_manager,
                                   float origin[3], float forward[3], float *fov_degrees,
                                   float *detection_radius) {
    if (!memory || !player || !object_id || !target_manager ||
        !origin || !forward || !fov_degrees || !detection_radius ||
        !rv_memory_range(memory, player, SHAZBOT_TARGET_INFO_OFFSET + 4, PROT_READ)) return 0;
    const char *player_class = rv_class_name(memory, player);
    if (!player_class || strcmp(player_class, "Player") != 0 || rv_word(player, 0x20) != object_id)
        return 0;

    uint32_t target_id = rv_word(player, SHAZBOT_TARGET_ID_OFFSET);
    if (target_id >= 512) return 0;
    unsigned char *target_info = (unsigned char *)target_manager + SHAZBOT_TARGET_ARRAY_OFFSET +
                                 (size_t)target_id * SHAZBOT_TARGET_STRIDE;
    size_t manager_bytes = SHAZBOT_TARGET_ARRAY_OFFSET +
                           ((size_t)target_id + 1) * SHAZBOT_TARGET_STRIDE;
    if (!rv_memory_range(memory, target_manager, manager_bytes, PROT_READ) ||
        rv_pointer(player, SHAZBOT_TARGET_INFO_OFFSET) != target_info) return 0;
    void *sensor_data = rv_pointer(target_info, SHAZBOT_TARGET_SENSOR_DATA_OFFSET);
    if (!rv_memory_range(memory, sensor_data, SHAZBOT_SENSOR_DETECT_RADIUS_OFFSET + 4, PROT_READ)) return 0;
    const char *sensor_class = rv_class_name(memory, sensor_data);
    if (!sensor_class || strcmp(sensor_class, "SensorData") != 0) return 0;
    float radius = rv_float(sensor_data, SHAZBOT_SENSOR_DETECT_RADIUS_OFFSET);
    if (!isfinite(radius) || radius <= 0.0f) return 0;

    void *vtable = rv_pointer(player, 0);
    if (!rv_memory_range(memory, vtable, SHAZBOT_CAMERA_FOV_VTABLE_OFFSET + 4, PROT_READ)) return 0;
    void *eye_method = rv_pointer(vtable, SHAZBOT_EYE_TRANSFORM_VTABLE_OFFSET);
    void *fov_method = rv_pointer(vtable, SHAZBOT_CAMERA_FOV_VTABLE_OFFSET);
    if (!rv_memory_range(memory, eye_method, 1, PROT_EXEC) ||
        !rv_memory_range(memory, fov_method, 1, PROT_EXEC)) return 0;

    float matrix[16];
    memset(matrix, 0, sizeof matrix);
    ((void (*)(void *, float *))eye_method)(player, matrix);
    float fov = ((float (*)(void *))fov_method)(player);
    origin[0] = matrix[3]; origin[1] = matrix[7]; origin[2] = matrix[11];
    /* Torque eye transforms use local +Y as forward: MatrixF::mulV(0,1,0). */
    forward[0] = matrix[1]; forward[1] = matrix[5]; forward[2] = matrix[9];
    float norm2 = forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2];
    if (!isfinite(fov) || fov <= 0.0f || fov >= 180.0f || !isfinite(norm2) || norm2 < 1.0e-8f)
        return 0;
    /* MatrixF's eye basis is orthonormal; the viewer normalizes defensively. */
    if (!isfinite(origin[0]) || !isfinite(origin[1]) || !isfinite(origin[2])) return 0;
    *fov_degrees = fov;
    *detection_radius = radius;
    return 1;
}

static int shazbot_player_velocity(const struct rv_memory *memory, void *player,
                                   float velocity[3]) {
    if (!memory || !player || !velocity ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_PLAYER_VELOCITY_X_OFFSET,
                         SHAZBOT_PLAYER_VELOCITY_Z_OFFSET - SHAZBOT_PLAYER_VELOCITY_X_OFFSET + 4,
                         PROT_READ)) return 0;
    velocity[0] = rv_float(player, SHAZBOT_PLAYER_VELOCITY_X_OFFSET);
    velocity[1] = rv_float(player, SHAZBOT_PLAYER_VELOCITY_Y_OFFSET);
    velocity[2] = rv_float(player, SHAZBOT_PLAYER_VELOCITY_Z_OFFSET);
    return isfinite(velocity[0]) && isfinite(velocity[1]) && isfinite(velocity[2]);
}

static int shazbot_shape_velocity(const struct rv_memory *memory, void *target,
                                  const char *class_name, float velocity[3]) {
    if (!memory || !target || !class_name || !velocity) return 0;
    if (strcmp(class_name, "Player") == 0)
        return shazbot_player_velocity(memory, target, velocity);

    /* These concrete ShapeBase classes have source-verified getVelocity
       implementations: Item owns mVelocity; Vehicle and its three registered
       subclasses inherit Vehicle::getVelocity. Other ShapeBases inherit the
       zero-valued GameBase implementation and are not treated as moving. */
    if (strcmp(class_name, "Item") != 0 && strcmp(class_name, "Vehicle") != 0 &&
        strcmp(class_name, "WheeledVehicle") != 0 &&
        strcmp(class_name, "FlyingVehicle") != 0 &&
        strcmp(class_name, "HoverVehicle") != 0) return 0;

#ifndef T2_HOOK_TEST
    if (!rv_memory_range(memory, (void *)0x0826CB50,
                         sizeof shazbot_velocity_getter_callback_bytes, PROT_READ | PROT_EXEC) ||
        memcmp((void *)0x0826CB50, shazbot_velocity_getter_callback_bytes,
               sizeof shazbot_velocity_getter_callback_bytes) != 0) return 0;
#endif
    void *vtable = rv_pointer(target, 0);
    if (!rv_memory_range(memory, vtable, 0xF0, PROT_READ)) return 0;
    void *getter = rv_pointer(vtable, 0xEC);
    if (!rv_memory_range(memory, getter, 1, PROT_EXEC)) return 0;
    /* The anchored ShapeBase console callback passes (&VectorF, this) to the
       +0xec slot; mirror that verified aggregate-return ABI directly. */
    ((void (*)(float *, void *))getter)(velocity, target);
    return isfinite(velocity[0]) && isfinite(velocity[1]) && isfinite(velocity[2]);
}

/* Match #25034 getEnergyPercent: local energy when inheritance is off; otherwise
   follow the mounted ShapeBase chain to its first non-inheriting energy source. */
static int shazbot_player_energy_fraction(const struct rv_memory *memory, void *dictionary,
                                           void *player, float *fraction) {
    if (!memory || !dictionary || !player || !fraction) return 0;
    void *current = player;
    void *visited[32];
    size_t visited_count = 0;
    float denominator = 0.0f;
    int inherited = 0;

    for (size_t depth = 0; depth < sizeof visited / sizeof visited[0]; depth++) {
        if (!rv_memory_range(memory, current, 0x7e8, PROT_READ)) return 0;
        for (size_t i = 0; i < visited_count; i++)
            if (visited[i] == current) return 0;
        visited[visited_count++] = current;
        uint32_t object_id = rv_word(current, 0x20);
        if (!object_id || object_id == UINT32_MAX ||
            rv_find_object(memory, dictionary, object_id) != current) return 0;

        void *data = rv_pointer(current, SHAZBOT_PLAYER_DATABLOCK_OFFSET);
        if (!rv_memory_range(memory, data, SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET + 1,
                             PROT_READ)) return 0;
        const char *data_class = rv_class_name(memory, data);
        if (!data_class || (!inherited && strcmp(data_class, "PlayerData") != 0)) return 0;
        const float maximum = rv_float(data, SHAZBOT_DATABLOCK_MAX_ENERGY_OFFSET);
        if (inherited && denominator == 0.0f) {
            if (!isfinite(maximum) || maximum <= 0.0f) return 0;
            denominator = maximum;
        }
        const int data_inherits =
            *((const unsigned char *)data + SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET) != 0;
        if (!inherited && !data_inherits) {
            denominator = maximum;
        } else if (inherited && data_inherits) {
            current = rv_pointer(current, 0x7e4);
            if (!current) return 0;
            continue;
        } else if (!inherited && data_inherits) {
            current = rv_pointer(current, 0x7e4);
            if (!current) return 0;
            inherited = 1;
            continue;
        }

        float energy = rv_float(current, SHAZBOT_PLAYER_ENERGY_OFFSET);
        if (!isfinite(denominator) || denominator <= 0.0f || !isfinite(energy) ||
            energy < 0.0f || energy > denominator) return 0;
        *fraction = energy / denominator;
        return isfinite(*fraction) && *fraction >= 0.0f && *fraction <= 1.0f;
    }
    return 0; /* Refuse a malformed or cyclic mount chain. */
}

static int shazbot_player_health_fraction(const struct rv_memory *memory, void *dictionary,
                                          void *player, float *fraction) {
    if (!memory || !dictionary || !player || !fraction ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_PLAYER_DAMAGE_OFFSET,
                         sizeof(float), PROT_READ) ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_PLAYER_DATABLOCK_OFFSET,
                         sizeof(uint32_t), PROT_READ)) return 0;
    uint32_t object_id = rv_word(player, 0x20);
    if (!object_id || rv_find_object(memory, dictionary, object_id) != player) return 0;
    void *data = rv_pointer(player, SHAZBOT_PLAYER_DATABLOCK_OFFSET);
    if (!rv_memory_range(memory, (unsigned char *)data + SHAZBOT_DATABLOCK_MAX_DAMAGE_OFFSET,
                         sizeof(float), PROT_READ)) return 0;
    const char *data_class = rv_class_name(memory, data);
    if (!data_class || strcmp(data_class, "PlayerData") != 0) return 0;
    const float damage = rv_float(player, SHAZBOT_PLAYER_DAMAGE_OFFSET);
    const float maximum = rv_float(data, SHAZBOT_DATABLOCK_MAX_DAMAGE_OFFSET);
    if (!isfinite(damage) || !isfinite(maximum) || maximum <= 0.0f ||
        damage < 0.0f || damage > maximum) return 0;
    *fraction = 1.0f - damage / maximum;
    return isfinite(*fraction) && *fraction >= 0.0f && *fraction <= 1.0f;
}

static int shazbot_player_speed_reference(const struct rv_memory *memory, void *player,
                                          float *speed_reference) {
    if (!memory || !player || !speed_reference ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_PLAYER_DATABLOCK_OFFSET,
                         sizeof(uint32_t), PROT_READ)) return 0;
    void *data = rv_pointer(player, SHAZBOT_PLAYER_DATABLOCK_OFFSET);
    if (!rv_memory_range(memory, data, SHAZBOT_PLAYER_MAX_FORWARD_SPEED_OFFSET + sizeof(float),
                         PROT_READ)) return 0;
    const char *data_class = rv_class_name(memory, data);
    if (!data_class || strcmp(data_class, "PlayerData") != 0) return 0;
    const float value = rv_float(data, SHAZBOT_PLAYER_MAX_FORWARD_SPEED_OFFSET);
    if (!isfinite(value) || value <= 0.0f) return 0;
    *speed_reference = value;
    return 1;
}

static int shazbot_player_target_info(const struct rv_memory *memory, void *player,
                                      void *dictionary, void *target_manager,
                                      uint32_t self_id, int local_sensor_group,
                                      float position[3], float velocity[3], uint32_t *id_out) {
    if (!memory || !player || !dictionary || !target_manager || !position || !velocity ||
        !rv_memory_range(memory, player, SHAZBOT_TARGET_ID_OFFSET + sizeof(uint32_t), PROT_READ) ||
        !rv_memory_range(memory, target_manager, SHAZBOT_TARGET_ARRAY_OFFSET, PROT_READ)) return 0;
    const char *class_name = rv_class_name(memory, player);
    if (!class_name || strcmp(class_name, "Player") != 0 ||
        !(rv_word(player, 0x40) & 2)) return 0;
    uint32_t id = rv_word(player, 0x20);
    uint32_t target_id = rv_word(player, SHAZBOT_TARGET_ID_OFFSET);
    if (!id || id == self_id || target_id >= SHAZBOT_TARGET_INFO_COUNT ||
        rv_find_object(memory, dictionary, id) != player) return 0;

    unsigned char *server_info = (unsigned char *)target_manager + SHAZBOT_TARGET_ARRAY_OFFSET +
                                 (size_t)target_id * SHAZBOT_TARGET_INFO_STRIDE;
    unsigned char *client_info = (unsigned char *)target_manager + SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET +
                                 (size_t)target_id * SHAZBOT_TARGET_INFO_STRIDE;
    void *object_info = rv_pointer(player, SHAZBOT_TARGET_INFO_OFFSET);
    size_t manager_bytes = SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET +
                           ((size_t)target_id + 1) * SHAZBOT_TARGET_INFO_STRIDE;
    if (!rv_memory_range(memory, target_manager, manager_bytes, PROT_READ) ||
        (object_info != server_info && object_info != client_info) ||
        !rv_memory_range(memory, object_info, SHAZBOT_TARGET_INFO_STRIDE, PROT_READ)) return 0;
    int target_group = (int32_t)rv_word(object_info, 0x18);
    if (target_group <= 0 || local_sensor_group <= 0 || target_group == local_sensor_group ||
        !rv_position(memory, player, RV_PLAYER, position) ||
        !shazbot_player_velocity(memory, player, velocity)) return 0;
    if (id_out) *id_out = id;
    return 1;
}

static int shazbot_nearest_visible_enemy(const struct rv_memory *memory, void *visible_set,
                                         void *dictionary, void *target_manager,
                                         uint32_t self_id, int local_sensor_group,
                                         const float self_position[3], const float self_velocity[3],
                                         float position_delta[3],
                                         float velocity_delta[3]) {
    if (!memory || !visible_set || !dictionary || !target_manager || !self_id ||
        !self_position || !self_velocity || !position_delta || !velocity_delta ||
        !rv_memory_range(memory, visible_set, 0x3c, PROT_READ)) return 0;
    uint32_t count = rv_word(visible_set, 0x30);
    void *objects = rv_pointer(visible_set, 0x38);
    if (count > 4096 || (count && !rv_memory_range(memory, objects, (size_t)count * 4, PROT_READ)))
        return 0;

    int found = 0;
    double best_distance2 = 0.0;
    uint32_t best_id = 0;
    float best_position[3], best_velocity[3];
    for (uint32_t i = 0; i < count; i++) {
        void *candidate = rv_pointer(objects, (size_t)i * 4);
        float position[3], velocity[3];
        uint32_t id;
        if (!shazbot_player_target_info(memory, candidate, dictionary, target_manager,
                                        self_id, local_sensor_group, position, velocity, &id))
            continue;
        double dx = (double)position[0] - self_position[0];
        double dy = (double)position[1] - self_position[1];
        double dz = (double)position[2] - self_position[2];
        double distance2 = dx * dx + dy * dy + dz * dz;
        if (!isfinite(distance2)) continue;
        if (!found || distance2 < best_distance2 ||
            (distance2 == best_distance2 && id < best_id)) {
            found = 1;
            best_distance2 = distance2;
            best_id = id;
            memcpy(best_position, position, sizeof best_position);
            memcpy(best_velocity, velocity, sizeof best_velocity);
        }
    }
    if (!found) return 0;
    for (int axis = 0; axis < 3; axis++) {
        position_delta[axis] = best_position[axis] - self_position[axis];
        velocity_delta[axis] = best_velocity[axis] - self_velocity[axis];
        if (!isfinite(position_delta[axis]) || !isfinite(velocity_delta[axis])) return 0;
    }
    return 1;
}

static int shazbot_assigned_task_delta(const struct rv_memory *memory, void *client_target_list,
                                       void *dictionary, void *target_manager,
                                       const float self_position[3], const float self_velocity[3],
                                       float delta[3], float velocity_delta[3],
                                       int *velocity_valid) {
    if (velocity_valid) *velocity_valid = 0;
    if (!memory || !client_target_list || !self_position || !self_velocity || !delta ||
        !velocity_delta ||
        !rv_memory_range(memory, client_target_list,
                         SHAZBOT_CLIENT_TARGET_LIST_COUNT_OFFSET + sizeof(uint32_t), PROT_READ))
        return 0;
    uint32_t count = rv_word(client_target_list, SHAZBOT_CLIENT_TARGET_LIST_COUNT_OFFSET);
    if (count > SHAZBOT_CLIENT_TARGET_LIST_CAPACITY ||
        !rv_memory_range(memory, client_target_list,
                         SHAZBOT_CLIENT_TARGET_LIST_ENTRIES_OFFSET +
                         (size_t)count * SHAZBOT_CLIENT_TARGET_ENTRY_STRIDE, PROT_READ)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry_offset = SHAZBOT_CLIENT_TARGET_LIST_ENTRIES_OFFSET +
                              (size_t)i * SHAZBOT_CLIENT_TARGET_ENTRY_STRIDE;
        void *task = rv_pointer(client_target_list, entry_offset);
        if (!rv_memory_range(memory, task, 0x48, PROT_READ)) continue;
        const char *class_name = rv_class_name(memory, task);
        if (!class_name || strcmp(class_name, "ClientTarget") != 0 ||
            rv_word(task, SHAZBOT_CLIENT_TARGET_TYPE_OFFSET) != 0) continue;
        float task_position[3] = {0.0f, 0.0f, 0.0f};
        int position_valid = 1;
        for (int axis = 0; axis < 3; axis++) {
            task_position[axis] = rv_float(task, SHAZBOT_CLIENT_TARGET_POSITION_OFFSET +
                                           (size_t)axis * sizeof(float));
            if (!isfinite(task_position[axis])) position_valid = 0;
        }
        if (!position_valid) continue;
        int delta_valid = 1;
        for (int axis = 0; axis < 3; axis++) {
            delta[axis] = task_position[axis] - self_position[axis];
            if (!isfinite(delta[axis])) delta_valid = 0;
        }
        if (!delta_valid) continue;

        /* A task may be a position-only assignment (target ID -1/stale) or
           point to a live ghost. Use velocity only for the latter when the
           client TargetInfo still owns that exact object and its concrete
           ShapeBase class has a verified nonzero velocity implementation. */
        uint32_t target_id = rv_word(task, 0x34);
        if (dictionary && target_manager && target_id < SHAZBOT_TARGET_INFO_COUNT) {
            unsigned char *client_info = (unsigned char *)target_manager +
                SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET +
                (size_t)target_id * SHAZBOT_TARGET_INFO_STRIDE;
            unsigned char *server_info = (unsigned char *)target_manager +
                SHAZBOT_TARGET_ARRAY_OFFSET +
                (size_t)target_id * SHAZBOT_TARGET_INFO_STRIDE;
            size_t client_manager_bytes = SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET +
                ((size_t)target_id + 1) * SHAZBOT_TARGET_INFO_STRIDE;
            size_t server_manager_bytes = SHAZBOT_TARGET_ARRAY_OFFSET +
                ((size_t)target_id + 1) * SHAZBOT_TARGET_INFO_STRIDE;
            if (rv_memory_range(memory, target_manager, client_manager_bytes, PROT_READ) &&
                rv_memory_range(memory, target_manager, server_manager_bytes, PROT_READ) &&
                (rv_word(client_info, 0x3c) & 1)) {
                void *target = rv_pointer(client_info, 0x24);
                if (rv_memory_range(memory, target, 0x274, PROT_READ)) {
                    float target_velocity[3];
                    uint32_t target_object_id = rv_word(target, 0x20);
                    if (target_object_id && (rv_word(target, 0x40) & 2) &&
                        rv_word(target, 0x270) == target_id &&
                        rv_find_object(memory, dictionary, target_object_id) == target &&
                        rv_pointer(target, SHAZBOT_TARGET_INFO_OFFSET) == server_info) {
                        const char *target_class = rv_class_name(memory, target);
                        if (target_class &&
                            shazbot_shape_velocity(memory, target, target_class, target_velocity)) {
                            int finite_delta = 1;
                            for (int axis = 0; axis < 3; axis++) {
                                velocity_delta[axis] = target_velocity[axis] - self_velocity[axis];
                                if (!isfinite(velocity_delta[axis])) finite_delta = 0;
                            }
                            if (finite_delta && velocity_valid) *velocity_valid = 1;
                        }
                    }
                }
            }
        }
        return 1; /* type 0 is AssignedTask; type 1/2 tasks are not substituted. */
    }
    return 0;
}

static void shazbot_observation_put_vector(float values[SHAZBOT_OBSERVATION_DIMENSIONS],
                                           size_t offset, const float vector[3]) {
    for (int axis = 0; axis < 3; axis++) values[offset + (size_t)axis] = vector[axis];
}

static float shazbot_sqrtf(float value) {
    float result;
    __asm__ __volatile__("flds %1; fsqrt; fstps %0" : "=m"(result) : "m"(value));
    return result;
}

/* +0x990 is mJumpSurfaceNormal, which persists between contacts. The paired
   +0xa3c/+0xa3d ContactInfo flags are updated by findContact; require both a
   current contact and a current jump-surface classification before exposing it. */
static int shazbot_player_contact_normal(const struct rv_memory *memory, void *player,
                                         float normal[3]) {
    if (!memory || !player || !normal ||
        !rv_memory_range(memory, player, 0xA4C, PROT_READ)) return 0;
    const unsigned char *bytes = player;
    if (rv_pointer(player, 0x7E4) || !bytes[0xA3C] || !bytes[0xA3D]) return 0;
    float candidate[3] = {rv_float(player, 0x990), rv_float(player, 0x994),
                          rv_float(player, 0x998)};
    if (!isfinite(candidate[0]) || !isfinite(candidate[1]) || !isfinite(candidate[2])) return 0;
    float length2 = candidate[0] * candidate[0] + candidate[1] * candidate[1] +
                    candidate[2] * candidate[2];
    if (!isfinite(length2) || length2 < 1.0e-8f) return 0;
    float inverse_length = 1.0f / shazbot_sqrtf(length2);
    for (int axis = 0; axis < 3; axis++) normal[axis] = candidate[axis] * inverse_length;
    return 1;
}

/* View direction from the Player's own eye transform. Unlike the view cone, this
   needs no SensorData record: Move X/Y and pitch/yaw are view-relative, so the
   policy needs the look direction even when the sensor radius is unavailable. */
static int shazbot_player_eye_forward(const struct rv_memory *memory, void *player,
                                      float forward[3]) {
    if (!memory || !player || !forward || !rv_memory_range(memory, player, 0x24, PROT_READ))
        return 0;
    void *vtable = rv_pointer(player, 0);
    if (!rv_memory_range(memory, vtable, SHAZBOT_EYE_TRANSFORM_VTABLE_OFFSET + 4, PROT_READ))
        return 0;
    void *eye_method = rv_pointer(vtable, SHAZBOT_EYE_TRANSFORM_VTABLE_OFFSET);
    if (!rv_memory_range(memory, eye_method, 1, PROT_EXEC)) return 0;
    float matrix[16];
    memset(matrix, 0, sizeof matrix);
    ((void (*)(void *, float *))eye_method)(player, matrix);
    /* Torque eye transforms use local +Y as forward: MatrixF::mulV(0,1,0). */
    forward[0] = matrix[1]; forward[1] = matrix[5]; forward[2] = matrix[9];
    float norm2 = forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2];
    return isfinite(norm2) && norm2 > 1.0e-8f;
}

static int shazbot_collect_observation(const struct rv_memory *memory, void *connection,
                                       void *dictionary, void *target_manager,
                                       void *visible_set, void *client_target_list,
                                       int view_valid, const float view_forward[3],
                                       float fov_degrees, float sensor_radius,
                                       float values[SHAZBOT_OBSERVATION_DIMENSIONS]) {
    if (!memory || !values) return 0;
    memset(values, 0, SHAZBOT_OBSERVATION_DIMENSIONS * sizeof values[0]);
    float player_position[3], player_velocity[3];
    uint32_t player_id;
    void *player = NULL;
    if (!shazbot_local_player_position(memory, connection, dictionary, player_position,
                                       &player_id, &player) ||
        !shazbot_player_velocity(memory, player, player_velocity)) return 0;
    shazbot_observation_put_vector(values, 0, player_position);
    shazbot_observation_put_vector(values, 3, player_velocity);
    float energy_fraction = 0.0f;
    if (shazbot_player_energy_fraction(memory, dictionary, player, &energy_fraction)) {
        values[30] = energy_fraction;
        values[31] = 1.0f;
    }
    float speed_reference = 0.0f;
    if (shazbot_player_speed_reference(memory, player, &speed_reference))
        values[29] = speed_reference;

    float contact_normal[3];
    if (shazbot_player_contact_normal(memory, player, contact_normal)) {
        shazbot_observation_put_vector(values, 6, contact_normal);
        values[9] = 1.0f;
    }
    int local_sensor_group = 0;
    if (rv_memory_range(memory, (unsigned char *)connection + SHAZBOT_LOCAL_SENSOR_GROUP_OFFSET,
                        sizeof(uint32_t), PROT_READ))
        local_sensor_group = (int32_t)rv_word(connection, SHAZBOT_LOCAL_SENSOR_GROUP_OFFSET);
    float opponent_position_delta[3], opponent_velocity_delta[3];
    if (shazbot_nearest_visible_enemy(memory, visible_set, dictionary, target_manager,
                                      player_id, local_sensor_group, player_position,
                                      player_velocity,
                                      opponent_position_delta, opponent_velocity_delta)) {
        shazbot_observation_put_vector(values, 10, opponent_position_delta);
        shazbot_observation_put_vector(values, 13, opponent_velocity_delta);
        values[16] = 1.0f;
        values[17] = 1.0f;
    }
    float objective_delta[3], objective_velocity_delta[3];
    int objective_velocity_valid = 0;
    if (shazbot_assigned_task_delta(memory, client_target_list, dictionary, target_manager,
                                    player_position, player_velocity, objective_delta,
                                    objective_velocity_delta, &objective_velocity_valid)) {
        shazbot_observation_put_vector(values, 18, objective_delta);
        values[24] = 1.0f;
        if (objective_velocity_valid) {
            shazbot_observation_put_vector(values, 21, objective_velocity_delta);
            values[25] = 1.0f;
        }
    }
    float eye_forward[3];
    if (!(view_valid && view_forward && isfinite(fov_degrees) && fov_degrees > 0.0f &&
          fov_degrees < 180.0f && isfinite(sensor_radius) && sensor_radius > 0.0f) &&
        shazbot_player_eye_forward(memory, player, eye_forward)) {
        view_valid = 1;
        view_forward = eye_forward;
        fov_degrees = 90.0f;     /* only gates this block; FOV is not an observation slot */
        sensor_radius = 1.0f;    /* likewise; the view cone keeps its real radius */
    }
    if (view_valid && view_forward && isfinite(fov_degrees) && fov_degrees > 0.0f &&
        fov_degrees < 180.0f && isfinite(sensor_radius) && sensor_radius > 0.0f) {
        float length2 = view_forward[0] * view_forward[0] +
                        view_forward[1] * view_forward[1] +
                        view_forward[2] * view_forward[2];
        if (isfinite(length2) && length2 > 1.0e-8f) {
            float inverse_length = 1.0f / shazbot_sqrtf(length2);
            float normalized_forward[3] = {view_forward[0] * inverse_length,
                                           view_forward[1] * inverse_length,
                                           view_forward[2] * inverse_length};
            shazbot_observation_put_vector(values, 26, normalized_forward);
        }
    }
    return 1;
}

static size_t shazbot_encode_observation(unsigned char out[SHAZBOT_OBSERVATION_PACKET_SIZE],
                                         uint64_t sequence, uint64_t monotonic_time_ns,
                                         const float values[SHAZBOT_OBSERVATION_DIMENSIONS]) {
    if (!out || !values) return 0;
    for (size_t i = 0; i < SHAZBOT_OBSERVATION_DIMENSIONS; i++)
        if (!isfinite(values[i])) return 0;
    memcpy(out, "SHZB", 4);
    shazbot_put_u16le(out + 4, 1);
    shazbot_put_u16le(out + 6, 1); /* observation */
    shazbot_put_u32le(out + 8, SHAZBOT_OBSERVATION_PAYLOAD_SIZE);
    shazbot_put_u64le(out + 12, sequence);
    shazbot_put_u64le(out + 20, monotonic_time_ns);
    for (size_t i = 0; i < SHAZBOT_OBSERVATION_DIMENSIONS; i++)
        shazbot_put_f32le(out + SHAZBOT_HEADER_SIZE + i * sizeof(float), values[i]);
    return SHAZBOT_OBSERVATION_PACKET_SIZE;
}

static void shazbot_store_mounted_datablock_id(
    const struct rv_memory *memory, void *player,
    size_t image_slot, size_t feature_base, size_t valid_slot,
    float values[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS]) {
    unsigned char *state = (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                           image_slot * SHAZBOT_MOUNTED_IMAGE_STRIDE;
    if (!rv_memory_range(memory, state, sizeof(uint32_t), PROT_READ)) return;
    void *image_data = rv_pointer(state, 0);
    if (!rv_memory_range(memory, image_data, 0x24, PROT_READ)) return;
    const char *class_name = rv_class_name(memory, image_data);
    if (!class_name || (strcmp(class_name, "ShapeBaseImageData") != 0 &&
                        strcmp(class_name, "TurretImageData") != 0)) return;
    uint32_t image_id = rv_word(image_data, 0x20);
    if (image_id == 0 || image_id > 65535) return;
    for (size_t bit = 0; bit < 16; bit++)
        values[feature_base + bit] = (image_id & (1U << bit)) ? 1.0f : 0.0f;
    values[valid_slot] = 1.0f;
}

static void shazbot_player_mounted_datablock_ids(
    const struct rv_memory *memory, void *player,
    float values[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS]) {
    if (!memory || !player || !values ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                         2 * SHAZBOT_MOUNTED_IMAGE_STRIDE, SHAZBOT_MOUNTED_IMAGE_STRIDE,
                         PROT_READ)) return;
    shazbot_store_mounted_datablock_id(memory, player, 0,
        SHAZBOT_V5_MOUNTED_WEAPON_ID_BITS_BASE, SHAZBOT_V5_MOUNTED_WEAPON_ID_VALID, values);
    shazbot_store_mounted_datablock_id(memory, player, 2,
        SHAZBOT_V5_MOUNTED_PACK_ID_BITS_BASE, SHAZBOT_V5_MOUNTED_PACK_ID_VALID, values);
}

static uint16_t shazbot_stable_name_hash(const char *name) {
    uint32_t hash = 2166136261U;
    if (!name) return 0;
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor; ++cursor) {
        hash ^= *cursor;
        hash *= 16777619U;
    }
    return (uint16_t)((hash ^ (hash >> 16)) & 0xffffU);
}

static void shazbot_report_semantic_name_diagnostic(
    size_t image_slot, const char *stage, void *image_data,
    const char *class_name, uint32_t shape_file_offset, void *shape_pointer,
    const char *shape_name) {
    if (image_slot >= sizeof(unsigned) * 8) return;
    unsigned bit = 1U << image_slot;
    if (shazbot_semantic_name_diagnostic_reported & bit) return;
    fprintf(stderr,
            "[t2_hook] Shazbot v6 shape diagnostic slot=%zu stage=%s image=%p class=%s shapeFile_offset=0x%x shapeFile_ptr=%p shapeFile=%s\n",
            image_slot, stage, image_data,
            class_name ? class_name : "<unreadable>", shape_file_offset, shape_pointer,
            shape_name ? shape_name : "<unreadable>");
    shazbot_semantic_name_diagnostic_reported |= bit;
}

static void shazbot_store_mounted_name_hash(
    const struct rv_memory *memory, void *player, size_t image_slot,
    size_t feature_base, size_t feature_count, size_t valid_slot,
    float values[SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS]) {
    unsigned char *state = (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                           image_slot * SHAZBOT_MOUNTED_IMAGE_STRIDE;
    if (!rv_memory_range(memory, state, sizeof(uint32_t), PROT_READ)) {
        shazbot_report_semantic_name_diagnostic(image_slot, "slot-unreadable",
                                                NULL, NULL, 0, NULL, NULL);
        return;
    }
    void *image_data = rv_pointer(state, 0);
    if (!image_data) return;
    if (!rv_memory_range(memory, image_data, 0x24, PROT_READ)) {
        shazbot_report_semantic_name_diagnostic(image_slot, "image-unreadable",
                                                image_data, NULL, 0, NULL, NULL);
        return;
    }
    const char *class_name = rv_class_name(memory, image_data);
    if (!class_name || (strcmp(class_name, "ShapeBaseImageData") != 0 &&
                        strcmp(class_name, "TurretImageData") != 0)) {
        shazbot_report_semantic_name_diagnostic(image_slot, "unexpected-class",
                                                image_data, class_name, 0, NULL, NULL);
        return;
    }
    /* Client-created SimDataBlocks receive only their numeric ID from the
       network event, so SimObject::objectName is intentionally unset. The
       datablock's shapeFile is transmitted by ShapeBaseImageData::packData and
       is available through the registered persist-field offset. */
    uint32_t shape_file_offset = 0;
    if (!rv_persist_field_offset(memory, image_data, "shapeFile", &shape_file_offset) ||
        !rv_memory_range(memory, (unsigned char *)image_data + shape_file_offset,
                         sizeof(uint32_t), PROT_READ)) {
        shazbot_report_semantic_name_diagnostic(image_slot, "shapeFile-field-not-found",
                                                image_data, class_name, shape_file_offset,
                                                NULL, NULL);
        return;
    }
    void *shape_pointer = rv_pointer(image_data, shape_file_offset);
    const char *shape_name = rv_string(memory, shape_pointer, 128);
    if (!shape_name || !shape_name[0]) {
        shazbot_report_semantic_name_diagnostic(image_slot, "shapeFile-unreadable-or-empty",
                                                image_data, class_name, shape_file_offset,
                                                shape_pointer, NULL);
        return;
    }
    shazbot_report_semantic_name_diagnostic(image_slot, "ok", image_data,
                                            class_name, shape_file_offset,
                                            shape_pointer, shape_name);
    const uint16_t stable_hash = shazbot_stable_name_hash(shape_name);
    for (size_t bit = 0; bit < feature_count; ++bit)
        values[feature_base + bit] = (stable_hash & (1U << bit)) ? 1.0f : 0.0f;
    values[valid_slot] = 1.0f;
}

static void shazbot_player_mounted_name_hashes(
    const struct rv_memory *memory, void *player,
    float values[SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS]) {
    if (!memory || !player || !values ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                         2 * SHAZBOT_MOUNTED_IMAGE_STRIDE, SHAZBOT_MOUNTED_IMAGE_STRIDE,
                          PROT_READ)) return;
    shazbot_store_mounted_name_hash(memory, player, 0,
        SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_BITS_BASE,
        SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_BITS_COUNT,
        SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_VALID, values);
    shazbot_store_mounted_name_hash(memory, player, 2,
        SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_BITS_BASE,
        SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_BITS_COUNT,
        SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_VALID, values);
}

#define SHAZBOT_HUD_WEAPON_ORDER_CAPACITY 16
#define SHAZBOT_HUD_WEAPON_NAME_CAPACITY 64

static void shazbot_hud_weapon_clear_state(void) {
    memset(shazbot_hud_weapon_order, 0, sizeof shazbot_hud_weapon_order);
    shazbot_hud_weapon_count = 0;
    shazbot_hud_active_weapon[0] = 0;
    shazbot_hud_weapon_context_valid = 1;
    shazbot_report_once(&shazbot_hud_weapon_clear_reported,
                        "client weapon-HUD inventory reset was observed");
}

static const char *shazbot_stock_hud_weapon_name(int hud_slot) {
    static const char *const names[] = {
        "Blaster", "Plasma", "Chaingun", "Disc", "GrenadeLauncher",
        "SniperRifle", "ELFGun", "Mortar", "MissileLauncher",
        "TargetingLaser", "ShockLance", "TR2Disc", "TR2GrenadeLauncher",
        "TR2Chaingun", "TR2GoldTargetingLaser", "TR2SilverTargetingLaser",
        "TR2ShockLance", "TR2Mortar",
    };
    if (hud_slot < 0 || (size_t)hud_slot >= sizeof names / sizeof names[0]) return NULL;
    return names[hud_slot];
}

static void shazbot_hud_weapon_update_state(int hud_slot, const char *name, int add_item) {
    const char *resolved_name = name;
    if (!resolved_name || !resolved_name[0])
        resolved_name = shazbot_stock_hud_weapon_name(hud_slot);
    if (shazbot_hud_weapon_item_events_logged < 64) {
        fprintf(stderr,
                "[t2_hook] Shazbot: weapon-HUD item event slot=%d name=%s resolved=%s add=%d\n",
                hud_slot, name ? name : "", resolved_name ? resolved_name : "", add_item);
        shazbot_hud_weapon_item_events_logged++;
    }
    if (!resolved_name || !resolved_name[0] ||
        strlen(resolved_name) >= SHAZBOT_HUD_WEAPON_NAME_CAPACITY) return;
    shazbot_hud_weapon_context_valid = 1;
    shazbot_report_once(&shazbot_hud_weapon_item_reported,
                        "client weapon-HUD item updates were observed");
    size_t found = shazbot_hud_weapon_count;
    for (size_t index = 0; index < shazbot_hud_weapon_count; ++index) {
        if (strcmp(shazbot_hud_weapon_order[index], resolved_name) == 0) {
            found = index;
            break;
        }
    }
    if (add_item) {
        if (found != shazbot_hud_weapon_count) return;
        if (shazbot_hud_weapon_count >= SHAZBOT_HUD_WEAPON_ORDER_CAPACITY) {
            shazbot_hud_weapon_context_valid = 0;
            return;
        }
        snprintf(shazbot_hud_weapon_order[shazbot_hud_weapon_count],
                  sizeof shazbot_hud_weapon_order[0], "%s", resolved_name);
        shazbot_hud_weapon_count++;
        return;
    }
    if (found == shazbot_hud_weapon_count) {
        shazbot_hud_weapon_context_valid = 0;
        return;
    }
    for (size_t index = found; index + 1 < shazbot_hud_weapon_count; ++index)
        memcpy(shazbot_hud_weapon_order[index], shazbot_hud_weapon_order[index + 1],
               sizeof shazbot_hud_weapon_order[index]);
    memset(shazbot_hud_weapon_order[--shazbot_hud_weapon_count], 0,
           sizeof shazbot_hud_weapon_order[0]);
    if (strcmp(shazbot_hud_active_weapon, resolved_name) == 0)
        shazbot_hud_active_weapon[0] = 0;
}

static void shazbot_hud_weapon_active_state(const char *name) {
    if (!name || strlen(name) >= sizeof shazbot_hud_active_weapon) {
        shazbot_hud_active_weapon[0] = 0;
        return;
    }
    snprintf(shazbot_hud_active_weapon, sizeof shazbot_hud_active_weapon, "%s", name);
    shazbot_report_once(&shazbot_hud_weapon_active_reported,
                        "client weapon-HUD selection updates were observed");
}

static uint32_t shazbot_weapon_item_code(const char *name) {
    static const struct { const char *name; uint32_t code; } codes[] = {
        {"Blaster", 1}, {"Plasma", 2}, {"Chaingun", 3}, {"Disc", 4},
        {"GrenadeLauncher", 5}, {"SniperRifle", 6}, {"ELFGun", 7},
        {"Mortar", 8}, {"MissileLauncher", 9}, {"TargetingLaser", 10},
        {"ShockLance", 11}, {"TR2Disc", 4}, {"TR2GrenadeLauncher", 5},
        {"TR2Chaingun", 3}, {"TR2GoldTargetingLaser", 10},
        {"TR2SilverTargetingLaser", 10}, {"TR2ShockLance", 11},
        {"TR2Mortar", 8},
    };
    if (!name) return 0;
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; ++i)
        if (strcmp(name, codes[i].name) == 0) return codes[i].code;
    return 0;
}

static void shazbot_store_hud_weapon_slot_codes(
    float values[SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS]) {
    if (!values || !shazbot_hud_weapon_context_valid) return;
    for (size_t slot = 0; slot < SHAZBOT_V7_WEAPON_SLOT_COUNT; ++slot) {
        if (slot >= shazbot_hud_weapon_count) continue;
        const uint32_t code = shazbot_weapon_item_code(shazbot_hud_weapon_order[slot]);
        for (size_t bit = 0; bit < SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_PER_SLOT; ++bit)
            values[SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_BASE +
                   slot * SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_PER_SLOT + bit] =
                (code & (1U << bit)) ? 1.0f : 0.0f;
    }
}

static void shazbot_apply_hud_weapon_context(
    float values[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS]) {
    if (!values || !shazbot_hud_weapon_context_valid) return;
    memset(values + 34, 0, (47 - 34) * sizeof values[0]);
    const size_t slot_count = shazbot_hud_weapon_count < 6 ? shazbot_hud_weapon_count : 6;
    for (size_t slot = 0; slot < slot_count; ++slot)
        values[41 + slot] = 1.0f;
    values[47] = 1.0f;
    if (!shazbot_hud_active_weapon[0]) return;
    for (size_t slot = 0; slot < slot_count; ++slot) {
        if (strcmp(shazbot_hud_weapon_order[slot], shazbot_hud_active_weapon) == 0) {
            values[34 + slot] = 1.0f;
            values[40] = 1.0f;
            return;
        }
    }
}

static int shazbot_pack_command_index(const char *shape_file) {
    static const char *const shape_files[12] = {
        "pack_upgrade_energy.dts", "pack_upgrade_repair.dts",
        "pack_upgrade_shield.dts", "pack_upgrade_cloaking.dts",
        "pack_upgrade_sensorjammer.dts", "pack_upgrade_ammo.dts",
        "pack_upgrade_satchel.dts", "pack_deploy_inventory.dts",
        "pack_deploy_turreti.dts", "pack_deploy_turreto.dts",
        "pack_deploy_sensor_motion.dts", "pack_deploy_sensor_pulse.dts",
    };
    if (!shape_file) return -1;
    for (int index = 0; index < 12; ++index)
        if (strcmp(shape_file, shape_files[index]) == 0) return index;
    return -1;
}

static void shazbot_apply_mounted_pack_availability(
    const struct rv_memory *memory, void *player,
    float values[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS]) {
    unsigned char *state = (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                           2 * SHAZBOT_MOUNTED_IMAGE_STRIDE;
    if (!rv_memory_range(memory, state, sizeof(uint32_t), PROT_READ)) return;
    void *image_data = rv_pointer(state, 0);
    const char *class_name = rv_class_name(memory, image_data);
    if (!class_name || (strcmp(class_name, "ShapeBaseImageData") != 0 &&
                        strcmp(class_name, "TurretImageData") != 0)) return;
    uint32_t shape_file_offset = 0;
    if (!rv_persist_field_offset(memory, image_data, "shapeFile", &shape_file_offset) ||
        !rv_memory_range(memory, (unsigned char *)image_data + shape_file_offset,
                         sizeof(uint32_t), PROT_READ)) return;
    const char *shape_file = rv_string(memory,
        rv_pointer(image_data, shape_file_offset), 128);
    if (!shape_file || !shape_file[0]) return;
    const int index = shazbot_pack_command_index(shape_file);
    if (index < 0) return;
    memset(values + 48, 0, 12 * sizeof values[0]);
    values[48 + index] = 1.0f;
    values[60] = 1.0f;
}

static int shazbot_player_command_context(
    const struct rv_memory *memory, void *dictionary, void *player,
    float values[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS]) {
    static const char *const pack_items[12] = {
        "EnergyPack", "RepairPack", "ShieldPack", "CloakingPack",
        "SensorJammerPack", "AmmoPack", "SatchelCharge", "InventoryDeployable",
        "TurretIndoorDeployable", "TurretOutdoorDeployable",
        "MotionSensorDeployable", "PulseSensorDeployable",
    };
    if (!memory || !dictionary || !player || !values) return 0;

    float health_fraction = 0.0f;
    if (shazbot_player_health_fraction(memory, dictionary, player, &health_fraction)) {
        values[32] = health_fraction;
        values[33] = 1.0f;
    }
    shazbot_player_mounted_datablock_ids(memory, player, values);

    float weapon_count_value = 0.0f;
    if (!rv_dynamic_float(memory, player, "weaponSlotCount", &weapon_count_value) ||
        weapon_count_value < 0.0f || weapon_count_value > 32.0f ||
        (float)(int)weapon_count_value != weapon_count_value) return 1;
    const size_t weapon_count = (size_t)weapon_count_value;
    int inventory_valid = 1;
    const unsigned char *weapon_image = (const unsigned char *)player +
        SHAZBOT_MOUNTED_IMAGES_OFFSET; /* $WeaponSlot is 0 in the shipped scripts. */
    if (!rv_memory_range(memory, weapon_image, SHAZBOT_MOUNTED_IMAGE_STRIDE, PROT_READ))
        inventory_valid = 0;
    const char *current_weapon = NULL;
    if (inventory_valid) {
        void *image_data = rv_pointer(weapon_image, 0);
        const char *image_class = rv_class_name(memory, image_data);
        if (image_class && strcmp(image_class, "ShapeBaseImageData") == 0)
            current_weapon = rv_dynamic_field(memory, image_data, "item");
    }

    for (size_t slot = 0; slot < 6; slot++) {
        char field_name[24];
        snprintf(field_name, sizeof field_name, "weaponSlot[%zu]", slot);
        const char *weapon_name = rv_dynamic_field(memory, player, field_name);
        if (slot < weapon_count && !weapon_name) inventory_valid = 0;
        if (slot < weapon_count && weapon_name && weapon_name[0]) {
            char inventory_field[96];
            int used = snprintf(inventory_field, sizeof inventory_field, "inv[%s]", weapon_name);
            float count = 0.0f;
            if (used < 0 || (size_t)used >= sizeof inventory_field) inventory_valid = 0;
            else if (rv_dynamic_float(memory, player, inventory_field, &count) && count > 0.0f)
                values[41 + slot] = 1.0f;
            if (current_weapon && strcmp(current_weapon, weapon_name) == 0) {
                values[34 + slot] = 1.0f;
                values[40] = 1.0f;
            }
        }
    }

    if (!inventory_valid) {
        memset(values + 34, 0, (47 - 34) * sizeof values[0]);
        return 1;
    }
    values[47] = 1.0f;
    for (size_t index = 0; index < 12; index++) {
        char inventory_field[48];
        snprintf(inventory_field, sizeof inventory_field, "inv[%s]", pack_items[index]);
        float count = 0.0f;
        if (rv_dynamic_float(memory, player, inventory_field, &count) && count > 0.0f)
            values[48 + index] = 1.0f;
    }
    values[60] = 1.0f;
    return 1;
}

static size_t shazbot_encode_command_observation_v5(
    unsigned char out[SHAZBOT_COMMAND_OBSERVATION_V5_PACKET_SIZE], uint64_t sequence,
    uint64_t monotonic_time_ns,
    const float values[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS]) {
    if (!out || !values) return 0;
    for (size_t index = 0; index < SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS; index++)
        if (!isfinite(values[index])) return 0;
    memcpy(out, "SHZB", 4);
    shazbot_put_u16le(out + 4, 1);
    shazbot_put_u16le(out + 6, SHAZBOT_COMMAND_OBSERVATION_V5_KIND);
    shazbot_put_u32le(out + 8, SHAZBOT_COMMAND_OBSERVATION_V5_PAYLOAD_SIZE);
    shazbot_put_u64le(out + 12, sequence);
    shazbot_put_u64le(out + 20, monotonic_time_ns);
    for (size_t index = 0; index < SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS; index++)
        shazbot_put_f32le(out + SHAZBOT_HEADER_SIZE + index * sizeof(float), values[index]);
    return SHAZBOT_COMMAND_OBSERVATION_V5_PACKET_SIZE;
}

static size_t shazbot_encode_command_observation_v6(
    unsigned char out[SHAZBOT_COMMAND_OBSERVATION_V6_PACKET_SIZE], uint64_t sequence,
    uint64_t monotonic_time_ns,
    const float values[SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS]) {
    if (!out || !values) return 0;
    for (size_t index = 0; index < SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS; index++)
        if (!isfinite(values[index])) return 0;
    memcpy(out, "SHZB", 4);
    shazbot_put_u16le(out + 4, 1);
    shazbot_put_u16le(out + 6, SHAZBOT_COMMAND_OBSERVATION_V6_KIND);
    shazbot_put_u32le(out + 8, SHAZBOT_COMMAND_OBSERVATION_V6_PAYLOAD_SIZE);
    shazbot_put_u64le(out + 12, sequence);
    shazbot_put_u64le(out + 20, monotonic_time_ns);
    for (size_t index = 0; index < SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS; index++)
        shazbot_put_f32le(out + SHAZBOT_HEADER_SIZE + index * sizeof(float), values[index]);
    return SHAZBOT_COMMAND_OBSERVATION_V6_PACKET_SIZE;
}

static size_t shazbot_encode_command_observation_v7(
    unsigned char out[SHAZBOT_COMMAND_OBSERVATION_V7_PACKET_SIZE], uint64_t sequence,
    uint64_t monotonic_time_ns,
    const float values[SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS]) {
    if (!out || !values) return 0;
    for (size_t index = 0; index < SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS; index++)
        if (!isfinite(values[index])) return 0;
    memcpy(out, "SHZB", 4);
    shazbot_put_u16le(out + 4, 1);
    shazbot_put_u16le(out + 6, SHAZBOT_COMMAND_OBSERVATION_V7_KIND);
    shazbot_put_u32le(out + 8, SHAZBOT_COMMAND_OBSERVATION_V7_PAYLOAD_SIZE);
    shazbot_put_u64le(out + 12, sequence);
    shazbot_put_u64le(out + 20, monotonic_time_ns);
    for (size_t index = 0; index < SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS; index++)
        shazbot_put_f32le(out + SHAZBOT_HEADER_SIZE + index * sizeof(float), values[index]);
    return SHAZBOT_COMMAND_OBSERVATION_V7_PACKET_SIZE;
}

static const char *shazbot_notification_socket_path(void) {
    const char *configured = hook_option_str("telemetry.shazbot_notification_socket");
    return configured && configured[0] ? configured : SHAZBOT_NOTIFICATION_SOCKET;
}

static int shazbot_open_notification_socket(void) {
    if (shazbot_notification_socket_fd >= 0) return 1;
    const char *path = shazbot_notification_socket_path();
    const size_t path_length = path ? strlen(path) : 0;
    if (path_length == 0 || path_length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_length + 1);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                           path_length + 1);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    if (bind(fd, (const struct sockaddr *)&address, address_length) != 0) {
        int bind_error = errno;
        if (bind_error == EADDRINUSE) {
            struct stat info;
            if (lstat(path, &info) == 0 &&
                S_ISSOCK(info.st_mode) && info.st_uid == geteuid()) {
                int probe = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
                int active = probe >= 0 &&
                    connect(probe, (const struct sockaddr *)&address, address_length) == 0;
                if (probe >= 0) close(probe);
                if (!active && unlink(path) == 0 &&
                    bind(fd, (const struct sockaddr *)&address, address_length) == 0) {
                    bind_error = 0;
                } else {
                    errno = bind_error;
                }
            }
        }
        if (bind_error != 0) {
            close(fd);
            errno = bind_error;
            return 0;
        }
    }
    (void)chmod(path, S_IRUSR | S_IWUSR);
    shazbot_notification_socket_fd = fd;
    return 1;
}

static int shazbot_decode_notification(const unsigned char *frame, size_t length,
                                       uint64_t *sequence, uint32_t *notification) {
    if (!frame || !sequence || !notification || length != SHAZBOT_NOTIFICATION_PACKET_SIZE ||
        memcmp(frame, "SHZB", 4) != 0 || shazbot_get_u16le(frame + 4) != 1 ||
        shazbot_get_u16le(frame + 6) != SHAZBOT_NOTIFICATION_KIND ||
        shazbot_get_u32le(frame + 8) != SHAZBOT_NOTIFICATION_PAYLOAD_SIZE) return 0;
    *sequence = shazbot_get_u64le(frame + 12);
    *notification = shazbot_get_u32le(frame + SHAZBOT_HEADER_SIZE);
    return *notification == SHAZBOT_NOTIFICATION_CAPTURE_SAVED ||
           *notification == SHAZBOT_NOTIFICATION_CAPTURE_START;
}

static void shazbot_start_auto_command_capture(void) {
    if (shazbot_auto_command_capture_active) return;
    if (!hook_option_bool("telemetry.shazbot_command_labels")) {
        shazbot_report_once(&shazbot_auto_command_capture_reported,
                            "automatic command capture requested, but command labels are disabled");
        return;
    }
    if (!shazbot_local_player_valid || !Con__evaluate) {
        shazbot_report_once(&shazbot_auto_command_capture_reported,
                            "automatic command capture requested without a valid Player/console");
        return;
    }
    shazbot_auto_command_capture_active = 1;
    shazbot_command_label_capture_active = 1;
    shazbot_auto_command_capture_reported = 0;
    (void)Con__evaluate(
        "shazbot_command_capture_start(); "
        "clientCmdCenterPrint(\"Shazbot command capture started\", 6, 1);",
        0, "t2launch/shazbot_auto_capture.cs", 0);
    if (hook_option_bool("telemetry.shazbot_auto_weapon_slot_test")) {
        const char *relative_script = hook_option_str("telemetry.shazbot_weapon_slot_test_script");
        if (!relative_script || strncmp(relative_script, "prefs/", 6) != 0 ||
            strchr(relative_script, '"') || strstr(relative_script, "..")) {
            shazbot_report_once(&shazbot_auto_command_capture_reported,
                                "weapon-slot test script path is invalid");
        } else {
            char eval_script[512];
            int length = snprintf(eval_script, sizeof eval_script, "exec(\"%s\");", relative_script);
            if (length < 0 || (size_t)length >= sizeof eval_script) {
                shazbot_report_once(&shazbot_auto_command_capture_reported,
                                    "weapon-slot test script path is too long");
            } else {
                const char *script_status = Con__evaluate(
                    eval_script, 0, "t2launch/shazbot_weapon_slot_test.cs", 0);
                fprintf(stderr, "[t2_hook] Shazbot weapon-slot test script loaded: %s%s%s\n",
                        relative_script, script_status ? " (" : "",
                        script_status ? script_status : "");
            }
        }
    }
    fprintf(stderr, "[t2_hook] Shazbot automatic command capture started\n");
}

static void shazbot_stop_auto_command_capture(void) {
    if (!shazbot_auto_command_capture_active) return;
    shazbot_auto_command_capture_active = 0;
    shazbot_command_label_capture_active = 0;
    (void)Con__evaluate(
        "shazbot_command_capture_stop(); "
        "clientCmdCenterPrint(\"Shazbot command labels saved\", 6, 1);",
        0, "t2launch/shazbot_auto_capture.cs", 0);
    fprintf(stderr, "[t2_hook] Shazbot automatic command capture stopped\n");
}

static void shazbot_handle_notification(uint32_t notification) {
    if (notification == SHAZBOT_NOTIFICATION_CAPTURE_START) {
        shazbot_start_auto_command_capture();
    } else if (notification == SHAZBOT_NOTIFICATION_CAPTURE_SAVED) {
        int command_capture_was_active = shazbot_auto_command_capture_active;
        if (command_capture_was_active)
            shazbot_stop_auto_command_capture();
        if (shazbot_autoplay_demo_active && Con__evaluate) {
            shazbot_autoplay_demo_active = 0;
            fprintf(stderr, "[t2_hook] Shazbot capture saved; exiting autoplay demo client\n");
            (void)Con__evaluate("schedule(1, 0, \"quit\");", 0,
                                "t2launch/autoplay_demo_exit.cs", 0);
        } else if (!command_capture_was_active && Con__evaluate) {
            (void)Con__evaluate(
                "clientCmdCenterPrint(\"Shazbot v5 trace saved\", 6, 1);",
                0, "t2launch/shazbot_notification.cs", 0);
        }
    }
}

static void shazbot_poll_notifications(void) {
    if (!hook_option_bool("telemetry.shazbot")) return;
    if (!shazbot_open_notification_socket()) {
        shazbot_report_once(&shazbot_notification_socket_error_reported,
                            "could not open the local centerprint notification socket");
        return;
    }
    unsigned char frame[SHAZBOT_NOTIFICATION_PACKET_SIZE];
    for (;;) {
        ssize_t received = recv(shazbot_notification_socket_fd, frame, sizeof frame, 0);
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
                !shazbot_notification_socket_error_reported) {
                fprintf(stderr, "[t2_hook] Shazbot notification receive failed: %s\n", strerror(errno));
                shazbot_notification_socket_error_reported = 1;
            }
            if (errno == EINTR) continue;
            break;
        }
        uint64_t sequence = 0;
        uint32_t notification = 0;
        if (!shazbot_decode_notification(frame, (size_t)received, &sequence, &notification)) continue;
        if (shazbot_notification_sequence_valid &&
            sequence <= shazbot_notification_last_sequence) continue;
        shazbot_notification_sequence_valid = 1;
        shazbot_notification_last_sequence = sequence;
        shazbot_handle_notification(notification);
    }
}

static int shazbot_open_socket(void) {
    const char *configured = hook_option_str("telemetry.shazbot_socket");
    if (!configured || !configured[0]) configured = "/tmp/shazbot.sock";
    size_t length = configured ? strnlen(configured, sizeof shazbot_address.sun_path) : 0;
    if (!length || length >= sizeof shazbot_address.sun_path) return 0;
    if (shazbot_socket_fd >= 0 && strcmp(configured, shazbot_socket_path) == 0) return 1;
    if (shazbot_socket_fd >= 0) close(shazbot_socket_fd);
    shazbot_socket_fd = -1;
    memset(&shazbot_address, 0, sizeof shazbot_address);
    shazbot_address.sun_family = AF_UNIX;
    memcpy(shazbot_address.sun_path, configured, length + 1);
    shazbot_address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    shazbot_socket_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (shazbot_socket_fd < 0) return 0;
    memcpy(shazbot_socket_path, configured, length + 1);
    return 1;
}

static uint64_t shazbot_packet_sequence;
static ssize_t shazbot_real_sendto(int fd, const void *packet, size_t length, int flags,
                                   const struct sockaddr *address, socklen_t address_length);

static void shazbot_forward_server_packet(const void *packet, size_t length,
                                          const struct sockaddr *peer,
                                          socklen_t peer_length) {
    if (!hook_option_bool("telemetry.shazbot_packet_capture") || !packet ||
        length < 3 || length > SHAZBOT_MAX_GAME_PACKET_SIZE || !peer ||
        (peer->sa_family != AF_INET && peer->sa_family != AF_INET6) ||
        !(((const unsigned char *)packet)[0] & 1U) ||
        ((((const unsigned char *)packet)[2] >> 4) & 3U) != 0U)
        return;
    if (!shazbot_open_socket()) return;

    unsigned char frame[SHAZBOT_HOOK_SERVER_PACKET_FRAME_SIZE];
    memcpy(frame, "SHZB", 4);
    shazbot_put_u16le(frame + 4, 1);
    shazbot_put_u16le(frame + 6, SHAZBOT_HOOK_SERVER_PACKET_KIND);
    shazbot_put_u32le(frame + 8, (uint32_t)(2 + length));
    const uint64_t sequence = __atomic_add_fetch(&shazbot_packet_sequence, 1, __ATOMIC_RELAXED);
    shazbot_put_u64le(frame + 12, sequence);
    struct timespec now;
    uint64_t timestamp = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0)
        timestamp = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    shazbot_put_u64le(frame + 20, timestamp);
    shazbot_put_u16le(frame + SHAZBOT_HEADER_SIZE, (uint16_t)length);
    memcpy(frame + SHAZBOT_HEADER_SIZE + 2, packet, length);
    (void)peer_length;
    (void)shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                                shazbot_address_length, frame,
                                SHAZBOT_HEADER_SIZE + 2 + length,
                                shazbot_real_sendto);
}

typedef ssize_t (*shazbot_recvfrom_fn)(int, void *, size_t, int,
                                       struct sockaddr *, socklen_t *);
typedef ssize_t (*shazbot_recv_fn)(int, void *, size_t, int);

ssize_t recvfrom(int fd, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *address_length) {
    static shazbot_recvfrom_fn real_recvfrom;
    if (!real_recvfrom) real_recvfrom = (shazbot_recvfrom_fn)next_symbol("recvfrom");
    if (!real_recvfrom) { errno = ENOSYS; return -1; }
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof peer;
    struct sockaddr *destination = address ? address : (struct sockaddr *)&peer;
    socklen_t *destination_length = address_length ? address_length : &peer_length;
    ssize_t received = real_recvfrom(fd, buffer, length, flags, destination,
                                     destination_length);
    if (received > 0) {
        const struct sockaddr *source = address ? address : (struct sockaddr *)&peer;
        const socklen_t source_length = address_length ? *address_length : peer_length;
        shazbot_forward_server_packet(buffer, (size_t)received, source, source_length);
    }
    return received;
}

ssize_t recv(int fd, void *buffer, size_t length, int flags) {
    static shazbot_recv_fn real_recv;
    if (!real_recv) real_recv = (shazbot_recv_fn)next_symbol("recv");
    if (!real_recv) { errno = ENOSYS; return -1; }
    ssize_t received = real_recv(fd, buffer, length, flags);
    if (received > 0) {
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof peer;
        if (getpeername(fd, (struct sockaddr *)&peer, &peer_length) == 0)
            shazbot_forward_server_packet(buffer, (size_t)received,
                                          (const struct sockaddr *)&peer, peer_length);
    }
    return received;
}

static ssize_t shazbot_real_sendto(int fd, const void *packet, size_t length, int flags,
                                   const struct sockaddr *address, socklen_t address_length) {
    return sendto(fd, packet, length, flags, address, address_length);
}

static int shazbot_parse_vector_text(const struct rv_memory *memory, const char *text,
                                     float vector[3]) {
    if (!rv_string(memory, text, 96)) return 0;
    char extra;
    if (sscanf(text, "%f %f %f %c", &vector[0], &vector[1], &vector[2], &extra) != 3)
        return 0;
    return isfinite(vector[0]) && isfinite(vector[1]) && isfinite(vector[2]);
}

/* Emit engine ShapeBase muzzle-point/vector rays for mounted image slots.
   getMuzzleVector returns the native corrected aim direction when available;
   this is a ray cue, not a calculated projectile impact/ballistic trajectory. */
static void shazbot_emit_muzzle_rays(const struct rv_memory *memory, void *player,
                                     uint32_t player_id, float detection_radius,
                                     uint64_t timestamp) {
    if (!memory || !player || !isfinite(detection_radius) || detection_radius <= 0.0f ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET,
                         SHAZBOT_MOUNTED_IMAGE_COUNT * SHAZBOT_MOUNTED_IMAGE_STRIDE, PROT_READ))
        return;
    void *vtable = rv_pointer(player, 0);
    if (!rv_memory_range(memory, vtable, 0x170, PROT_READ) ||
        !rv_memory_range(memory, rv_pointer(vtable, 0x154), 1, PROT_EXEC) ||
        !rv_memory_range(memory, rv_pointer(vtable, 0x16c), 1, PROT_EXEC)) return;

    for (uint32_t slot = 0; slot < SHAZBOT_MOUNTED_IMAGE_COUNT; slot++) {
        unsigned char *image_state = (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                                     (size_t)slot * SHAZBOT_MOUNTED_IMAGE_STRIDE;
        if (!rv_memory_range(memory, image_state, 0x30, PROT_READ) ||
            !rv_memory_range(memory, rv_pointer(image_state, 0x2c), 0x10, PROT_READ)) continue;
        void *image_data = rv_pointer(image_state, 0);
        if (!rv_memory_range(memory, image_data, 0x30, PROT_READ)) continue;
        const char *image_class = rv_class_name(memory, image_data);
        if (!image_class || strcmp(image_class, "ShapeBaseImageData") != 0) continue;

        char slot_text[16];
        snprintf(slot_text, sizeof slot_text, "%u", slot);
        const char *argv[3] = {"getMuzzle", "", slot_text};
        const char *point_text = ((const char *(*)(void *, int, const char **))
                                  SHAZBOT_MUZZLE_POINT_CALLBACK)(player, 3, argv);
        float muzzle[3], direction[3];
        if (!shazbot_parse_vector_text(memory, point_text, muzzle)) continue;
        const char *vector_text = ((const char *(*)(void *, int, const char **))
                                   SHAZBOT_MUZZLE_VECTOR_CALLBACK)(player, 3, argv);
        if (!shazbot_parse_vector_text(memory, vector_text, direction)) continue;
        float length2 = direction[0] * direction[0] + direction[1] * direction[1] +
                        direction[2] * direction[2];
        if (!isfinite(length2) || length2 < 1.0e-8f) continue;
        float aim[3] = {
            muzzle[0] + direction[0] * detection_radius,
            muzzle[1] + direction[1] * detection_radius,
            muzzle[2] + direction[2] * detection_radius,
        };
        unsigned char packet[SHAZBOT_MUZZLE_AIM_PACKET_SIZE];
        uint64_t sequence = shazbot_sequence + 1;
        size_t length = shazbot_encode_muzzle_aim(packet, sequence, timestamp,
                                                   player_id, slot, 0, muzzle, aim,
                                                   detection_radius);
        if (!length) continue;
        if (shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                                  shazbot_address_length, packet, length,
                                  shazbot_real_sendto)) {
            shazbot_sequence = sequence;
            shazbot_socket_error_reported = 0;
        } else if (!shazbot_socket_error_reported) {
            fprintf(stderr, "[t2_hook] Shazbot muzzle ray dropped: %s\n", strerror(errno));
            shazbot_socket_error_reported = 1;
        }
    }
}

struct shazbot_native_aim_sample {
    uint32_t player_id, image_slot, target_id;
    float muzzle[3], aim[3], detection_radius;
};

/* Find which mounted image supplied this renderBeacon projectile pointer. */
static int shazbot_image_slot_for_projectile(const struct rv_memory *memory, void *player,
                                             void *projectile_data, uint32_t *slot_out) {
    if (!memory || !player || !projectile_data || !slot_out ||
        !rv_memory_range(memory, (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET,
                         SHAZBOT_MOUNTED_IMAGE_COUNT * SHAZBOT_MOUNTED_IMAGE_STRIDE, PROT_READ))
        return 0;
    for (uint32_t slot = 0; slot < SHAZBOT_MOUNTED_IMAGE_COUNT; slot++) {
        unsigned char *state = (unsigned char *)player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                               (size_t)slot * SHAZBOT_MOUNTED_IMAGE_STRIDE;
        if (!rv_memory_range(memory, state, 0x30, PROT_READ) ||
            !rv_memory_range(memory, rv_pointer(state, 0x2c), 0x10, PROT_READ)) continue;
        void *image_data = rv_pointer(state, 0);
        if (!rv_memory_range(memory, image_data, SHAZBOT_IMAGE_PROJECTILE_OFFSET + 4, PROT_READ)) continue;
        const char *class_name = rv_class_name(memory, image_data);
        if (!class_name || strcmp(class_name, "ShapeBaseImageData") != 0) continue;
        if (rv_pointer(image_data, SHAZBOT_IMAGE_PROJECTILE_OFFSET) == projectile_data) {
            *slot_out = slot;
            return 1;
        }
    }
    return 0;
}

/* This parent-frame reader is pinned to GameTSCtrl::renderBeacon's #25034 ABI:
   beacon object at EBP+0x0c, native source/muzzle Point3F* at +0x14, and
   ProjectileData* at +0x1c. The aim point comes from its existing native
   dglDrawBillboard call after ProjectileData::calculateAim succeeds. */
static int shazbot_capture_native_aim(const struct rv_memory *memory, void *dictionary,
                                      void *parent_frame,
                                      const float *aim_point,
                                      struct shazbot_native_aim_sample *sample) {
    if (!memory || !parent_frame || !aim_point || !sample ||
        !shazbot_spatial_context_valid || !shazbot_spatial_player ||
        !rv_memory_range(memory, parent_frame, 0x20, PROT_READ) ||
        !rv_memory_range(memory, aim_point, 3 * sizeof(float), PROT_READ)) return 0;
    if (!rv_memory_range(memory, dictionary, 4096 * sizeof(uint32_t), PROT_READ) ||
        rv_find_object(memory, dictionary, shazbot_spatial_player_id) != shazbot_spatial_player)
        return 0;

    void *beacon = rv_pointer(parent_frame, 0x0c);
    void *source_position = rv_pointer(parent_frame, 0x14);
    void *projectile_data = rv_pointer(parent_frame, 0x1c);
    if (!rv_memory_range(memory, source_position, 3 * sizeof(float), PROT_READ) ||
        !rv_memory_range(memory, projectile_data, 0x20, PROT_READ))
        return 0;
    const char *beacon_class = rv_class_name(memory, beacon);
    if (!beacon_class || strcmp(beacon_class, "BeaconObject") != 0) return 0;
    uint32_t target_id = rv_word(beacon, 0x20);
    if (!target_id || rv_find_object(memory, dictionary, target_id) != beacon) return 0;

    uint32_t image_slot;
    if (!shazbot_image_slot_for_projectile(memory, shazbot_spatial_player,
                                           projectile_data, &image_slot)) return 0;
    sample->player_id = shazbot_spatial_player_id;
    sample->image_slot = image_slot;
    sample->target_id = target_id;
    for (int i = 0; i < 3; i++) {
        sample->muzzle[i] = rv_float(source_position, i * 4);
        sample->aim[i] = aim_point[i];
        if (!isfinite(sample->muzzle[i]) || !isfinite(sample->aim[i])) return 0;
    }
    sample->detection_radius = shazbot_spatial_detection_radius;
    return isfinite(sample->detection_radius) && sample->detection_radius > 0.0f;
}

static int shazbot_aim_sample_due(uint32_t player_id, uint32_t image_slot,
                                  uint32_t target_id, uint64_t epoch) {
    static uint32_t player_ids[64], image_slots[64], target_ids[64];
    static uint64_t epochs[64];
    for (size_t i = 0; i < sizeof target_ids / sizeof target_ids[0]; i++) {
        if (player_ids[i] == player_id && image_slots[i] == image_slot && target_ids[i] == target_id) {
            if (epochs[i] == epoch) return 0;
            epochs[i] = epoch;
            return 1;
        }
    }
    size_t slot = shazbot_aim_key_cursor++ % (sizeof target_ids / sizeof target_ids[0]);
    player_ids[slot] = player_id;
    image_slots[slot] = image_slot;
    target_ids[slot] = target_id;
    epochs[slot] = epoch;
    return 1;
}

static void shazbot_emit_native_aim(const struct rv_memory *memory, void *parent_frame,
                                    const float *aim_point) {
    struct shazbot_native_aim_sample sample;
    void *dictionary = rv_memory_range(memory, (void *)SHAZBOT_ID_DICTIONARY,
                                       sizeof(uint32_t), PROT_READ)
        ? rv_pointer((void *)SHAZBOT_ID_DICTIONARY, 0) : NULL;
    if (!shazbot_capture_native_aim(memory, dictionary, parent_frame, aim_point, &sample) ||
        !shazbot_aim_sample_due(sample.player_id, sample.image_slot, sample.target_id,
                                shazbot_spatial_epoch)) return;
    unsigned char packet[SHAZBOT_MUZZLE_AIM_PACKET_SIZE];
    uint64_t sequence = shazbot_sequence + 1;
    size_t length = shazbot_encode_muzzle_aim(packet, sequence, shazbot_spatial_epoch,
        sample.player_id, sample.image_slot, sample.target_id,
        sample.muzzle, sample.aim, sample.detection_radius);
    if (!length) return;
    if (shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                              shazbot_address_length, packet, length, shazbot_real_sendto)) {
        shazbot_sequence = sequence;
        shazbot_socket_error_reported = 0;
    } else if (!shazbot_socket_error_reported) {
        fprintf(stderr, "[t2_hook] Shazbot native aim point dropped: %s\n", strerror(errno));
        shazbot_socket_error_reported = 1;
    }
}

static const unsigned char shazbot_native_billboard_prologue[] = {
    0x55,0x89,0xe5,0x81,0xec,0xfc,0x00,0x00,0x00,0x57,0x56,0x53
};

static int shazbot_install_aim_billboard_hook(void) {
    if (shazbot_aim_hook_attempted) return shazbot_aim_hook_installed;
    shazbot_aim_hook_attempted = 1;
    if (target != &game_targets[0] || target->dedicated ||
        !hook_option_bool("telemetry.shazbot")) return 0;
    int protection;
    if (!readable_range(SHAZBOT_AIM_BILLBOARD_CALL, sizeof shazbot_aim_billboard_call_original,
                        &protection) || !(protection & PROT_EXEC) ||
        memcmp((void *)SHAZBOT_AIM_BILLBOARD_CALL, shazbot_aim_billboard_call_original,
               sizeof shazbot_aim_billboard_call_original) != 0 ||
        !readable_range(SHAZBOT_NATIVE_BILLBOARD, sizeof shazbot_native_billboard_prologue,
                        &protection) || !(protection & PROT_EXEC) ||
        memcmp((void *)SHAZBOT_NATIVE_BILLBOARD, shazbot_native_billboard_prologue,
               sizeof shazbot_native_billboard_prologue) != 0) {
        fprintf(stderr, "[t2_hook] Shazbot native aim hook refused: target bytes differ\n");
        return 0;
    }
    int32_t relative = (int32_t)((uintptr_t)shazbot_aim_billboard_wrapper -
                                  (SHAZBOT_AIM_BILLBOARD_CALL + sizeof shazbot_aim_billboard_call_original));
    memcpy(shazbot_aim_billboard_call_patch + 1, &relative, sizeof relative);
    const struct code_patch patch = {
        SHAZBOT_AIM_BILLBOARD_CALL, shazbot_aim_billboard_call_original,
        sizeof shazbot_aim_billboard_call_original, 0,
        shazbot_aim_billboard_call_patch, sizeof shazbot_aim_billboard_call_patch
    };
    shazbot_aim_hook_installed = apply_code_patches("Shazbot native aim telemetry", &patch, 1);
    if (shazbot_aim_hook_installed)
        fprintf(stderr, "[t2_hook] Shazbot native aim-point capture enabled (memory only)\n");
    return shazbot_aim_hook_installed;
}

__attribute__((noinline))
static void shazbot_aim_billboard_wrapper(const float *hit, float width, float rotation) {
    if (hook_option_bool("telemetry.shazbot") && shazbot_spatial_context_valid) {
        void *frame = __builtin_frame_address(0);
        void *parent_frame = rv_memory_range(&shazbot_spatial_memory, frame,
                                             sizeof(uint32_t), PROT_READ)
            ? rv_pointer(frame, 0) : NULL;
        shazbot_emit_native_aim(&shazbot_spatial_memory, parent_frame, hit);
    }
    shazbot_native_billboard(hit, width, rotation);
}

static void poll_shazbot_telemetry(void) {
    /* Client target only: never apply any of these offsets to the dedicated build. */
    if (!hook_option_bool("telemetry.shazbot")) {
        shazbot_spatial_context_valid = 0;
        shazbot_local_player_valid = 0;
        return;
    }
    if (target != &game_targets[0] || target->dedicated) {
        shazbot_report_once(&shazbot_target_reported,
                            "telemetry is enabled, but this is not the supported client target");
        shazbot_spatial_context_valid = 0;
        shazbot_local_player_valid = 0;
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return;
    uint64_t timestamp = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    if (shazbot_last_attempt_ns && timestamp >= shazbot_last_attempt_ns &&
        timestamp - shazbot_last_attempt_ns < SHAZBOT_SAMPLE_PERIOD_NS) return;
    shazbot_last_attempt_ns = timestamp;
    const int demo_playing = review_demo_playing();
    if (demo_playing && (!shazbot_demo_state_initialized || !shazbot_demo_state)) {
        fprintf(stderr, "[t2_hook] Shazbot: demo playback start monotonic_ns=%llu\n",
                (unsigned long long)timestamp);
    }
    shazbot_demo_state = demo_playing;
    shazbot_demo_state_initialized = 1;
    shazbot_spatial_context_valid = 0;

    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory)) {
        shazbot_report_once(&shazbot_memory_reported,
                            "waiting for a readable client memory snapshot");
        shazbot_local_player_valid = 0;
        return;
    }
    if (!rv_memory_range(&memory, (void *)SHAZBOT_CONNECTION, sizeof(uint32_t), PROT_READ) ||
        !rv_memory_range(&memory, (void *)SHAZBOT_ID_DICTIONARY, sizeof(uint32_t), PROT_READ) ||
        !rv_memory_range(&memory, (void *)SHAZBOT_TARGET_MANAGER, sizeof(uint32_t), PROT_READ)) {
        shazbot_report_once(&shazbot_globals_reported,
                            "client connection/object globals are not readable at the expected build anchors");
        shazbot_local_player_valid = 0;
        return;
    }
    void *connection = rv_pointer((void *)SHAZBOT_CONNECTION, 0);
    void *dictionary = rv_pointer((void *)SHAZBOT_ID_DICTIONARY, 0);
    void *target_manager = rv_pointer((void *)SHAZBOT_TARGET_MANAGER, 0);
    void *visible_set = rv_memory_range(&memory, (void *)SHAZBOT_VISIBLE_SHAPEBASE_SET,
                                        sizeof(uint32_t), PROT_READ)
        ? rv_pointer((void *)SHAZBOT_VISIBLE_SHAPEBASE_SET, 0) : NULL;
    void *client_target_list = rv_memory_range(&memory, (void *)SHAZBOT_CLIENT_TARGET_LIST,
                                               sizeof(uint32_t), PROT_READ)
        ? rv_pointer((void *)SHAZBOT_CLIENT_TARGET_LIST, 0) : NULL;
    float xyz[3];
    uint32_t player_id;
    void *player = NULL;
    const char *player_failure = NULL;
    if (!shazbot_local_player_position_detail(&memory, connection, dictionary, xyz,
                                               &player_id, &player, &player_failure)) {
        shazbot_local_player_valid = 0;
        const char *reason = player_failure ? player_failure : "unknown reason";
        if (strcmp(shazbot_player_failure_state, reason) != 0 ||
            timestamp < shazbot_player_status_time_ns ||
            timestamp - shazbot_player_status_time_ns >= 15000000000ULL) {
            fprintf(stderr, "[t2_hook] Shazbot: local Player validation waiting: %s\n", reason);
            snprintf(shazbot_player_failure_state, sizeof shazbot_player_failure_state,
                     "%s", reason);
            shazbot_player_status_time_ns = timestamp;
        }
        return;
    }
    shazbot_local_player_valid = 1;
    if (shazbot_player_failure_state[0]) {
        fprintf(stderr, "[t2_hook] Shazbot: local Player validation recovered\n");
        shazbot_player_failure_state[0] = '\0';
        shazbot_player_status_time_ns = 0;
    }
    if (!shazbot_open_socket()) {
        if (!shazbot_socket_open_reported) {
            const char *path = hook_option_str("telemetry.shazbot_socket");
            char message[256];
            snprintf(message, sizeof message, "could not open telemetry socket %s: %s",
                     path && path[0] ? path : "/tmp/shazbot.sock", strerror(errno));
            shazbot_report_once(&shazbot_socket_open_reported, message);
        }
        return;
    }

    unsigned char packet[SHAZBOT_PACKET_SIZE];
    uint64_t sequence = shazbot_sequence + 1;
    size_t length = shazbot_encode_telemetry(packet, sequence, timestamp, xyz, 1.0f);
    if (!length) return;
    if (shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                              shazbot_address_length, packet, length, shazbot_real_sendto)) {
        shazbot_sequence = sequence;
        shazbot_socket_error_reported = 0;
        shazbot_report_once(&shazbot_telemetry_sent_reported,
                            "position telemetry is being sent to the configured socket");
    } else if (!shazbot_socket_error_reported) {
        fprintf(stderr, "[t2_hook] Shazbot telemetry sample dropped: %s\n", strerror(errno));
        shazbot_socket_error_reported = 1;
    }

    /* Optional view data requires all of its #25034 camera/SensorData anchors;
       its absence masks only the view slots, not the observation frame. */
    int spatial_anchors_valid = shazbot_spatial_anchors_valid(&memory);
    if (!spatial_anchors_valid) {
        if (!shazbot_spatial_anchor_reported) {
            fprintf(stderr, "[t2_hook] Shazbot view cone disabled: target spatial anchors differ\n");
            shazbot_spatial_anchor_reported = 1;
        }
    }
    int view_valid = 0;
    float origin[3], forward[3], fov_degrees = 0.0f, detection_radius = 0.0f;
    if (spatial_anchors_valid) {
        if (!shazbot_aim_hook_attempted) (void)shazbot_install_aim_billboard_hook();
        view_valid = shazbot_local_view_cone(&memory, player, player_id, target_manager,
                                              origin, forward, &fov_degrees, &detection_radius);
        if (view_valid) {
            shazbot_spatial_memory = memory;
            shazbot_spatial_player = player;
            shazbot_spatial_player_id = player_id;
            shazbot_spatial_detection_radius = detection_radius;
            shazbot_spatial_epoch = timestamp;
            shazbot_spatial_context_valid = 1;
            unsigned char view_packet[SHAZBOT_VIEW_PACKET_SIZE];
            uint64_t view_sequence = shazbot_sequence + 1;
            size_t view_length = shazbot_encode_view_cone(view_packet, view_sequence, timestamp,
                                                           player_id, origin, forward,
                                                           fov_degrees, detection_radius);
            if (view_length && shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                                                      shazbot_address_length, view_packet, view_length,
                                                      shazbot_real_sendto)) {
                shazbot_sequence = view_sequence;
                shazbot_socket_error_reported = 0;
            } else if (!shazbot_socket_error_reported) {
                fprintf(stderr, "[t2_hook] Shazbot view-cone sample dropped: %s\n", strerror(errno));
                shazbot_socket_error_reported = 1;
            }
            shazbot_emit_muzzle_rays(&memory, player, player_id, detection_radius, timestamp);
        }
    }

    const char *anchor_failure = shazbot_observation_anchor_failure(&memory);
    if (anchor_failure) {
        if (!shazbot_observation_anchor_reported) {
            fprintf(stderr, "[t2_hook] Shazbot Observation disabled: %s\n", anchor_failure);
            shazbot_observation_anchor_reported = 1;
        }
        return;
    }
    float values[SHAZBOT_OBSERVATION_DIMENSIONS];
    if (!shazbot_collect_observation(&memory, connection, dictionary, target_manager,
                                     visible_set, client_target_list, view_valid,
                                     view_valid ? forward : NULL, fov_degrees,
                                     detection_radius, values)) {
        shazbot_report_once(&shazbot_observation_collect_reported,
                            "local Player is valid, but observation fields could not be collected");
        return;
    }
    unsigned char observation_packet[SHAZBOT_OBSERVATION_PACKET_SIZE];
    uint64_t observation_sequence = shazbot_sequence + 1;
    size_t observation_length = shazbot_encode_observation(observation_packet,
                                                           observation_sequence,
                                                           timestamp, values);
    if (observation_length &&
        shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                              shazbot_address_length, observation_packet,
                              observation_length, shazbot_real_sendto)) {
        shazbot_sequence = observation_sequence;
        shazbot_socket_error_reported = 0;
        shazbot_report_once(&shazbot_observation_sent_reported,
                            "policy observations are being sent to the configured socket");
    } else if (!shazbot_socket_error_reported) {
        fprintf(stderr, "[t2_hook] Shazbot Observation sample dropped: %s\n", strerror(errno));
        shazbot_socket_error_reported = 1;
    }

    float command_values[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS] = {0};
    memcpy(command_values, values, sizeof values);
    if (shazbot_player_command_context(&memory, dictionary, player, command_values)) {
        shazbot_apply_hud_weapon_context(command_values);
        shazbot_apply_mounted_pack_availability(&memory, player, command_values);
        unsigned char command_packet[SHAZBOT_COMMAND_OBSERVATION_V5_PACKET_SIZE];
        uint64_t command_sequence = shazbot_sequence + 1;
        size_t command_length = shazbot_encode_command_observation_v5(
            command_packet, command_sequence, timestamp, command_values);
        if (command_length &&
            shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                                  shazbot_address_length, command_packet, command_length,
                                  shazbot_real_sendto)) {
            shazbot_sequence = command_sequence;
            shazbot_socket_error_reported = 0;
            shazbot_report_once(&shazbot_command_context_reported,
                                "v5 health, weapon-slot, and pack inventory context is being sent");
        } else if (!shazbot_socket_error_reported) {
            fprintf(stderr, "[t2_hook] Shazbot v5 command context dropped: %s\n", strerror(errno));
            shazbot_socket_error_reported = 1;
        }

        /* V6 retains the v4/health/inventory prefix and adds mounted image
           asset-path hashes. */
        float semantic_values[SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS] = {0};
        memcpy(semantic_values, command_values,
               SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_BITS_BASE * sizeof(float));
        shazbot_player_mounted_name_hashes(&memory, player, semantic_values);
        unsigned char semantic_packet[SHAZBOT_COMMAND_OBSERVATION_V6_PACKET_SIZE];
        uint64_t semantic_sequence = shazbot_sequence + 1;
        size_t semantic_length = shazbot_encode_command_observation_v6(
            semantic_packet, semantic_sequence, timestamp, semantic_values);
        if (semantic_length &&
            shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                                  shazbot_address_length, semantic_packet, semantic_length,
                                  shazbot_real_sendto)) {
            shazbot_sequence = semantic_sequence;
            shazbot_socket_error_reported = 0;
            shazbot_report_once(&shazbot_semantic_command_context_reported,
                                "v6 stable mounted weapon/pack shapeFile path hashes are being sent");
        } else if (!shazbot_socket_error_reported) {
            fprintf(stderr, "[t2_hook] Shazbot v6 semantic command context dropped: %s\n",
                    strerror(errno));
            shazbot_socket_error_reported = 1;
        }

        /* V7 replaces mounted-only IDs/hashes with the stable script identity
           of each ordered weapon slot, matching the numeric selection actions. */
        float weapon_slot_values[SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS] = {0};
        memcpy(weapon_slot_values, command_values,
               SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_BASE * sizeof(float));
        shazbot_store_hud_weapon_slot_codes(weapon_slot_values);
        unsigned char weapon_slot_packet[SHAZBOT_COMMAND_OBSERVATION_V7_PACKET_SIZE];
        uint64_t weapon_slot_sequence = shazbot_sequence + 1;
        size_t weapon_slot_length = shazbot_encode_command_observation_v7(
            weapon_slot_packet, weapon_slot_sequence, timestamp, weapon_slot_values);
        if (weapon_slot_length &&
            shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                                  shazbot_address_length, weapon_slot_packet,
                                  weapon_slot_length, shazbot_real_sendto)) {
            shazbot_sequence = weapon_slot_sequence;
            shazbot_socket_error_reported = 0;
            shazbot_report_once(&shazbot_weapon_slot_context_reported,
                                "v7 ordered weapon-slot item identities are being sent");
        } else if (!shazbot_socket_error_reported) {
            fprintf(stderr, "[t2_hook] Shazbot v7 weapon-slot context dropped: %s\n",
                    strerror(errno));
            shazbot_socket_error_reported = 1;
        }

    }
}

static int rv_team(const struct rv_memory *memory, void *object) {
    void *info = rv_pointer(object, 0x26c);
    return rv_memory_range(memory, info, 0x1c, PROT_READ)
        ? (int32_t)rv_word(info, 0x18) : RV_OTHER_TEAM;
}

/* Return the hm2 target category and beacon type; all ShapeBase reads follow
   exact class-name and ghost checks. Waypoints are plain SimObjects. */
static int rv_target_info(const struct rv_memory *memory, void *object,
                          uint32_t self_id, int local_team, struct rv_target *out) {
    const char *name = rv_class_name(memory, object);
    if (!name) return 0;
    uint32_t id = rv_word(object, 0x20);
    if (!id || id == self_id) return 0;
    enum rv_kind kind;
    int type = 0, team;
    if (strcmp(name, "ClientTarget") == 0) {
        if (!rv_memory_range(memory, object, 0x44, PROT_READ) || rv_word(object, 0x30) != 2) return 0;
        kind = RV_WAYPOINT; team = RV_OTHER_TEAM;
    } else {
        if (strcmp(name, "Player") == 0) kind = RV_PLAYER;
        else if (strcmp(name, "Turret") == 0 || strcmp(name, "StaticShape") == 0) kind = RV_SHAPE;
        else return 0;
        if (!rv_memory_range(memory, object, 0x2d4, PROT_READ) || !(rv_word(object, 0x40) & 2)) return 0;
        team = rv_team(memory, object);
        if (kind == RV_SHAPE) {
            void *data = rv_pointer(object, 0x2d0);
            if (!rv_memory_range(memory, data, 0x48, PROT_READ)) return 0;
            const char *file = rv_string(memory, rv_pointer(data, 0x44), 256);
            if (!file) return 0;
            const char *base = strrchr(file, '/');
            file = base ? base + 1 : file;
            if (strcmp(file, "deploy_inventory.dts") == 0 ||
                strcmp(file, "turret_outdoor_deploy.dts") == 0 ||
                strcmp(file, "turret_indoor_deployc.dts") == 0 ||
                strcmp(file, "turret_indoor_deployf.dts") == 0 ||
                strcmp(file, "turret_indoor_deployw.dts") == 0) type = 2;
            else if (strcmp(file, "turret_base_large.dts") != 0 &&
                     strcmp(file, "sensor_pulse_large.dts") != 0 &&
                     strcmp(file, "ext_flagstand.dts") != 0 &&
                     strcmp(file, "vehicle_pad_station.dts") != 0) return 0;
        }
    }
    if (kind == RV_PLAYER ? team == local_team : team <= 0 || (team == local_team && team != RV_OTHER_TEAM))
        return 0;
    float position[3];
    if (!rv_position(memory, object, kind, position)) return 0;
    *out = (struct rv_target){object, id, kind, type, 0};
    return 1;
}

/* -cos(theta)*abs(cos(theta)), up to a common view-vector magnitude factor,
   orders absolute yaw exactly like hm2's acos without needing libm calls. */
static double rv_yaw_rank(const float position[3], const float origin[3], float view_x, float view_y) {
    double x = (double)position[0] - origin[0], y = (double)position[1] - origin[1];
    double length = x * x + y * y;
    if (!length) return -((double)view_x * view_x + (double)view_y * view_y);
    double dot = x * view_x + y * view_y;
    return -dot * (dot < 0 ? -dot : dot) / length;
}
static int rv_compare_targets(const void *left, const void *right) {
    const struct rv_target *a = left, *b = right;
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    return a->id < b->id ? -1 : a->id != b->id;
}

/* Match the reference's separate 64-player/64-object pools, keeping lowest
   IDs when a pool fills so bucket enumeration order does not affect selection. */
static void rv_add_target(const struct rv_target *candidate) {
    size_t same_kind = 0, largest = 0;
    int player = candidate->kind == RV_PLAYER;
    for (size_t i = 0; i < rv_tracking.count; i++) {
        struct rv_target *t = &rv_tracking.targets[i];
        if ((t->kind == RV_PLAYER) != player) continue;
        if (!same_kind || t->id > rv_tracking.targets[largest].id) largest = i;
        same_kind++;
    }
    size_t limit = player ? RV_PLAYER_LIMIT : RV_OBJECT_LIMIT;
    if (same_kind < limit) rv_tracking.targets[rv_tracking.count++] = *candidate;
    else if (candidate->id < rv_tracking.targets[largest].id) rv_tracking.targets[largest] = *candidate;
}

static void rv_rank_targets(const struct rv_memory *memory, void *dictionary,
                            uint32_t self_id, int team, const float origin[3], float vx, float vy) {
    rv_tracking.count = 0;
    unsigned int visited = 0;
    for (size_t bucket = 0; bucket < 4096; bucket++) {
        void *object = rv_pointer(dictionary, bucket * 4);
        for (unsigned int chain = 0; object && chain < 4096 && visited++ < 131072; chain++) {
            if (!rv_memory_range(memory, object, 0x24, PROT_READ)) break;
            void *next = rv_pointer(object, 0x10);
            struct rv_target candidate;
            if (rv_target_info(memory, object, self_id, team, &candidate)) {
                float position[3];
                rv_position(memory, object, candidate.kind, position);
                candidate.rank = rv_yaw_rank(position, origin, vx, vy);
                rv_add_target(&candidate);
            }
            object = next;
        }
    }
    qsort(rv_tracking.targets, rv_tracking.count, sizeof rv_tracking.targets[0], rv_compare_targets);
}

struct rv_beacon_backup { void *object; uint32_t id, type; float xyz[3]; };
static int rv_compare_beacons(const void *left, const void *right) {
    const struct rv_beacon_backup *a = left, *b = right;
    return a->id < b->id ? -1 : a->id != b->id;
}
static size_t rv_collect_beacons(const struct rv_memory *memory, void *dictionary,
                                 void *set, struct rv_beacon_backup beacons[RV_BEACON_LIMIT]) {
    if (!rv_memory_range(memory, set, 0x3c, PROT_READ)) return 0;
    uint32_t count = rv_word(set, 0x30);
    void *data = rv_pointer(set, 0x38);
    if (count > 4096 || !rv_memory_range(memory, data, count * 4, PROT_READ)) return 0;
    size_t used = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *object = rv_pointer(data, i * 4);
        const char *name = rv_class_name(memory, object);
        if (!name || strcmp(name, "BeaconObject") != 0 ||
            !rv_memory_range(memory, object, 0x88c, PROT_READ | PROT_WRITE) ||
            !(rv_word(object, 0x40) & 2)) continue;
        struct rv_beacon_backup candidate = {object, rv_word(object, 0x20), rv_word(object, 0x888), {0}};
        if (!candidate.id || rv_find_object(memory, dictionary, candidate.id) != object ||
            !rv_position(memory, object, RV_SHAPE, candidate.xyz)) continue;
        size_t slot = used;
        if (used == RV_BEACON_LIMIT) {
            slot = 0;
            for (size_t j = 1; j < used; j++) if (beacons[j].id > beacons[slot].id) slot = j;
            if (candidate.id >= beacons[slot].id) continue;
        } else used++;
        beacons[slot] = candidate;
    }
    qsort(beacons, used, sizeof beacons[0], rv_compare_beacons);
    return used;
}

static void rv_set_beacon(void *object, const float xyz[3], uint32_t type) {
    for (int i = 0; i < 3; i++) memcpy((unsigned char *)object + 0xa8 + i * 16, &xyz[i], 4);
    memcpy((unsigned char *)object + 0x888, &type, 4);
}

static void review_beacon_render(void *hud) {
    static int drawing;
    /* This call site belongs to the gameplay HUD, unlike swaps while loading.
       It runs even when target reassignment is off or there are no beacons. */
    if (!drawing && !target->dedicated && review_patches_applied &&
        hook_option_bool("review.cheat_view") && review_demo_playing()) {
        review_model_hud_seen = 1;
        review_model_hud_id = rv_word(hud, 0x20);
    }
    if (drawing || target->dedicated || !review_patches_applied ||
        !hook_option_bool("review.cheat_view") || !hook_option_bool("review.autobeacon") ||
        !review_demo_playing()) {
        if (!drawing) memset(&rv_tracking, 0, sizeof rv_tracking);
        rv_native_beacon_render(hud);
        return;
    }
    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory) ||
        !rv_memory_range(&memory, (void *)RV_CONNECTION, 4, PROT_READ) ||
        !rv_memory_range(&memory, (void *)RV_ID_DICTIONARY, 4, PROT_READ) ||
        !rv_memory_range(&memory, (void *)RV_CLIENT_TARGET_SET, 4, PROT_READ)) {
        rv_native_beacon_render(hud);
        return;
    }
    void *connection = rv_pointer((void *)RV_CONNECTION, 0);
    void *dictionary = rv_pointer((void *)RV_ID_DICTIONARY, 0);
    if (!rv_memory_range(&memory, connection, 0x8438, PROT_READ) ||
        !rv_memory_range(&memory, dictionary, 0x4000, PROT_READ)) {
        rv_native_beacon_render(hud);
        return;
    }
    void *self = rv_pointer(connection, 0x825c);
    int team = (int32_t)rv_word(connection, 0x8434);
    float origin[3];
    if (team <= 0 || !rv_memory_range(&memory, self, 0xcc, PROT_READ) ||
        !rv_position(&memory, self, RV_PLAYER, origin)) {
        memset(&rv_tracking, 0, sizeof rv_tracking);
        rv_native_beacon_render(hud);
        return;
    }
    float vx = rv_float(self, 0xa0), vy = rv_float(self, 0x9c);
    if (!isfinite(vx) || !isfinite(vy) || (vx == 0 && vy == 0)) {
        rv_native_beacon_render(hud);
        return;
    }
    uint32_t self_id = rv_word(self, 0x20);
    uintptr_t stream = rv_word(connection, 0x8220);
    if (rv_tracking.connection != (uintptr_t)connection || rv_tracking.stream != stream ||
        rv_tracking.self_id != self_id || rv_tracking.team != team) {
        memset(&rv_tracking, 0, sizeof rv_tracking);
        rv_tracking.connection = (uintptr_t)connection; rv_tracking.stream = stream;
        rv_tracking.self_id = self_id; rv_tracking.team = team;
    }
    if (rv_tracking.frame++ % RV_REFRESH_FRAMES == 0)
        rv_rank_targets(&memory, dictionary, self_id, team, origin, vx, vy);

    struct rv_beacon_backup beacons[RV_BEACON_LIMIT];
    size_t count = rv_collect_beacons(&memory, dictionary,
        rv_pointer((void *)RV_CLIENT_TARGET_SET, 0), beacons);
    size_t next = 0, assigned = 0;
    for (size_t i = 0; i < count; i++) {
        float xyz[3];
        uint32_t type = 1;  /* hm2's unassigned marker-beacon type */
        memcpy(xyz, beacons[i].xyz, sizeof xyz);
        while (next < rv_tracking.count) {
            const struct rv_target *cached = &rv_tracking.targets[next++];
            void *object = rv_find_object(&memory, dictionary, cached->id);
            struct rv_target live;
            if (object != cached->object ||
                !rv_target_info(&memory, object, self_id, team, &live) || live.kind != cached->kind ||
                !rv_position(&memory, object, live.kind, xyz)) continue;
            type = live.type;
            assigned++;
            break;
        }
        rv_set_beacon(beacons[i].object, xyz, type);
    }
    if (!rv_tracking.reported || rv_tracking.last_beacons != (int)count ||
        rv_tracking.last_targets != (int)assigned) {
        fprintf(stderr, "[t2_hook] Autobeacon: %zu recorded beacons, %zu assigned targets (%zu ranked)\n",
                count, assigned, rv_tracking.count);
        rv_tracking.reported = 1;
        rv_tracking.last_beacons = count; rv_tracking.last_targets = assigned;
    }
    drawing = 1;
    rv_native_beacon_render(hud);
    drawing = 0;
    for (size_t i = 0; i < count; i++)
        rv_set_beacon(beacons[i].object, beacons[i].xyz, beacons[i].type);
}

static int install_browser_bot_limit(void) {
    static const unsigned char limit[] = {0xff};
    const struct code_patch patch = {BROWSER_BOT_LIMIT_ADDRESS, browser_bot_limit_original,
                                     sizeof(browser_bot_limit_original), 1, limit, sizeof(limit)};
    if (!apply_code_patches("Browser bot limit", &patch, 1)) return 0;
    fprintf(stderr, "[t2_hook] Server browser: default maximum bot count 255 instead of 16 (memory only)\n");
    return 1;
}

/* HUD bars (HudBarBaseCtrl: HudEnergy, HudCapacitor, HudDamage, HudHeat)
   register "value" (+0xD4) and "verticalFill" (+0xE0), but onRender
   (0x0818EA00) never stores the fill it computes and only fills
   horizontally. Replace the stretch of onRender that asks the bar for its
   fill (vtable +0x118) and lays out the fill rectangle with a call to
   hud_bar_layout(), which stores the value and can fill from the bottom, as
   the Windows l33thax patch did. The stretch leaves {x, y, width, height}
   at [ebp-0x10] and scratch at [ebp-0x18]; the stock code continues at
   0x0818EAD1 with the colour and the pulse. The rectangle is the bar's
   subRegion (+0xC4, x y width height) moved to the bar's screen offset, not
   its extent. */
#define HUD_BAR_LAYOUT_ADDRESS 0x0818EA5E
#define HUD_BAR_LAYOUT_RESUME  0x0818EAD1
#define HUD_BAR_VALUE_OFFSET   0xD4
#define HUD_BAR_VERTICAL_OFFSET 0xE0
#define HUD_BAR_FLIP_VERTICAL_OFFSET   0xC2  /* HudBitmapCtrl's flipVertical */
#define HUD_BAR_FLIP_HORIZONTAL_OFFSET 0xC3  /* and flipHorizontal */
static const unsigned char hud_bar_layout_original[] = {
    0x8b, 0x06, 0x83, 0xc4, 0xf4, 0x56, 0x8b, 0x80, 0x18, 0x01, 0x00, 0x00, 0xff, 0xd0, 0x8b, 0x96,
    0xc4, 0x00, 0x00, 0x00, 0x8b, 0x86, 0xc8, 0x00, 0x00, 0x00, 0x03, 0x13, 0x03, 0x43, 0x04, 0x89,
    0x55, 0xe8, 0x89, 0x45, 0xec, 0x89, 0x55, 0xf0, 0x89, 0x45, 0xf4, 0x8b, 0x96, 0xcc, 0x00, 0x00,
    0x00, 0x89, 0x55, 0xf8, 0x8b, 0x86, 0xd0, 0x00, 0x00, 0x00, 0x89, 0x55, 0xe4, 0xdb, 0x45, 0xe4,
    0xde, 0xc9, 0x83, 0xc4, 0x10, 0x89, 0x45, 0xfc, 0xd9, 0x7d, 0xe2, 0x66, 0x8b, 0x55, 0xe2, 0x66,
    0x81, 0xca, 0x00, 0x0c, 0x66, 0x89, 0x55, 0xe0, 0xd9, 0x6d, 0xe0, 0xdb, 0x5d, 0xe4, 0x8b, 0x45,
    0xe4, 0xd9, 0x6d, 0xe2, 0x89, 0x45, 0xf8, 0x83, 0xf8, 0x01, 0x75, 0x07, 0xc7, 0x45, 0xf8, 0x02,
    0x00, 0x00, 0x00};
static int hud_bar_store_value, hud_bar_vertical_fill, hud_bar_flip;

/* The stock fill: the length times the value in x87 extended precision,
   truncated, never exactly 1. */
static int32_t hud_bar_fill(int32_t length, float value) {
    int32_t fill = (int32_t)((long double)length * value);
    return fill == 1 ? 2 : fill;
}

/* frame is [ebp-0x18]: two scratch words, then the fill rectangle.
   With ui.bar_flip, the bars use the flipVertical and flipHorizontal fields
   they inherit from HudBitmapCtrl, which only mirror a bitmap: the flip
   along the fill reverses its direction (a vertical bar fills from the top,
   a horizontal one from the right), and the flip across it inverts the bar,
   which then shows 1 - value (0% full, 100% empty). */
static void hud_bar_layout(int32_t *frame, unsigned char *control, const int32_t *offset) {
    typedef float (*bar_value_t)(void *control);
    bar_value_t bar_value = *(bar_value_t *)(*(unsigned char **)control + 0x118);
    float value = bar_value(control);
    if (hud_bar_store_value) *(float *)(control + HUD_BAR_VALUE_OFFSET) = value;
    int32_t x = *(int32_t *)(control + 0xC4) + offset[0];
    int32_t y = *(int32_t *)(control + 0xC8) + offset[1];
    int32_t width = *(int32_t *)(control + 0xCC), height = *(int32_t *)(control + 0xD0);
    frame[0] = frame[2] = x;
    frame[1] = frame[3] = y;
    int vertical = hud_bar_vertical_fill && control[HUD_BAR_VERTICAL_OFFSET];
    int reverse = 0;
    if (hud_bar_flip) {
        int flip_vertical = control[HUD_BAR_FLIP_VERTICAL_OFFSET] != 0;
        int flip_horizontal = control[HUD_BAR_FLIP_HORIZONTAL_OFFSET] != 0;
        reverse = vertical ? flip_vertical : flip_horizontal;
        if (vertical ? flip_horizontal : flip_vertical)
            value = value >= 1.0f ? 0.0f : value <= 0.0f ? 1.0f : 1.0f - value;
    }
    if (vertical) {
        int32_t fill = hud_bar_fill(height, value);
        frame[3] = reverse ? y : y + height - fill;  /* from the top, or the bottom */
        frame[4] = width;
        frame[5] = fill;
    } else {
        int32_t fill = hud_bar_fill(width, value);
        frame[2] = reverse ? x + width - fill : x;  /* from the right, or the left */
        frame[4] = fill;
        frame[5] = height;
    }
}

/* GuiControl::getValue() asks the control for a script value, which HUD bars
   don't have, so it returns "". HudBarBaseCtrl::getValue() returns the fill
   hud_bar_layout() stored instead. */
static const char *hud_bar_get_value_cb(void *obj, S32 argc, const char **argv) {
    (void)argc; (void)argv;
    static char value[32];
    snprintf(value, sizeof(value), "%g", *(float *)((unsigned char *)obj + HUD_BAR_VALUE_OFFSET));
    return value;
}

/* ── ~w soundpack crash guard (security.safe_soundpacks) ──────────────────
 * A chat message carrying "~w<path>" makes the client play <path> as a sound:
 * scripts/client.cs hands <path> to the console functions alxGetWaveLen and
 * alxCreateSource, which, for a name that is not a registered audio object,
 * parse it as a WAV file through the Loki OpenAL loader. Pointing ~w at a file
 * that exists but is not a WAV (a config, a .vl2, a shared library) makes the
 * loader trust bogus RIFF/WAVE chunk sizes and crash the client with a signal
 * (no Windows-style dialog, but just as forceful an exit). Any player, relayed
 * by the server, or the server itself can send such a line.
 *
 * The hook overrides both console functions with wrappers that return the
 * engine's own safe "0" when the argument names a loose file that is not a
 * valid WAV, so the fragile parser never sees it. Registered sounds, files
 * inside .vl2 volumes and real .wav soundpacks pass through to the original
 * unchanged: the guard only ever blocks a file it positively opened and
 * positively rejected, which could not have produced audio anyway, so it can
 * never silence a legitimate sound. */
#define ALX_GET_WAVE_LEN_CB   0x080CE9B4
#define ALX_CREATE_SOURCE_CB  0x080CDD10
/* Both are int console functions (a length in ms, an audio handle), not string
 * ones, so the originals return S32 and the wrappers format the result. */
typedef S32 (*alx_int_cb_t)(void *obj, S32 argc, const char **argv);
static alx_int_cb_t real_alx_get_wave_len, real_alx_create_source;
/* A loose ~w file larger than this is blocked before it is parsed, so a valid
 * but enormous WAV cannot be used to stall or exhaust memory; 0 = no cap. The
 * engine's own $MaxMessageWavLength still caps playback duration. */
static off_t soundpack_max_bytes;

/* 1 = a loose file that is definitely not a valid WAV, so block it; 0 = pass
 * through (not a loose file at all — an object name, a volume entry or missing
 * — or a well-formed WAV). */
static int file_blocked_as_nonwav(const char *path) {
    if (!path || !path[0]) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;                        /* not a loose file: pass through */
    off_t size = lseek(fd, 0, SEEK_END);
    if (soundpack_max_bytes > 0 && size > soundpack_max_bytes) {
        close(fd);                               /* valid or not, too big to load */
        return 1;
    }
    unsigned char head[12];
    int valid = 0;
    if (size >= 44 && pread(fd, head, sizeof(head), 0) == (ssize_t)sizeof(head) &&
        memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
        off_t off = 12;
        int fmt_ok = 0, data_ok = 0;
        for (int i = 0; i < 32 && off + 8 <= size; i++) {
            unsigned char chunk[8];
            if (pread(fd, chunk, 8, off) != 8) break;
            uint32_t len = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (uint32_t)chunk[7] << 24;
            if (memcmp(chunk, "fmt ", 4) == 0 && len >= 16) fmt_ok = 1;
            if (memcmp(chunk, "data", 4) == 0) {   /* the audio bytes; must fit the file */
                data_ok = (uint64_t)off + 8 + len <= (uint64_t)size;
                break;
            }
            off += 8 + len + (len & 1);            /* RIFF chunks are word-aligned */
        }
        valid = fmt_ok && data_ok;
    }
    close(fd);
    return !valid;                               /* opened but not a valid WAV: block */
}

static const char *alx_get_wave_len_safe_cb(void *obj, S32 argc, const char **argv) {
    static char result[16];
    if (argc >= 2 && file_blocked_as_nonwav(argv[1])) return "0";
    snprintf(result, sizeof(result), "%d",
             real_alx_get_wave_len ? real_alx_get_wave_len(obj, argc, argv) : 0);
    return result;
}

/* alxCreateSource(description, filename, [x y z]): argv[2] is the filename.
 * The profile/position forms put an object name or a number there, which is
 * not a loose file and passes through. */
static const char *alx_create_source_safe_cb(void *obj, S32 argc, const char **argv) {
    static char result[16];
    if (argc >= 3 && file_blocked_as_nonwav(argv[2])) return "0";
    snprintf(result, sizeof(result), "%d",
             real_alx_create_source ? real_alx_create_source(obj, argc, argv) : 0);
    return result;
}

static int install_safe_soundpacks(void) {
    int prot;
    if (!(readable_range(ALX_GET_WAVE_LEN_CB, 3, &prot) && (prot & PROT_EXEC) &&
          memcmp((void *)ALX_GET_WAVE_LEN_CB, "\x55\x89\xe5", 3) == 0 &&
          readable_range(ALX_CREATE_SOURCE_CB, 3, &prot) && (prot & PROT_EXEC) &&
          memcmp((void *)ALX_CREATE_SOURCE_CB, "\x55\x89\xe5", 3) == 0)) {
        fprintf(stderr, "[t2_hook] Safe soundpacks refused: audio callback bytes mismatch\n");
        return 0;
    }
    real_alx_get_wave_len = (alx_int_cb_t)ALX_GET_WAVE_LEN_CB;
    real_alx_create_source = (alx_int_cb_t)ALX_CREATE_SOURCE_CB;
    long kbytes = hook_long_or_default("security.max_soundpack_kbytes", 8192, 0, LONG_MAX / 1024L);
    soundpack_max_bytes = kbytes > 0 ? (off_t)kbytes * 1024 : 0;
    /* Re-register the global functions so script calls reach the wrappers; the
     * arg counts match the engine's so normal calls still validate. The engine
     * registers these at audio init, before this runs, so the override wins. */
    Con__addMethodS(NULL, "alxGetWaveLen", alx_get_wave_len_safe_cb,
        "alxGetWaveLen( profile|filename )", 2, 2);
    Con__addMethodS(NULL, "alxCreateSource", alx_create_source_safe_cb,
        "alxCreateSource( description, [filename], [x y z] )", 2, 6);
    if (soundpack_max_bytes > 0)
        fprintf(stderr, "[t2_hook] Safe soundpacks: ~w chat sounds that are not valid WAV files, or "
                "larger than %ld KiB, are blocked (memory only)\n", kbytes);
    else
        fprintf(stderr, "[t2_hook] Safe soundpacks: ~w chat sounds that are not valid WAV files are "
                "blocked, no size cap (memory only)\n");
    return 1;
}

static int install_hud_bars(int store_value, int vertical_fill, int flip) {
    /* push ebx (offset); push esi (control); lea eax,[ebp-0x18]; push eax;
       call hud_bar_layout; add esp,12; jmp to the stock colour code */
    unsigned char code[sizeof(hud_bar_layout_original)];
    memset(code, 0x90, sizeof(code));
    static const unsigned char head[] = {0x53, 0x56, 0x8d, 0x45, 0xe8, 0x50};
    memcpy(code, head, sizeof(head));
    size_t at = sizeof(head);
    int32_t call = (int32_t)((uintptr_t)hud_bar_layout - (HUD_BAR_LAYOUT_ADDRESS + at + 5));
    code[at] = 0xe8;
    memcpy(code + at + 1, &call, sizeof(call));
    at += 5;
    code[at++] = 0x83; code[at++] = 0xc4; code[at++] = 0x0c;
    int32_t resume = (int32_t)(HUD_BAR_LAYOUT_RESUME - (HUD_BAR_LAYOUT_ADDRESS + at + 5));
    code[at] = 0xe9;
    memcpy(code + at + 1, &resume, sizeof(resume));
    const struct code_patch patch = {HUD_BAR_LAYOUT_ADDRESS, hud_bar_layout_original,
                                     sizeof(hud_bar_layout_original), 0, code, sizeof(code)};
    hud_bar_store_value = store_value;
    hud_bar_vertical_fill = vertical_fill;
    hud_bar_flip = flip;
    if (!apply_code_patches("HUD bar", &patch, 1)) {
        hud_bar_store_value = hud_bar_vertical_fill = hud_bar_flip = 0;
        return 0;
    }
    fprintf(stderr, "[t2_hook] HUD bars:%s%s%s (memory only)\n",
            store_value ? " getValue() reports the fill;" : "",
            vertical_fill ? " verticalFill works;" : "",
            flip ? " flipVertical/flipHorizontal reverse or invert the fill;" : "");
    return 1;
}

/* The IFF render skips its markers when the control object is a Camera
   (observer mode); jump past that check (je 0x0819370F becomes jmp), as the
   Windows l33thax patch did at 0x5068EF. */
#define OBSERVER_IFF_ADDRESS 0x081936BF
static const unsigned char observer_iff_original[] = {0x74, 0x4e, 0x8b, 0x88, 0xbc, 0x08, 0x00, 0x00};

static int install_observer_iff(void) {
    static const unsigned char jump[] = {0xeb};
    const struct code_patch patch = {OBSERVER_IFF_ADDRESS, observer_iff_original,
                                     sizeof(observer_iff_original), 0, jump, sizeof(jump)};
    if (!apply_code_patches("Observer IFF", &patch, 1)) return 0;
    fprintf(stderr, "[t2_hook] Observers see IFF markers (memory only)\n");
    return 1;
}

/* The client refuses to connect to a server flagged LAN when the server's
   address is not in one of the client's own class-B subnets ("Cannot join a
   LAN server with no matching Class B IP address"). After the interface-match
   loop, a "no match" flag at -0x26b9(%ebp) is still set and the error path
   runs; the success path is at 0x08237190. Make the post-loop branch
   unconditional so the join always succeeds, as the Windows t2classbpatch did
   for tribes2.exe. Client only. */
#define LAN_JOIN_CHECK_ADDRESS 0x082370CE
static const unsigned char lan_join_check_original[] = {0x0f, 0x84, 0xbc, 0x00, 0x00, 0x00};

static int install_lan_join_anywhere(void) {
    /* je 0x08237190 (0f 84 bc 00 00 00) -> jmp 0x08237190 (e9 bd 00 00 00) + nop */
    static const unsigned char jump[] = {0xe9, 0xbd, 0x00, 0x00, 0x00, 0x90};
    const struct code_patch patch = {LAN_JOIN_CHECK_ADDRESS, lan_join_check_original,
                                     sizeof(lan_join_check_original), 0, jump, sizeof(jump)};
    if (!apply_code_patches("LAN join", &patch, 1)) return 0;
    fprintf(stderr, "[t2_hook] Connect to LAN servers outside the local subnet (memory only)\n");
    return 1;
}

/* The game clamps $pref::visibleDistanceMod (the float at 0x0849518C) to
   [0.5, 1.0] in a small clamp function: at 0x081DC7F2 `je 0x081DC80E` is taken
   only when the value is above 1.0 and forces it back to 1.0. NOP that branch
   so a value above 1.0 survives (farther visible distance), the Linux
   equivalent of PenguinOfDoom's Windows visdist patch. The write-back is to the
   pref itself, so every reader picks up the larger value with no further
   change; the 0.5 lower bound is left intact and the FPU stack stays balanced.
   Client only. */
#define VISDIST_CLAMP_ADDRESS 0x081DC7F2
static const unsigned char visdist_clamp_original[] = {0x74, 0x1a};

static int install_visible_distance(void) {
    static const unsigned char nops[] = {0x90, 0x90};
    const struct code_patch patch = {VISDIST_CLAMP_ADDRESS, visdist_clamp_original,
                                     sizeof(visdist_clamp_original), 0, nops, sizeof(nops)};
    if (!apply_code_patches("Visible distance", &patch, 1)) return 0;
    fprintf(stderr, "[t2_hook] visibleDistanceMod is no longer capped at 1.0 (memory only)\n");
    return 1;
}

/* GameConnection::readPacket stores the control object's whiteout (the
   concussion/flash blind) as readFloat(7) * 1.5 at +0x844c, the same offset
   as the Windows build. Load 0 instead of the 1.5 scale (fldl -> fldz), so
   the received whiteout is always 0: the 7 bits are still read, so the
   stream stays in sync, and the FPU stack depth is unchanged. Only what this
   client receives changes; a listen server it hosts still sends real values
   (those come from ShapeBase +0x7b0 through getWhiteOut). The Linux
   equivalent of HappyMod 2's "no whiteout". Client only. */
#define NO_WHITEOUT_ADDRESS 0x08275B38
static const unsigned char no_whiteout_original[] = {0xdd, 0x05, 0xa8, 0xc8, 0x45, 0x08};

static int install_no_whiteout(void) {
    static const unsigned char zero[] = {0xd9, 0xee, 0x90, 0x90, 0x90, 0x90};
    const struct code_patch patch = {NO_WHITEOUT_ADDRESS, no_whiteout_original,
                                     sizeof(no_whiteout_original), 0, zero, sizeof(zero)};
    if (!apply_code_patches("No whiteout", &patch, 1)) return 0;
    fprintf(stderr, "[t2_hook] Received flash whiteout is dropped (memory only)\n");
    return 1;
}

/* setFov() (0x0823415C) clamps the field of view to 1-179 degrees: a load
   of 179.0 at 0x08452028, then fld1. Load 360.0 from here and 0.0 (fldz)
   instead, as the Windows l33thax patch did. */
#define FOV_MAX_LOAD 0x08234171
#define FOV_MIN_LOAD 0x08234186
static const unsigned char fov_max_load_original[] = {0xd9, 0x05, 0x28, 0x20, 0x45, 0x08};
static const unsigned char fov_min_load_original[] = {0xd9, 0xe8, 0xd8, 0xd1};  /* fld1; fcom st(1) */
static const float fov_range_max = 360.0f;

static int install_fov_range(void) {
    unsigned char max_load[6] = {0xd9, 0x05};
    uint32_t address = (uint32_t)(uintptr_t)&fov_range_max;
    memcpy(max_load + 2, &address, sizeof(address));
    static const unsigned char fldz[] = {0xd9, 0xee};
    const struct code_patch patches[] = {
        {FOV_MAX_LOAD, fov_max_load_original, sizeof(fov_max_load_original), 0, max_load, sizeof(max_load)},
        {FOV_MIN_LOAD, fov_min_load_original, sizeof(fov_min_load_original), 0, fldz, sizeof(fldz)},
    };
    if (!apply_code_patches("FOV range", patches, sizeof(patches) / sizeof(patches[0]))) return 0;
    fprintf(stderr, "[t2_hook] setFov() accepts 0-360 degrees (memory only)\n");
    return 1;
}

/* GameBase refuses getDataBlock() and setDataBlock() on client (ghost)
   objects with an error, and setTargetId() returns silently; let all three
   act on client objects, as the Windows l33thax patch did. Each check tests
   the client-object flag (byte +0x40, bit 2). */
#define CLIENT_GET_DATABLOCK_CHECK 0x0825C995  /* je past the error */
#define CLIENT_SET_DATABLOCK_CHECK 0x0825BB17  /* je past the error */
#define CLIENT_SET_TARGET_CHECK    0x0825BBE2  /* jne to the return */
static const unsigned char client_get_datablock_original[] = {0x74, 0x11, 0x83, 0xc4, 0xf4};
static const unsigned char client_set_datablock_original[] = {0x74, 0x17, 0x83, 0xc4, 0xf4};
static const unsigned char client_set_target_original[] = {0x0f, 0x85, 0xdb, 0x00, 0x00, 0x00};

static int install_client_datablocks(void) {
    static const unsigned char jump[] = {0xeb};
    static const unsigned char nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const struct code_patch patches[] = {
        {CLIENT_GET_DATABLOCK_CHECK, client_get_datablock_original, sizeof(client_get_datablock_original),
         0, jump, sizeof(jump)},
        {CLIENT_SET_DATABLOCK_CHECK, client_set_datablock_original, sizeof(client_set_datablock_original),
         0, jump, sizeof(jump)},
        {CLIENT_SET_TARGET_CHECK, client_set_target_original, sizeof(client_set_target_original),
         0, nops, sizeof(nops)},
    };
    if (!apply_code_patches("Client datablock", patches, sizeof(patches) / sizeof(patches[0]))) return 0;
    fprintf(stderr, "[t2_hook] getDataBlock(), setDataBlock() and setTargetId() work on client objects "
            "(memory only)\n");
    return 1;
}

/* Both executables clamp $pref::Net::PacketRateToClient to 32 and
   $pref::Net::PacketSize to 450 in three places: the NetConnection
   constructor and two rate setters. In the client the clamped values become
   the rate and size it asks the server for; in the server they cap what it
   sends each client. A stock peer still holds the other end to 32 and 450,
   while QoL-patched TribesNext peers accept more. At each site the rate
   compare is followed by its store at +9, the size compare at +0x26 and its
   store at +0x32; each instruction names its variable's address. The rate
   compare takes a sign-extended imm8, so 127 is the largest possible limit. */
#define NET_PACKET_RATE_LIMIT 64
#define NET_PACKET_SIZE_LIMIT 1000
#define NET_RATE_STORE_OFFSET 0x09
#define NET_SIZE_COMPARE_OFFSET 0x26
#define NET_SIZE_STORE_OFFSET 0x32

/* An instruction's opcode bytes, its variable's address, then its operand. */
static void net_instruction(unsigned char *out, unsigned char op1, unsigned char op2, uintptr_t variable,
                            const unsigned char *operand, size_t operand_length) {
    uint32_t address = (uint32_t)variable;
    out[0] = op1;
    out[1] = op2;
    memcpy(out + 2, &address, sizeof(address));
    memcpy(out + 6, operand, operand_length);
}

static int install_net_packet_limits(void) {
    static const unsigned char rate8[] = {NET_PACKET_RATE_LIMIT};
    static const unsigned char rate32[] = {NET_PACKET_RATE_LIMIT, 0, 0, 0};
    static const unsigned char size32[] = {NET_PACKET_SIZE_LIMIT & 0xff, NET_PACKET_SIZE_LIMIT >> 8, 0, 0};
    static const unsigned char stock_rate8[] = {0x20}, stock_rate32[] = {0x20, 0, 0, 0};
    static const unsigned char stock_size32[] = {0xc2, 0x01, 0, 0};
    /* cmpl $32,rate; movl $32,rate; cmpl $450,size; movl $450,size */
    unsigned char rate_compare[7], rate_store[10], size_compare[10], size_store[10];
    net_instruction(rate_compare, 0x83, 0x3d, target->net_rate_variable, stock_rate8, 1);
    net_instruction(rate_store, 0xc7, 0x05, target->net_rate_variable, stock_rate32, 4);
    net_instruction(size_compare, 0x81, 0x3d, target->net_size_variable, stock_size32, 4);
    net_instruction(size_store, 0xc7, 0x05, target->net_size_variable, stock_size32, 4);
    struct code_patch patches[12];
    size_t count = 0;
    for (size_t i = 0; i < 3; i++) {
        uintptr_t site = target->net_rate_compare[i];
        patches[count++] = (struct code_patch){site, rate_compare, sizeof(rate_compare), 6,
            rate8, sizeof(rate8)};
        patches[count++] = (struct code_patch){site + NET_RATE_STORE_OFFSET, rate_store,
            sizeof(rate_store), 6, rate32, sizeof(rate32)};
        patches[count++] = (struct code_patch){site + NET_SIZE_COMPARE_OFFSET, size_compare,
            sizeof(size_compare), 6, size32, sizeof(size32)};
        patches[count++] = (struct code_patch){site + NET_SIZE_STORE_OFFSET, size_store,
            sizeof(size_store), 6, size32, sizeof(size32)};
    }
    if (!apply_code_patches("Network packet limit", patches, count)) return 0;
    fprintf(stderr, "[t2_hook] Network: %s packet limits %d/s and %d bytes instead of 32/s and 450 "
            "(memory only)\n", target->description, NET_PACKET_RATE_LIMIT, NET_PACKET_SIZE_LIMIT);
    return 1;
}

/* ── Native TribesNext handshake ──────────────────────────────────────────
 * The TribesNext QoL update authenticates accounts in the connection
 * handshake itself, which servers with $Host::RequireAuth insist on. Its
 * challenge request replaces the WON certificate flag with a 0 and appends a
 * native flag: 0x54, an 8-byte nonce, the server's IPv4 address as the
 * client sees it, a 12-bit length, and the account blob from
 * native_auth_blob(). The server answers in the WON flag's place of the
 * challenge response with a flag, a 9-bit length and a challenge encrypted to
 * the account key; the client decrypts it (raw RSA, as hook_tn_decrypt does),
 * finds its nonce, keeps the 64 bytes from there and sends bytes 12-51 back
 * in the connect request's flag slot. QoL servers read all of this at
 * protocol 51 too, and their protocol-52 game data would not parse here, so
 * the client keeps announcing 51. */
#define NATIVE_AUTH_CHALLENGE_FLAG_CALL 0x08236844
#define NATIVE_AUTH_REPLY_FLAG_READ     0x082376E2
#define NATIVE_AUTH_REPLY_FLAG_STORE    0x08237713
#define NATIVE_AUTH_CONNECT_FLAG_CALL   0x08237556
#define NATIVE_AUTH_SERVER_ADDRESS      0x08689418  /* the pending connection's NetAddress */
#define BITSTREAM_WRITE_FLAG            0x08102C04
#define BITSTREAM_READ_INT              0x08102E60
#define BITSTREAM_WRITE_INT             0x08102F1C
#define NATIVE_AUTH_MARKER              0x54
#define NATIVE_AUTH_SIGNATURE_BYTES     512
#define NATIVE_AUTH_BLOB_MIN            0x218  /* the server's accepted blob lengths */
#define NATIVE_AUTH_BLOB_MAX            0x400
/* Both calls go to writeFlag at 0x08102C04. */
static const unsigned char native_auth_challenge_call_original[] = {0xe8, 0xbb, 0xc3, 0xec, 0xff};
static const unsigned char native_auth_connect_call_original[] = {0xe8, 0xa9, 0xb6, 0xec, 0xff};
static const unsigned char native_auth_reply_read_original[] =
    {0x8b, 0x77, 0x0c, 0x3b, 0x77, 0x18, 0x7e, 0x08, 0xc6, 0x47, 0x14, 0x01, 0xb0, 0x00};

typedef char (*bitstream_write_flag_t)(void *stream, int flag);
typedef int (*bitstream_read_int_t)(void *stream, int bits);
typedef void (*bitstream_write_int_t)(void *stream, int value, int bits);
static bitstream_write_flag_t bitstream_write_flag = (bitstream_write_flag_t)BITSTREAM_WRITE_FLAG;
static bitstream_read_int_t bitstream_read_int = (bitstream_read_int_t)BITSTREAM_READ_INT;
static bitstream_write_int_t bitstream_write_int = (bitstream_write_int_t)BITSTREAM_WRITE_INT;

static struct {
    int sent;       /* the last challenge request carried the native flag */
    int verified;   /* its reply echoed our nonce; the connect request answers */
    unsigned char nonce[8];
    unsigned char block[64];
} native_auth;

static int run_rsa_helper(const char *base, const char *exp, const char *mod,
                          char *result, size_t result_size);
static int hex2nibble(char c);
static void bytes_to_hex(const uint8_t *data, int len, char *out);

/* Hex digits to big-endian bytes without leading zero bytes; -1 if invalid. */
static int native_auth_hex_bytes(const char *hex, size_t hex_length, unsigned char *out, size_t cap) {
    while (hex_length > 1 && hex[0] == '0') { hex++; hex_length--; }
    if (!hex_length) return -1;
    size_t count = (hex_length + 1) / 2;
    if (count > cap) return -1;
    memset(out, 0, count);
    for (size_t i = 0; i < hex_length; i++) {
        int nibble = hex2nibble(hex[i]);
        if (nibble < 0) return -1;
        size_t digit = hex_length - 1 - i;  /* from the least significant end */
        out[count - 1 - digit / 2] |= (unsigned char)(digit % 2 ? nibble << 4 : nibble);
    }
    return (int)count;
}

/* The account blob of the QoL client's certificate helper:
   "name\tguid\tlen(E)\tlen(N)+1\t", E, a zero byte and N (the zero ends the
   server's tab tokenizer before N), then the 512-byte certificate signature. */
static int native_auth_blob(const char *certificate, unsigned char *out, size_t cap) {
    const char *fields[5];
    size_t lengths[5];
    const char *field = certificate;
    for (int i = 0; i < 5; i++) {
        const char *tab = strchr(field, '\t');
        if ((i < 4) != (tab != NULL)) return -1;
        fields[i] = field;
        lengths[i] = tab ? (size_t)(tab - field) : strlen(field);
        if (!lengths[i]) return -1;
        field = tab ? tab + 1 : field;
    }
    char name[64], guid_text[16];
    if (lengths[0] >= sizeof(name) || lengths[1] >= sizeof(guid_text)) return -1;
    memcpy(name, fields[0], lengths[0]); name[lengths[0]] = 0;
    memcpy(guid_text, fields[1], lengths[1]); guid_text[lengths[1]] = 0;
    char *end;
    long guid = strtol(guid_text, &end, 10);
    if (*end || guid <= 0 || guid > 0x7fffffffL) return -1;
    unsigned char exponent[512], modulus[512], signature[NATIVE_AUTH_SIGNATURE_BYTES];
    int exponent_length = native_auth_hex_bytes(fields[2], lengths[2], exponent, sizeof(exponent));
    int modulus_length = native_auth_hex_bytes(fields[3], lengths[3], modulus, sizeof(modulus));
    int signature_length = native_auth_hex_bytes(fields[4], lengths[4], signature, sizeof(signature));
    if (exponent_length <= 0 || modulus_length <= 0 || signature_length <= 0) return -1;
    memmove(signature + sizeof(signature) - signature_length, signature, (size_t)signature_length);
    memset(signature, 0, sizeof(signature) - signature_length);
    int header = snprintf((char *)out, cap, "%s\t%ld\t%d\t%d\t", name, guid,
                          exponent_length, modulus_length + 1);
    if (header < 0) return -1;
    size_t length = (size_t)header + exponent_length + 1 + modulus_length + sizeof(signature);
    if (length > cap || length < NATIVE_AUTH_BLOB_MIN || length > NATIVE_AUTH_BLOB_MAX) return -1;
    unsigned char *cursor = out + header;
    memcpy(cursor, exponent, exponent_length); cursor += exponent_length;
    *cursor++ = 0;
    memcpy(cursor, modulus, modulus_length); cursor += modulus_length;
    memcpy(cursor, signature, sizeof(signature));
    return (int)length;
}

/* The QoL client's search: the first nonzero position holding the nonce with
   64 bytes left, which it keeps. */
static int native_auth_find_block(const unsigned char *plain, int length,
                                  const unsigned char nonce[8], unsigned char block[64]) {
    for (int i = 0; i < length; i++) {
        if (plain[i] == 0) continue;
        if (i + 64 > length) return 0;
        if (memcmp(plain + i, nonce, 8) == 0) {
            memcpy(block, plain + i, 64);
            return 1;
        }
    }
    return 0;
}

static int native_auth_ready(void) {
    return tribesnext_certificate[0] && tribesnext_private_exponent[0] && tribesnext_modulus[0];
}

/* Replaces the challenge request's writeFlag(WON certificate) call. */
static char native_auth_challenge_flag(void *stream, int won_flag) {
    unsigned char blob[NATIVE_AUTH_BLOB_MAX];
    const unsigned char *address = (const unsigned char *)NATIVE_AUTH_SERVER_ADDRESS;
    memset(&native_auth, 0, sizeof(native_auth));
    int length = native_auth_ready() ? native_auth_blob(tribesnext_certificate, blob, sizeof(blob)) : -1;
    FILE *random = length > 0 ? fopen("/dev/urandom", "rb") : NULL;
    int have_nonce = random && fread(native_auth.nonce, 1, sizeof(native_auth.nonce), random) ==
                     sizeof(native_auth.nonce);
    if (random) fclose(random);
    if (length <= 0 || !have_nonce || *(const int32_t *)address != 1 /* IP address */) {
        fprintf(stderr, "[t2_hook] Native TribesNext challenge not sent: %s\n",
                !native_auth_ready() ? "the account is not unlocked" :
                length <= 0 ? "the certificate is not usable" : "no nonce or IP address");
        return bitstream_write_flag(stream, won_flag & 0xff);
    }
    bitstream_write_flag(stream, 0);
    bitstream_write_flag(stream, 1);
    bitstream_write_int(stream, NATIVE_AUTH_MARKER, 8);
    for (size_t i = 0; i < sizeof(native_auth.nonce); i++)
        bitstream_write_int(stream, native_auth.nonce[i], 8);
    for (int i = 4; i < 8; i++) bitstream_write_int(stream, address[i], 8);
    bitstream_write_int(stream, length, 12);
    for (int i = 0; i < length; i++) bitstream_write_int(stream, blob[i], 8);
    native_auth.sent = 1;
    fprintf(stderr, "[t2_hook] Native TribesNext challenge sent (%d-byte account blob)\n", length);
    return 0;
}

/* Replaces the challenge response's inline readFlag; the stock code stores
   and tests the result as the WON certificate flag. A native challenge is
   answered here and reported as 0, so the stock accept path runs. */
static int native_auth_reply_flag(void *stream) {
    if (!bitstream_read_int(stream, 1)) return 0;
    if (!native_auth.sent) return 1;
    native_auth.sent = 0;
    int length = bitstream_read_int(stream, 9) & 0x1ff;
    unsigned char cipher[512], plain[512];
    for (int i = 0; i < length; i++) cipher[i] = (unsigned char)bitstream_read_int(stream, 8);
    int key_bytes = (int)(strlen(tribesnext_modulus) + 1) / 2;
    char cipher_hex[1025], plain_hex[2048];
    if (length < 64 || length > key_bytes || key_bytes > (int)sizeof(plain)) {
        fprintf(stderr, "[t2_hook] Native TribesNext challenge refused: %d bytes\n", length);
        return 0;
    }
    bytes_to_hex(cipher, length, cipher_hex);
    if (!run_rsa_helper(cipher_hex, tribesnext_private_exponent, tribesnext_modulus,
                        plain_hex, sizeof(plain_hex))) {
        fprintf(stderr, "[t2_hook] Native TribesNext challenge: RSA operation failed\n");
        return 0;
    }
    memset(plain, 0, sizeof(plain));
    int plain_length = native_auth_hex_bytes(plain_hex, strlen(plain_hex), plain, sizeof(plain));
    if (plain_length <= 0 || plain_length > key_bytes) return 0;
    memmove(plain + key_bytes - plain_length, plain, (size_t)plain_length);
    memset(plain, 0, (size_t)(key_bytes - plain_length));
    native_auth.verified = native_auth_find_block(plain, key_bytes, native_auth.nonce, native_auth.block);
    fprintf(stderr, "[t2_hook] Native TribesNext challenge %s\n",
            native_auth.verified ? "answered" : "did not contain our nonce");
    return 0;
}

/* Replaces the connect request's writeFlag(WON certificate) call. */
static char native_auth_connect_flag(void *stream, int won_flag) {
    if (!native_auth.verified) return bitstream_write_flag(stream, won_flag & 0xff);
    bitstream_write_flag(stream, 1);
    for (int i = 12; i < 52; i++) bitstream_write_int(stream, native_auth.block[i], 8);
    memset(&native_auth, 0, sizeof(native_auth));
    return 0;
}

static void native_auth_call(unsigned char call[5], uintptr_t site, const void *target) {
    int32_t offset = (int32_t)((uintptr_t)target - (site + 5));
    call[0] = 0xe8;
    memcpy(call + 1, &offset, sizeof(offset));
}

static int install_native_auth(void) {
    unsigned char challenge_call[5], connect_call[5], reply_read[14];
    native_auth_call(challenge_call, NATIVE_AUTH_CHALLENGE_FLAG_CALL, (const void *)native_auth_challenge_flag);
    native_auth_call(connect_call, NATIVE_AUTH_CONNECT_FLAG_CALL, (const void *)native_auth_connect_flag);
    /* push %edi (the stream); call native_auth_reply_flag; add $4,%esp; jmp to the store */
    reply_read[0] = 0x57;
    native_auth_call(reply_read + 1, NATIVE_AUTH_REPLY_FLAG_READ + 1, (const void *)native_auth_reply_flag);
    reply_read[6] = 0x83; reply_read[7] = 0xc4; reply_read[8] = 0x04;
    reply_read[9] = 0xe9;
    int32_t store = (int32_t)(NATIVE_AUTH_REPLY_FLAG_STORE - (NATIVE_AUTH_REPLY_FLAG_READ + 14));
    memcpy(reply_read + 10, &store, sizeof(store));
    const struct code_patch patches[] = {
        {NATIVE_AUTH_CHALLENGE_FLAG_CALL, native_auth_challenge_call_original,
         sizeof(native_auth_challenge_call_original), 0, challenge_call, sizeof(challenge_call)},
        {NATIVE_AUTH_REPLY_FLAG_READ, native_auth_reply_read_original,
         sizeof(native_auth_reply_read_original), 0, reply_read, sizeof(reply_read)},
        {NATIVE_AUTH_CONNECT_FLAG_CALL, native_auth_connect_call_original,
         sizeof(native_auth_connect_call_original), 0, connect_call, sizeof(connect_call)},
    };
    if (!apply_code_patches("Native TribesNext handshake", patches, sizeof(patches) / sizeof(patches[0])))
        return 0;
    fprintf(stderr, "[t2_hook] Native TribesNext handshake enabled at protocol 51 (memory only)\n");
    return 1;
}

static int install_won_verifier(void) {
    const char *path = getenv("T2LAUNCH_WON_VERIFIER");
    if (!path || !*path) {
        fprintf(stderr, "[t2_hook] WON verifier file was not provided\n");
        return 0;
    }
    unsigned char encoded[817];
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[t2_hook] Cannot open WON verifier: %s\n", strerror(errno));
        return 0;
    }
    size_t count = fread(encoded, 1, sizeof(encoded), file);
    int extra = fgetc(file);
    fclose(file);
    int protection;
    /* Key slot consumed by the WON global constructor (client 0x08374BE3,
       server 0x08368B43). */
    uintptr_t slot = target->won_verifier;
    if (count != sizeof(encoded) || extra != EOF || encoded[0] != 0x30 ||
        encoded[1] != 0x82 || !readable_range(slot, sizeof(encoded), &protection) ||
        memcmp((void *)slot, "\x30\x82\x03\x2c", 4) != 0) {
        fprintf(stderr, "[t2_hook] WON verifier refused: key/slot format mismatch\n");
        return 0;
    }
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return 0;
    uintptr_t page = slot & ~((uintptr_t)size - 1);
    if (slot + sizeof(encoded) > page + (uintptr_t)size ||
        mprotect((void *)page, (size_t)size, protection | PROT_WRITE) != 0)
        return 0;
    memcpy((void *)slot, encoded, sizeof(encoded));
    if (mprotect((void *)page, (size_t)size, protection) != 0) return 0;
    fprintf(stderr, "[t2_hook] WON: installed local verifier key (signature checks remain active)\n");
    return 1;
}

/* Pick the target from the executable's basename; 0 for any other process. */
static int is_game_process(void) {
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) return 0;
    executable[length] = 0;
    const char *name = strrchr(executable, '/');
    if (!name) return 0;
    for (size_t i = 0; i < GAME_TARGET_COUNT; i++) {
        if (strcmp(name + 1, game_targets[i].executable) == 0) {
            target = &game_targets[i];
            return 1;
        }
    }
    return 0;
}

static int is_supported_game(void) {
    int protection;
    /* Guard the console addresses as well as the executable basename. */
    return readable_range(target->con_add_method, 3, &protection) &&
        (protection & PROT_EXEC) && memcmp((void *)target->con_add_method, "\x55\x89\xe5", 3) == 0 &&
        readable_range(target->con_evaluate, 3, &protection) &&
        (protection & PROT_EXEC) && memcmp((void *)target->con_evaluate, "\x55\x89\xe5", 3) == 0 &&
        readable_range(target->string_table, sizeof(uint32_t), &protection);
}

/* Files the hook reads live beside the game. The launcher sets
   T2LAUNCH_GAME_DIR; otherwise the working directory captured at startup is
   used. */
static const char *game_path(char *buffer, size_t size, const char *relative) {
    static char game_dir[4096];
    if (!game_dir[0]) {
        const char *configured = getenv("T2LAUNCH_GAME_DIR");
        if (configured && configured[0] && strlen(configured) < sizeof(game_dir))
            snprintf(game_dir, sizeof(game_dir), "%s", configured);
        else if (!getcwd(game_dir, sizeof(game_dir)))
            snprintf(game_dir, sizeof(game_dir), ".");
    }
    if ((size_t)snprintf(buffer, size, "%s/%s", game_dir, relative) >= size) buffer[0] = 0;
    return buffer;
}

/* In opt-in connection-trace mode, record resolver hostnames without logging
   packet contents or credentials. The launcher sets T2LAUNCH_TRACE_DIR. */
static int is_won_bootstrap_host(const char *node);

static void trace_resolver_name(const char *node, const char *service) {
    const char *enabled = getenv("TRACE_CONNECTS");
    if (!enabled || strcmp(enabled, "1") != 0 || !node || !*node) return;
    const char *trace_dir = getenv("T2LAUNCH_TRACE_DIR");
    if (!trace_dir || !trace_dir[0]) return;
    char path[4096];
    snprintf(path, sizeof(path), "%s/tribes2-dns.log", trace_dir);
    FILE *file = fopen(path, "a");
    if (!file) return;
    fprintf(file, "%s%s%s -> %s\n", node, service ? ":" : "",
            service ? service : "",
            is_won_bootstrap_host(node) ? "127.0.0.1" : node);
    fclose(file);
}

static int is_won_bootstrap_host(const char *node) {
    static const char *hosts[] = {
        "tribes2.m1.sierra.com", "tribes2.m2.sierra.com",
        "tribes2.m3.sierra.com"
    };
    const char *won = getenv("T2LAUNCH_WONEMU");
    if (!won || strcmp(won, "1") != 0 || !node) return 0;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); ++i)
        if (strcasecmp(node, hosts[i]) == 0) return 1;
    return 0;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **results) {
    typedef int (*getaddrinfo_fn)(const char *, const char *,
                                  const struct addrinfo *, struct addrinfo **);
    static getaddrinfo_fn real_getaddrinfo;
    if (!real_getaddrinfo)
        real_getaddrinfo = (getaddrinfo_fn)next_symbol("getaddrinfo");
    trace_resolver_name(node, service);
    if (!real_getaddrinfo) return EAI_SYSTEM;
    if (is_won_bootstrap_host(node)) node = "127.0.0.1";
    return real_getaddrinfo(node, service, hints, results);
}

struct hostent *gethostbyname(const char *name) {
    typedef struct hostent *(*gethostbyname_fn)(const char *);
    static gethostbyname_fn real_gethostbyname;
    if (!real_gethostbyname)
        real_gethostbyname = (gethostbyname_fn)next_symbol("gethostbyname");
    trace_resolver_name(name, NULL);
    if (!real_gethostbyname) return NULL;
    if (is_won_bootstrap_host(name)) name = "127.0.0.1";
    return real_gethostbyname(name);
}

/* The client joins threads it created detached when it shuts down. LinuxThreads
   answered EINVAL; NPTL dereferences the handle of a thread that has already
   exited and freed its descriptor, which crashes on modern glibc. Record the
   joinable threads and refuse joins on anything else, as LinuxThreads did. */
#define JOINABLE_THREAD_LIMIT 256
static pthread_mutex_t joinable_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t joinable_threads[JOINABLE_THREAD_LIMIT];
static int joinable_count;
static int joinable_overflow;

static int take_joinable_thread(pthread_t thread) {
    int found = 0;
    pthread_mutex_lock(&joinable_lock);
    for (int i = 0; i < joinable_count; i++) {
        if (pthread_equal(joinable_threads[i], thread)) {
            joinable_threads[i] = joinable_threads[--joinable_count];
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&joinable_lock);
    return found;
}

typedef int (*pthread_create_t)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
typedef int (*pthread_join_t)(pthread_t, void **);
typedef int (*pthread_detach_t)(pthread_t);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    static pthread_create_t real_create;
    if (!real_create) {
        /* The client imports pthread_create@GLIBC_2.1. */
        real_create = (pthread_create_t)dlvsym(RTLD_NEXT, "pthread_create", "GLIBC_2.1");
        if (!real_create) real_create = (pthread_create_t)next_symbol("pthread_create");
        if (!real_create) return EAGAIN;
    }
    int result = real_create(thread, attr, start, arg);
    int detached = 0;
    if (attr && pthread_attr_getdetachstate(attr, &detached) != 0) detached = 0;
    if (result == 0 && detached != PTHREAD_CREATE_DETACHED) {
        pthread_mutex_lock(&joinable_lock);
        if (joinable_count < JOINABLE_THREAD_LIMIT) joinable_threads[joinable_count++] = *thread;
        else joinable_overflow = 1;
        pthread_mutex_unlock(&joinable_lock);
    }
    return result;
}

int pthread_join(pthread_t thread, void **value) {
    static pthread_join_t real_join;
    if (!real_join) real_join = (pthread_join_t)next_symbol("pthread_join");
    if (!real_join) return ESRCH;
    /* Past the table limit untracked threads may be joinable, so trust the caller. */
    if (!take_joinable_thread(thread) && !joinable_overflow &&
        hook_option_bool("runtime.thread_join_guard")) {
        fprintf(stderr, "[t2_hook] Refused pthread_join on a detached or unknown thread\n");
        return EINVAL;
    }
    return real_join(thread, value);
}

int pthread_detach(pthread_t thread) {
    static pthread_detach_t real_detach;
    if (!real_detach) real_detach = (pthread_detach_t)next_symbol("pthread_detach");
    if (!real_detach) return ESRCH;
    take_joinable_thread(thread);
    return real_detach(thread);
}

/* ── Gamma (video.gamma) ─────────────────────────────────────────────────
 * The Video options gamma slider (0 to 1) reaches SDL_SetGamma as gamma 0.48
 * to 1.48 (0x081D5F98), which sets X11 gamma ramps; XWayland and most
 * compositors ignore them. The
 * hook records the value instead and, before each swap, copies the back
 * buffer into a texture and draws it back through a shader computing
 * pow(colour, 1/gamma), the curve SDL's ramps used. */
#define GL_TEXTURE_2D_ 0x0DE1
#define GL_RGB_ 0x1907
#define GL_UNSIGNED_BYTE_ 0x1401
#define GL_TEXTURE_MIN_FILTER_ 0x2801
#define GL_TEXTURE_MAG_FILTER_ 0x2800
#define GL_NEAREST_ 0x2600
#define GL_ALL_ATTRIB_BITS_ 0x000FFFFF
#define GL_CLIENT_ALL_ATTRIB_BITS_ 0xFFFFFFFF
#define GL_PROJECTION_ 0x1701
#define GL_MODELVIEW_ 0x1700
#define GL_TEXTURE_ 0x1702
#define GL_QUADS_ 0x0007
#define GL_BACK_ 0x0405
#define GL_TEXTURE0_ 0x84C0
#define GL_ACTIVE_TEXTURE_ 0x84E0
#define GL_CURRENT_PROGRAM_ 0x8B8D
#define GL_FRAGMENT_SHADER_ 0x8B30
#define GL_VERTEX_SHADER_ 0x8B31
#define GL_COMPILE_STATUS_ 0x8B81
#define GL_LINK_STATUS_ 0x8B82
#define GL_DEPTH_TEST_ 0x0B71
#define GL_BLEND_ 0x0BE2
#define GL_ALPHA_TEST_ 0x0BC0
#define GL_CULL_FACE_ 0x0B44
#define GL_SCISSOR_TEST_ 0x0C11
#define GL_STENCIL_TEST_ 0x0B90
#define GL_FOG_ 0x0B60
#define GL_LIGHTING_ 0x0B50
#define GL_LINEAR_ 0x2601
#define GL_TEXTURE_WRAP_S_ 0x2802
#define GL_TEXTURE_WRAP_T_ 0x2803
#define GL_CLAMP_TO_EDGE_ 0x812F
#define GL_TEXTURE_BINDING_2D_ 0x8069
#define GL_RGBA_ 0x1908
#define GL_RGBA8_ 0x8058
#define GL_COLOR_BUFFER_BIT_ 0x4000
#define GL_SCISSOR_BIT_ 0x00080000
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_READ_FRAMEBUFFER_ 0x8CA8
#define GL_DRAW_FRAMEBUFFER_ 0x8CA9
#define GL_RENDERBUFFER_ 0x8D41
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT_ 0x821A
#define GL_DEPTH24_STENCIL8_ 0x88F0
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5
#define GL_MAX_SAMPLES_ 0x8D57

static volatile float gamma_requested = 1.0f;
static int gamma_width, gamma_height;  /* the real window */

/* Post pass: one shader draw before each swap. It applies the gamma slider
   (video.gamma) and sharpening (video.sharpen), and with a render target
   (video.render_scale / video.msaa) it also scales the game's frame into the
   window. With neither gamma nor sharpening it does nothing. */
static struct {
    int state;  /* 0 unresolved, 1 ready, -1 unavailable */
    void (*GetIntegerv)(unsigned, int *);
    void (*GenTextures)(int, unsigned *);
    void (*DeleteTextures)(int, const unsigned *);
    void (*BindTexture)(unsigned, unsigned);
    void (*TexParameteri)(unsigned, unsigned, int);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*CopyTexSubImage2D)(unsigned, int, int, int, int, int, int, int);
    void (*ReadBuffer)(unsigned);
    void (*PushAttrib)(unsigned);
    void (*PopAttrib)(void);
    void (*PushClientAttrib)(unsigned);
    void (*PopClientAttrib)(void);
    void (*MatrixMode)(unsigned);
    void (*PushMatrix)(void);
    void (*PopMatrix)(void);
    void (*LoadIdentity)(void);
    void (*Disable)(unsigned);
    void (*Enable)(unsigned);
    void (*Viewport)(int, int, int, int);
    void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*Begin)(unsigned);
    void (*End)(void);
    void (*TexCoord2f)(float, float);
    void (*Vertex2f)(float, float);
    void (*ActiveTexture)(unsigned);
    unsigned (*CreateShader)(unsigned);
    void (*ShaderSource)(unsigned, int, const char *const *, const int *);
    void (*CompileShader)(unsigned);
    void (*GetShaderiv)(unsigned, unsigned, int *);
    unsigned (*CreateProgram)(void);
    void (*AttachShader)(unsigned, unsigned);
    void (*LinkProgram)(unsigned);
    void (*GetProgramiv)(unsigned, unsigned, int *);
    void (*UseProgram)(unsigned);
    int (*GetUniformLocation)(unsigned, const char *);
    void (*Uniform1i)(int, int);
    void (*Uniform1f)(int, float);
    void (*Uniform2f)(int, float, float);
    unsigned program, texture;
    int texture_width, texture_height, exponent_location, sharpness_location, texel_location;
} gl_gamma;

static const char gamma_vertex_shader[] =
    "void main() { gl_TexCoord[0] = gl_MultiTexCoord0; gl_Position = gl_Vertex; }\n";
/* Sharpening follows AMD's contrast-adaptive sharpening: a cross of four
   neighbours, weighted down where local contrast is already high. */
static const char gamma_fragment_shader[] =
    "uniform sampler2D frame; uniform float exponent; uniform float sharpness; uniform vec2 texel;\n"
    "void main() {\n"
    " vec2 uv = gl_TexCoord[0].xy;\n"
    " vec3 c = texture2D(frame, uv).rgb;\n"
    " if (sharpness > 0.0) {\n"
    "  vec3 n = texture2D(frame, uv + vec2(0.0, texel.y)).rgb;\n"
    "  vec3 s = texture2D(frame, uv - vec2(0.0, texel.y)).rgb;\n"
    "  vec3 e = texture2D(frame, uv + vec2(texel.x, 0.0)).rgb;\n"
    "  vec3 w = texture2D(frame, uv - vec2(texel.x, 0.0)).rgb;\n"
    "  vec3 lo = min(c, min(min(n, s), min(e, w)));\n"
    "  vec3 hi = max(c, max(max(n, s), max(e, w)));\n"
    "  vec3 amount = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(0.0001)), 0.0, 1.0));\n"
    "  vec3 weight = amount * (-1.0 / mix(8.0, 5.0, sharpness));\n"
    "  c = clamp((c + (n + s + e + w) * weight) / (1.0 + 4.0 * weight), 0.0, 1.0);\n"
    " }\n"
    " gl_FragColor = vec4(pow(c, vec3(exponent)), 1.0);\n"
    "}\n";

static int resolve_gl_gamma(void) {
    if (gl_gamma.state) return gl_gamma.state > 0;
    gl_gamma.state = -1;
    void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    void *scope = gl ? gl : RTLD_DEFAULT;
    void *(*get_proc)(const unsigned char *) =
        (void *(*)(const unsigned char *))dlsym(scope, "glXGetProcAddressARB");
#define GL_FN(field, name) \
    do { \
        *(void **)&gl_gamma.field = get_proc ? get_proc((const unsigned char *)name) : NULL; \
        if (!gl_gamma.field) *(void **)&gl_gamma.field = dlsym(scope, name); \
        if (!gl_gamma.field) { \
            fprintf(stderr, "[t2_hook] Post pass unavailable: no %s\n", name); \
            return 0; \
        } \
    } while (0)
    GL_FN(GetIntegerv, "glGetIntegerv"); GL_FN(GenTextures, "glGenTextures");
    GL_FN(DeleteTextures, "glDeleteTextures"); GL_FN(BindTexture, "glBindTexture");
    GL_FN(TexParameteri, "glTexParameteri"); GL_FN(TexImage2D, "glTexImage2D");
    GL_FN(CopyTexSubImage2D, "glCopyTexSubImage2D"); GL_FN(ReadBuffer, "glReadBuffer");
    GL_FN(PushAttrib, "glPushAttrib"); GL_FN(PopAttrib, "glPopAttrib");
    GL_FN(PushClientAttrib, "glPushClientAttrib"); GL_FN(PopClientAttrib, "glPopClientAttrib");
    GL_FN(MatrixMode, "glMatrixMode"); GL_FN(PushMatrix, "glPushMatrix");
    GL_FN(PopMatrix, "glPopMatrix"); GL_FN(LoadIdentity, "glLoadIdentity");
    GL_FN(Disable, "glDisable"); GL_FN(Enable, "glEnable"); GL_FN(Viewport, "glViewport");
    GL_FN(ColorMask, "glColorMask"); GL_FN(Begin, "glBegin"); GL_FN(End, "glEnd");
    GL_FN(TexCoord2f, "glTexCoord2f"); GL_FN(Vertex2f, "glVertex2f");
    GL_FN(ActiveTexture, "glActiveTexture"); GL_FN(CreateShader, "glCreateShader");
    GL_FN(ShaderSource, "glShaderSource"); GL_FN(CompileShader, "glCompileShader");
    GL_FN(GetShaderiv, "glGetShaderiv"); GL_FN(CreateProgram, "glCreateProgram");
    GL_FN(AttachShader, "glAttachShader"); GL_FN(LinkProgram, "glLinkProgram");
    GL_FN(GetProgramiv, "glGetProgramiv"); GL_FN(UseProgram, "glUseProgram");
    GL_FN(GetUniformLocation, "glGetUniformLocation"); GL_FN(Uniform1i, "glUniform1i");
    GL_FN(Uniform1f, "glUniform1f"); GL_FN(Uniform2f, "glUniform2f");
#undef GL_FN
    const char *sources[2] = {gamma_vertex_shader, gamma_fragment_shader};
    unsigned types[2] = {GL_VERTEX_SHADER_, GL_FRAGMENT_SHADER_};
    unsigned program = gl_gamma.CreateProgram();
    for (int i = 0; i < 2; i++) {
        unsigned shader = gl_gamma.CreateShader(types[i]);
        int ok = 0;
        gl_gamma.ShaderSource(shader, 1, &sources[i], NULL);
        gl_gamma.CompileShader(shader);
        gl_gamma.GetShaderiv(shader, GL_COMPILE_STATUS_, &ok);
        if (!ok) {
            fprintf(stderr, "[t2_hook] Post pass unavailable: shader did not compile\n");
            return 0;
        }
        gl_gamma.AttachShader(program, shader);
    }
    int linked = 0;
    gl_gamma.LinkProgram(program);
    gl_gamma.GetProgramiv(program, GL_LINK_STATUS_, &linked);
    if (!linked) {
        fprintf(stderr, "[t2_hook] Post pass unavailable: shader did not link\n");
        return 0;
    }
    gl_gamma.program = program;
    gl_gamma.exponent_location = gl_gamma.GetUniformLocation(program, "exponent");
    gl_gamma.sharpness_location = gl_gamma.GetUniformLocation(program, "sharpness");
    gl_gamma.texel_location = gl_gamma.GetUniformLocation(program, "texel");
    int previous = 0;
    gl_gamma.GetIntegerv(GL_CURRENT_PROGRAM_, &previous);
    gl_gamma.UseProgram(program);
    gl_gamma.Uniform1i(gl_gamma.GetUniformLocation(program, "frame"), 0);
    gl_gamma.UseProgram((unsigned)previous);
    gl_gamma.state = 1;
    fprintf(stderr, "[t2_hook] Gamma correction drawn by a shader pass\n");
    return 1;
}

/* video.sharpen: 0 (off) to 1 (strongest). */
static float post_sharpness(void) {
    static float sharpness = -1.0f;
    if (sharpness < 0.0f) {
        const char *value = hook_option_str("video.sharpen");
        sharpness = value ? (float)atof(value) : 0.0f;
        if (!(sharpness > 0.0f)) sharpness = 0.0f;
        if (sharpness > 1.0f) sharpness = 1.0f;
        if (sharpness > 0.0f) fprintf(stderr, "[t2_hook] Sharpening %.2f\n", (double)sharpness);
    }
    return sharpness;
}

/* The gamma exponent to apply, or 0 when neither gamma nor sharpening needs
   the shader. */
static float post_exponent(void) {
    float gamma = gamma_requested;
    int flat = gamma > 0.999f && gamma < 1.001f;
    if (flat && post_sharpness() <= 0.0f) return 0.0f;
    return flat ? 1.0f : 1.0f / gamma;
}

static int post_previous_program, post_previous_unit;

/* Save the game's state and set up a plain textured draw. */
static void post_begin(void) {
    gl_gamma.GetIntegerv(GL_CURRENT_PROGRAM_, &post_previous_program);
    gl_gamma.GetIntegerv(GL_ACTIVE_TEXTURE_, &post_previous_unit);
    gl_gamma.PushAttrib(GL_ALL_ATTRIB_BITS_);
    gl_gamma.PushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS_);
    gl_gamma.ActiveTexture(GL_TEXTURE0_);
    gl_gamma.MatrixMode(GL_TEXTURE_); gl_gamma.PushMatrix(); gl_gamma.LoadIdentity();
    gl_gamma.MatrixMode(GL_PROJECTION_); gl_gamma.PushMatrix(); gl_gamma.LoadIdentity();
    gl_gamma.MatrixMode(GL_MODELVIEW_); gl_gamma.PushMatrix(); gl_gamma.LoadIdentity();
    unsigned disabled[] = {GL_DEPTH_TEST_, GL_BLEND_, GL_ALPHA_TEST_, GL_CULL_FACE_, GL_SCISSOR_TEST_,
                           GL_STENCIL_TEST_, GL_FOG_, GL_LIGHTING_};
    for (size_t i = 0; i < sizeof(disabled) / sizeof(disabled[0]); i++) gl_gamma.Disable(disabled[i]);
    gl_gamma.ColorMask(1, 1, 1, 1);
    gl_gamma.Enable(GL_TEXTURE_2D_);
}

/* Draw texture (src_w x src_h) over the current viewport through the shader. */
static void post_quad(unsigned texture, int src_w, int src_h, float exponent, float sharpness) {
    gl_gamma.BindTexture(GL_TEXTURE_2D_, texture);
    gl_gamma.UseProgram(gl_gamma.program);
    gl_gamma.Uniform1f(gl_gamma.exponent_location, exponent);
    gl_gamma.Uniform1f(gl_gamma.sharpness_location, sharpness);
    gl_gamma.Uniform2f(gl_gamma.texel_location, 1.0f / (float)src_w, 1.0f / (float)src_h);
    gl_gamma.Begin(GL_QUADS_);
    gl_gamma.TexCoord2f(0, 0); gl_gamma.Vertex2f(-1, -1);
    gl_gamma.TexCoord2f(1, 0); gl_gamma.Vertex2f(1, -1);
    gl_gamma.TexCoord2f(1, 1); gl_gamma.Vertex2f(1, 1);
    gl_gamma.TexCoord2f(0, 1); gl_gamma.Vertex2f(-1, 1);
    gl_gamma.End();
}

static void post_end(void) {
    gl_gamma.UseProgram((unsigned)post_previous_program);
    gl_gamma.MatrixMode(GL_TEXTURE_); gl_gamma.PopMatrix();
    gl_gamma.MatrixMode(GL_PROJECTION_); gl_gamma.PopMatrix();
    gl_gamma.MatrixMode(GL_MODELVIEW_); gl_gamma.PopMatrix();
    gl_gamma.PopClientAttrib();
    gl_gamma.PopAttrib();
    gl_gamma.ActiveTexture((unsigned)post_previous_unit);
}

/* Without a render target: copy the back buffer and draw it back through the
   shader. Called before each swap on the game thread with its context current. */
static void apply_gamma_pass(void) {
    float exponent = post_exponent();
    if (exponent <= 0.0f) return;
    if (gamma_width <= 0 || gamma_height <= 0 || !resolve_gl_gamma()) return;
    post_begin();
    gl_gamma.Viewport(0, 0, gamma_width, gamma_height);
    if (!gl_gamma.texture) gl_gamma.GenTextures(1, &gl_gamma.texture);
    gl_gamma.BindTexture(GL_TEXTURE_2D_, gl_gamma.texture);
    if (gl_gamma.texture_width != gamma_width || gl_gamma.texture_height != gamma_height) {
        gl_gamma.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
        gl_gamma.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
        gl_gamma.TexImage2D(GL_TEXTURE_2D_, 0, GL_RGB_, gamma_width, gamma_height, 0, GL_RGB_,
                            GL_UNSIGNED_BYTE_, NULL);
        gl_gamma.texture_width = gamma_width;
        gl_gamma.texture_height = gamma_height;
    }
    gl_gamma.ReadBuffer(GL_BACK_);
    gl_gamma.CopyTexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, 0, 0, gamma_width, gamma_height);
    post_quad(gl_gamma.texture, gamma_width, gamma_height, exponent, post_sharpness());
    post_end();
}

/* ── Render target (video.render_scale, video.msaa) ──────────────────────
 * The game never uses framebuffer objects, so it always draws to framebuffer
 * 0. With a render target the hook binds its own FBO in that place, at the
 * resolution the game asked for, and draws it into the real window at swap:
 * - video.render_scale: a fullscreen below the monitor's native size opens
 *   at the native size (which fills the panel, even on Wayland where a mode
 *   switch is impossible) while the game keeps rendering at its own
 *   resolution, scaled up to fit (keeping the aspect ratio) or stretched;
 * - video.msaa: the FBO is multisampled and resolved at swap (window-system
 *   MSAA would break the copy-based gamma pass and the game's glReadPixels).
 * The game keeps its own size (it stores the size it asked SDL for), so only
 * glDrawBuffer/glReadBuffer, reads from a multisampled target and absolute
 * mouse positions need translating. */
struct render_rect { int x, y, w, h; };  /* GL window coordinates, origin bottom-left */

/* Where a game_w x game_h frame goes in a real_w x real_h window: centred
   and as large as fits without changing the aspect ratio, or stretched. */
static struct render_rect render_fit_rect(int game_w, int game_h, int real_w, int real_h, int stretch) {
    struct render_rect rect = {0, 0, real_w, real_h};
    if (stretch || game_w <= 0 || game_h <= 0) return rect;
    if ((long long)game_w * real_h > (long long)game_h * real_w) {
        rect.h = (int)((long long)game_h * real_w / game_w);  /* width-limited */
        rect.y = (real_h - rect.h) / 2;
    } else {
        rect.w = (int)((long long)game_w * real_h / game_h);  /* height-limited */
        rect.x = (real_w - rect.w) / 2;
    }
    return rect;
}

/* An SDL mouse position in the real window (origin top-left) to the game's. */
static void map_mouse_point(int *x, int *y, struct render_rect rect, int real_h, int game_w, int game_h) {
    if (rect.w <= 0 || rect.h <= 0) return;
    int top = real_h - rect.y - rect.h;
    int gx = (int)((long long)(*x - rect.x) * game_w / rect.w);
    int gy = (int)((long long)(*y - top) * game_h / rect.h);
    *x = gx < 0 ? 0 : gx >= game_w ? game_w - 1 : gx;
    *y = gy < 0 ? 0 : gy >= game_h ? game_h - 1 : gy;
}

/* The window's colour buffers are not valid while an FBO is bound; the
   FBO's own colour attachment stands in for them. */
static unsigned remap_draw_buffer(unsigned mode, int redirected) {
    if (redirected && mode >= 0x0400 && mode <= 0x0408) return GL_COLOR_ATTACHMENT0_;
    return mode;
}

static struct {
    int state;  /* 0 unresolved, 1 ready, -1 unavailable */
    void (*GenFramebuffers)(int, unsigned *);
    void (*DeleteFramebuffers)(int, const unsigned *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    void (*FramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
    unsigned (*CheckFramebufferStatus)(unsigned);
    void (*GenRenderbuffers)(int, unsigned *);
    void (*DeleteRenderbuffers)(int, const unsigned *);
    void (*BindRenderbuffer)(unsigned, unsigned);
    void (*RenderbufferStorage)(unsigned, unsigned, int, int);
    void (*RenderbufferStorageMultisample)(unsigned, int, unsigned, int, int);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(unsigned);
} gl_rt;

static int resolve_gl_rt(void) {
    if (gl_rt.state) return gl_rt.state > 0;
    gl_rt.state = -1;
    void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    void *scope = gl ? gl : RTLD_DEFAULT;
    void *(*get_proc)(const unsigned char *) =
        (void *(*)(const unsigned char *))dlsym(scope, "glXGetProcAddressARB");
    /* The core name first, then the EXT one from before OpenGL 3.0. */
#define RT_FN(field, name) \
    do { \
        static const char *const names[2] = {name, name "EXT"}; \
        for (int i = 0; i < 2 && !gl_rt.field; i++) { \
            *(void **)&gl_rt.field = get_proc ? get_proc((const unsigned char *)names[i]) : NULL; \
            if (!gl_rt.field) *(void **)&gl_rt.field = dlsym(scope, names[i]); \
        } \
        if (!gl_rt.field) { \
            fprintf(stderr, "[t2_hook] Render target unavailable: no %s\n", name); \
            return 0; \
        } \
    } while (0)
    RT_FN(GenFramebuffers, "glGenFramebuffers"); RT_FN(DeleteFramebuffers, "glDeleteFramebuffers");
    RT_FN(BindFramebuffer, "glBindFramebuffer");
    RT_FN(FramebufferTexture2D, "glFramebufferTexture2D");
    RT_FN(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    RT_FN(CheckFramebufferStatus, "glCheckFramebufferStatus");
    RT_FN(GenRenderbuffers, "glGenRenderbuffers"); RT_FN(DeleteRenderbuffers, "glDeleteRenderbuffers");
    RT_FN(BindRenderbuffer, "glBindRenderbuffer"); RT_FN(RenderbufferStorage, "glRenderbufferStorage");
    RT_FN(RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
    RT_FN(BlitFramebuffer, "glBlitFramebuffer"); RT_FN(ClearColor, "glClearColor");
    RT_FN(Clear, "glClear");
#undef RT_FN
    gl_rt.state = 1;
    return 1;
}

static struct {
    int active, scaling, samples;
    int game_w, game_h, real_w, real_h;
    struct render_rect rect;
    unsigned fbo, color_texture, depth;             /* single-sampled, sampled at swap */
    unsigned ms_fbo, ms_color, ms_depth;            /* multisampled, what the game draws to */
} rt;

/* video.render_scale: 0 off, 1 fit, 2 stretch. */
static int render_scale_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *value = hook_option_str("video.render_scale");
        mode = value && strcmp(value, "fit") == 0 ? 1 : value && strcmp(value, "stretch") == 0 ? 2 : 0;
    }
    return mode;
}

/* video.msaa: 0 or a sample count. */
static int msaa_samples(void) {
    static int samples = -1;
    if (samples < 0) {
        const char *value = hook_option_str("video.msaa");
        samples = value ? atoi(value) : 0;
        if (samples < 2) samples = 0;
        if (samples > 16) samples = 16;
    }
    return samples;
}

static int render_target_wanted(void) {
    return render_scale_mode() || msaa_samples();
}

/* The fullscreen display's largest mode: its native size. */
static int native_fullscreen_size(int *width, int *height) {
    if (!real_sdl_list_modes)
        real_sdl_list_modes = (sdl_list_modes_t)next_symbol("SDL_ListModes");
    if (!real_sdl_list_modes) return 0;
    sdl_rect_t **modes = real_sdl_list_modes(NULL, SDL_FULLSCREEN_FLAG | SDL_OPENGL_FLAG);
    if (!modes || modes == (sdl_rect_t **)-1) return 0;
    int best_w = 0, best_h = 0;
    for (int i = 0; modes[i]; i++)
        if ((long long)modes[i]->w * modes[i]->h > (long long)best_w * best_h) {
            best_w = modes[i]->w;
            best_h = modes[i]->h;
        }
    if (best_w <= 0 || best_h <= 0) return 0;
    *width = best_w;
    *height = best_h;
    return 1;
}

static unsigned rt_draw_fbo(void) { return rt.samples ? rt.ms_fbo : rt.fbo; }

/* Delete the targets while the context that owns them is still current. */
static void rt_teardown(void) {
    if (gl_rt.state > 0 && (rt.fbo || rt.ms_fbo)) {
        gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, 0);
        if (rt.ms_fbo) gl_rt.DeleteFramebuffers(1, &rt.ms_fbo);
        if (rt.ms_color) gl_rt.DeleteRenderbuffers(1, &rt.ms_color);
        if (rt.ms_depth) gl_rt.DeleteRenderbuffers(1, &rt.ms_depth);
        if (rt.fbo) gl_rt.DeleteFramebuffers(1, &rt.fbo);
        if (rt.depth) gl_rt.DeleteRenderbuffers(1, &rt.depth);
        if (rt.color_texture && gl_gamma.DeleteTextures) gl_gamma.DeleteTextures(1, &rt.color_texture);
    }
    memset(&rt, 0, sizeof(rt));
}

static unsigned rt_renderbuffer(int samples, unsigned format, int width, int height) {
    unsigned buffer = 0;
    gl_rt.GenRenderbuffers(1, &buffer);
    gl_rt.BindRenderbuffer(GL_RENDERBUFFER_, buffer);
    if (samples) gl_rt.RenderbufferStorageMultisample(GL_RENDERBUFFER_, samples, format, width, height);
    else gl_rt.RenderbufferStorage(GL_RENDERBUFFER_, format, width, height);
    return buffer;
}

/* After SDL_SetVideoMode, with the new context current. */
static void rt_setup(int game_w, int game_h, int real_w, int real_h, int scale_mode, int samples) {
    int scaling = scale_mode && (real_w != game_w || real_h != game_h);
    if ((!scaling && !samples) || game_w <= 0 || game_h <= 0) return;
    if (!resolve_gl_gamma() || !resolve_gl_rt()) return;
    int previous_texture = 0;
    gl_gamma.GetIntegerv(GL_TEXTURE_BINDING_2D_, &previous_texture);
    gl_gamma.GenTextures(1, &rt.color_texture);
    gl_gamma.BindTexture(GL_TEXTURE_2D_, rt.color_texture);
    gl_gamma.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, scaling ? GL_LINEAR_ : GL_NEAREST_);
    gl_gamma.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, scaling ? GL_LINEAR_ : GL_NEAREST_);
    gl_gamma.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    gl_gamma.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    gl_gamma.TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, game_w, game_h, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, NULL);
    gl_gamma.BindTexture(GL_TEXTURE_2D_, (unsigned)previous_texture);
    rt.depth = rt_renderbuffer(0, GL_DEPTH24_STENCIL8_, game_w, game_h);
    gl_rt.GenFramebuffers(1, &rt.fbo);
    gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, rt.fbo);
    gl_rt.FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, rt.color_texture, 0);
    gl_rt.FramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, GL_RENDERBUFFER_, rt.depth);
    if (gl_rt.CheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
        fprintf(stderr, "[t2_hook] Render target unavailable: framebuffer incomplete\n");
        rt_teardown();
        return;
    }
    if (samples) {
        int most = 0;
        gl_gamma.GetIntegerv(GL_MAX_SAMPLES_, &most);
        if (samples > most) samples = most;
        if (samples >= 2) {
            rt.ms_color = rt_renderbuffer(samples, GL_RGBA8_, game_w, game_h);
            rt.ms_depth = rt_renderbuffer(samples, GL_DEPTH24_STENCIL8_, game_w, game_h);
            gl_rt.GenFramebuffers(1, &rt.ms_fbo);
            gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, rt.ms_fbo);
            gl_rt.FramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_RENDERBUFFER_, rt.ms_color);
            gl_rt.FramebufferRenderbuffer(GL_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, GL_RENDERBUFFER_,
                                          rt.ms_depth);
            if (gl_rt.CheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
                fprintf(stderr, "[t2_hook] MSAA unavailable: multisampled framebuffer incomplete\n");
                gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, rt.fbo);
                gl_rt.DeleteFramebuffers(1, &rt.ms_fbo);
                gl_rt.DeleteRenderbuffers(1, &rt.ms_color);
                gl_rt.DeleteRenderbuffers(1, &rt.ms_depth);
                rt.ms_fbo = rt.ms_color = rt.ms_depth = 0;
                samples = 0;
            }
        } else {
            samples = 0;
        }
    }
    gl_rt.BindRenderbuffer(GL_RENDERBUFFER_, 0);
    if (!scaling && !samples) {  /* nothing left to do */
        rt_teardown();
        return;
    }
    rt.samples = samples;
    rt.scaling = scaling;
    rt.game_w = game_w; rt.game_h = game_h;
    rt.real_w = real_w; rt.real_h = real_h;
    rt.rect = render_fit_rect(game_w, game_h, real_w, real_h, scale_mode == 2);
    gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, rt_draw_fbo());
    rt.active = 1;
    fprintf(stderr, "[t2_hook] Rendering %dx%d into a %dx%d window%s%s\n", game_w, game_h, real_w, real_h,
            scaling ? (scale_mode == 2 ? ", stretched" : ", scaled to fit") : "",
            samples ? ", multisampled" : "");
    if (samples) fprintf(stderr, "[t2_hook] MSAA %dx\n", samples);
}

/* Resolve the multisampled target into the single-sampled one. */
static void rt_resolve(void) {
    if (!rt.samples) return;
    gl_gamma.PushAttrib(GL_SCISSOR_BIT_);
    gl_gamma.Disable(GL_SCISSOR_TEST_);
    gl_rt.BindFramebuffer(GL_READ_FRAMEBUFFER_, rt.ms_fbo);
    gl_rt.BindFramebuffer(GL_DRAW_FRAMEBUFFER_, rt.fbo);
    gl_rt.BlitFramebuffer(0, 0, rt.game_w, rt.game_h, 0, 0, rt.game_w, rt.game_h,
                          GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
    gl_gamma.PopAttrib();
}

/* Before the swap: draw the game's frame into the window. */
static void rt_present(void) {
    float exponent = post_exponent();
    gl_gamma.PushAttrib(GL_ALL_ATTRIB_BITS_);
    gl_gamma.Disable(GL_SCISSOR_TEST_);
    gl_gamma.ColorMask(1, 1, 1, 1);
    rt_resolve();
    gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, 0);
    gl_gamma.Viewport(0, 0, rt.real_w, rt.real_h);
    if (rt.rect.w != rt.real_w || rt.rect.h != rt.real_h) {  /* the bars */
        gl_rt.ClearColor(0, 0, 0, 1);
        gl_rt.Clear(GL_COLOR_BUFFER_BIT_);
    }
    if (exponent > 0.0f && gl_gamma.state > 0) {
        post_begin();
        gl_gamma.Viewport(rt.rect.x, rt.rect.y, rt.rect.w, rt.rect.h);
        post_quad(rt.color_texture, rt.game_w, rt.game_h, exponent, post_sharpness());
        post_end();
    } else {
        gl_rt.BindFramebuffer(GL_READ_FRAMEBUFFER_, rt.fbo);
        gl_rt.BindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
        gl_rt.BlitFramebuffer(0, 0, rt.game_w, rt.game_h, rt.rect.x, rt.rect.y,
                              rt.rect.x + rt.rect.w, rt.rect.y + rt.rect.h, GL_COLOR_BUFFER_BIT_,
                              rt.scaling ? GL_LINEAR_ : GL_NEAREST_);
        gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, 0);
    }
    gl_gamma.PopAttrib();
}

/* GL entry points the game resolves through SDL_GL_GetProcAddress. */
static void (*real_gl_draw_buffer)(unsigned);
static void (*real_gl_read_buffer)(unsigned);
static void (*real_gl_read_pixels)(int, int, int, int, unsigned, unsigned, void *);
static void (*real_gl_copy_tex_sub_image)(unsigned, int, int, int, int, int, int, int);

static void gl_draw_buffer_rt(unsigned mode) {
    if (real_gl_draw_buffer) real_gl_draw_buffer(remap_draw_buffer(mode, rt.active));
}

static void gl_read_buffer_rt(unsigned mode) {
    if (real_gl_read_buffer) real_gl_read_buffer(remap_draw_buffer(mode, rt.active));
}

/* Reads from a multisampled framebuffer are invalid: resolve and read the
   single-sampled copy (glReadPixels for screenshots, glCopyTexSubImage2D). */
static int rt_begin_read(void) {
    if (!rt.active || !rt.samples) return 0;
    rt_resolve();
    gl_rt.BindFramebuffer(GL_READ_FRAMEBUFFER_, rt.fbo);
    gl_rt.BindFramebuffer(GL_DRAW_FRAMEBUFFER_, rt.ms_fbo);
    return 1;
}

static void rt_end_read(int resolved) {
    if (resolved) gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, rt.ms_fbo);
}

static void gl_read_pixels_rt(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels) {
    int resolved = rt_begin_read();
    if (real_gl_read_pixels) real_gl_read_pixels(x, y, width, height, format, type, pixels);
    rt_end_read(resolved);
}

static void gl_copy_tex_sub_image_rt(unsigned target, int level, int xoffset, int yoffset,
                                     int x, int y, int width, int height) {
    int resolved = rt_begin_read();
    if (real_gl_copy_tex_sub_image)
        real_gl_copy_tex_sub_image(target, level, xoffset, yoffset, x, y, width, height);
    rt_end_read(resolved);
}

/* A wrapper for one of those entry points, or NULL to keep the original. */
static void *render_target_proc(const char *proc, void *address) {
    if (!address || !render_target_wanted()) return NULL;
    if (strcmp(proc, "glDrawBuffer") == 0) {
        real_gl_draw_buffer = (void (*)(unsigned))address;
        return (void *)gl_draw_buffer_rt;
    }
    if (strcmp(proc, "glReadBuffer") == 0) {
        real_gl_read_buffer = (void (*)(unsigned))address;
        return (void *)gl_read_buffer_rt;
    }
    if (strcmp(proc, "glReadPixels") == 0) {
        real_gl_read_pixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))address;
        return (void *)gl_read_pixels_rt;
    }
    if (strcmp(proc, "glCopyTexSubImage2D") == 0) {
        real_gl_copy_tex_sub_image = (void (*)(unsigned, int, int, int, int, int, int, int))address;
        return (void *)gl_copy_tex_sub_image_rt;
    }
    return NULL;
}

/* SDL 1.2 mouse events carry absolute positions in the real window: Uint16 x
   and y at offsets 4 and 6 of SDL_MOUSEMOTION (4), SDL_MOUSEBUTTONDOWN (5)
   and SDL_MOUSEBUTTONUP (6). Map them to the game's resolution; the relative
   motion that steers in-game is left alone. */
static void scale_mouse_event(unsigned char *event, struct render_rect rect, int real_h,
                              int game_w, int game_h) {
    if (event[0] != 4 && event[0] != 5 && event[0] != 6) return;
    uint16_t x, y;
    memcpy(&x, event + 4, sizeof(x));
    memcpy(&y, event + 6, sizeof(y));
    int mx = x, my = y;
    map_mouse_point(&mx, &my, rect, real_h, game_w, game_h);
    x = (uint16_t)mx;
    y = (uint16_t)my;
    memcpy(event + 4, &x, sizeof(x));
    memcpy(event + 6, &y, sizeof(y));
}

int SDL_PollEvent(void *event) {
    static int (*real_poll_event)(void *);
    if (!real_poll_event) real_poll_event = (int (*)(void *))next_symbol("SDL_PollEvent");
    if (!real_poll_event) return 0;
    int pending = real_poll_event(event);
    if (pending && event && rt.active && rt.scaling)
        scale_mouse_event((unsigned char *)event, rt.rect, rt.real_h, rt.game_w, rt.game_h);
    return pending;
}

int SDL_SetGamma(float red, float green, float blue) {
    static int (*real_set_gamma)(float, float, float);
    if (!hook_option_bool("video.gamma")) {
        if (!real_set_gamma) real_set_gamma = (int (*)(float, float, float))next_symbol("SDL_SetGamma");
        return real_set_gamma ? real_set_gamma(red, green, blue) : -1;
    }
    (void)green; (void)blue;  /* the game passes one value for all three */
    if (red <= 0.0f) return -1;
    if (red != gamma_requested)
        fprintf(stderr, "[t2_hook] Gamma %.3f\n", (double)red);
    gamma_requested = red;
    return 0;
}

/* ── Locked arrays (video.disable_locked_arrays) ─────────────────────────
 * Some drivers (AMD's rewritten Windows OpenGL driver, per the TribesNext
 * forum) tear and flicker once the game calls glLockArraysEXT, until the
 * context is destroyed. $pref::OpenGL::disableEXTCompiledVertexArray does not
 * cover every use, so hide the extension: the game resolves GL through
 * SDL_GL_GetProcAddress, and gets a glGetString without it and no
 * glLockArraysEXT. */
#define GL_EXTENSIONS_ 0x1F03
#define LOCKED_ARRAYS_EXTENSION "GL_EXT_compiled_vertex_array"

/* Copy extensions into output without one space-separated token. */
static void remove_extension(const char *extensions, const char *token, char *output, size_t size) {
    size_t used = 0, token_length = strlen(token);
    const char *cursor = extensions;
    if (size) output[0] = 0;
    while (*cursor) {
        while (*cursor == ' ') cursor++;
        const char *end = cursor;
        while (*end && *end != ' ') end++;
        size_t length = (size_t)(end - cursor);
        if (length && !(length == token_length && strncmp(cursor, token, length) == 0) &&
            used + length + 2 <= size) {
            if (used) output[used++] = ' ';
            memcpy(output + used, cursor, length);
            used += length;
            output[used] = 0;
        }
        cursor = end;
    }
}

static const unsigned char *(*real_gl_get_string)(unsigned);

/* No fog (ui.no_fog): the engine enables OpenGL distance fog with
   glEnable(GL_FOG). The hook returns this wrapper for glEnable so that
   enabling GL_FOG is dropped and every other capability passes through,
   removing fog without patching engine code (the Linux equivalent of the
   Windows T2NoFog patcher). */
#define GL_FOG_CAP 0x0B60u
static void (*real_gl_enable)(unsigned);
static void gl_enable_no_fog(unsigned cap) {
    if (cap == GL_FOG_CAP) return;
    if (real_gl_enable) real_gl_enable(cap);
}

static const unsigned char *filtered_gl_get_string(unsigned name) {
    static const unsigned char *source;
    static char *filtered;
    const unsigned char *value = real_gl_get_string ? real_gl_get_string(name) : NULL;
    if (name != GL_EXTENSIONS_ || !value) return value;
    if (value != source) {
        size_t size = strlen((const char *)value) + 1;
        char *copy = malloc(size);
        if (!copy) return value;
        remove_extension((const char *)value, LOCKED_ARRAYS_EXTENSION, copy, size);
        free(filtered);
        filtered = copy;
        source = value;
        fprintf(stderr, "[t2_hook] Hid %s from the game\n", LOCKED_ARRAYS_EXTENSION);
    }
    return (const unsigned char *)filtered;
}

void *SDL_GL_GetProcAddress(const char *proc) {
    static void *(*real_get_proc)(const char *);
    if (!real_get_proc) real_get_proc = (void *(*)(const char *))next_symbol("SDL_GL_GetProcAddress");
    if (!real_get_proc) return NULL;
    void *address = real_get_proc(proc);
    if (!proc) return address;
    void *wrapper = render_target_proc(proc, address);
    if (wrapper) return wrapper;
    if (address && hook_option_bool("ui.no_fog") && strcmp(proc, "glEnable") == 0) {
        real_gl_enable = (void (*)(unsigned))address;
        return (void *)gl_enable_no_fog;
    }
    if (!hook_option_bool("video.disable_locked_arrays")) return address;
    if (strcmp(proc, "glLockArraysEXT") == 0 || strcmp(proc, "glUnlockArraysEXT") == 0) return NULL;
    if (strcmp(proc, "glGetString") == 0 && address) {
        real_gl_get_string = (const unsigned char *(*)(unsigned))address;
        return (void *)filtered_gl_get_string;
    }
    return address;
}

/* Frame cap (video.fps_limit): advance an absolute deadline by one period per
   frame and report how long to sleep. A frame that ends more than a period
   late restarts the schedule instead of rushing to catch up. */
static long frame_cap_delay_ns(struct timespec *deadline, const struct timespec *now, long period_ns) {
    int64_t now_ns = (int64_t)now->tv_sec * 1000000000 + now->tv_nsec;
    int64_t due_ns = (int64_t)deadline->tv_sec * 1000000000 + deadline->tv_nsec;
    long delay = 0;
    if (due_ns == 0 || now_ns - due_ns > period_ns) due_ns = now_ns;
    else if (due_ns > now_ns) delay = (long)(due_ns - now_ns);
    due_ns += period_ns;
    deadline->tv_sec = (time_t)(due_ns / 1000000000);
    deadline->tv_nsec = (long)(due_ns % 1000000000);
    return delay;
}

static void cap_frame_rate(void) {
    static int limit = -1;
    static struct timespec deadline;
    if (limit < 0) {
        const char *value = hook_option_str("video.fps_limit");
        limit = value ? atoi(value) : 0;
        if (limit < 0) limit = 0;
        if (limit) fprintf(stderr, "[t2_hook] Frame rate capped at %d fps\n", limit);
    }
    if (!limit) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long delay = frame_cap_delay_ns(&deadline, &now, 1000000000L / limit);
    if (delay > 0) {
        struct timespec pause = {delay / 1000000000, delay % 1000000000};
        nanosleep(&pause, NULL);
    }
}

/* ---- Configurable render modifiers -----------------------------------------
   Rules use: [client|server|both] [connect|frame] object-pattern.field-pattern = value
   Object and field patterns support '*' and '?'; class:Pattern selects by
   Torque class name. Rules are local presentation overrides. */
#define RENDER_RULE_LIMIT 128
#define RENDER_RULE_TEXT 128
#define RENDER_RULE_VALUE 320
#define RENDER_MODIFIER_SCRIPT_LIMIT (256 * 1024)
#define RENDER_FIELD_CACHE_CLASSES 512
#define RENDER_FIELD_CACHE_FIELDS 512
enum render_rule_scope { RENDER_SCOPE_CLIENT, RENDER_SCOPE_SERVER, RENDER_SCOPE_BOTH };
enum render_rule_phase { RENDER_PHASE_CONNECT, RENDER_PHASE_FRAME };
struct render_rule {
    char object_pattern[RENDER_RULE_TEXT];
    char field_pattern[RENDER_RULE_TEXT];
    char value[RENDER_RULE_VALUE];
    enum render_rule_scope scope;
    enum render_rule_phase phase;
};
static struct render_rule render_rules[RENDER_RULE_LIMIT];
static size_t render_rule_count;
static char render_modifier_output_storage[RENDER_MODIFIER_SCRIPT_LIMIT];
static char *render_modifier_output;
static size_t render_modifier_output_used;
static size_t render_modifier_matches;
static int render_modifier_collecting;
static int render_modifier_output_truncated;
static int render_modifier_truncation_reported;
static enum render_rule_phase render_modifier_active_phase;
static int render_modifier_method_registered;
static int render_modifiers_installed;
static char render_modifier_last_context[64];
static uint64_t render_modifier_last_tick_ns;
static struct rv_memory render_modifier_memory;
struct render_field_cache {
    void *class_rep;
    const char *names[RENDER_FIELD_CACHE_FIELDS];
    size_t count;
};
static struct render_field_cache render_field_caches[RENDER_FIELD_CACHE_CLASSES];
static size_t render_field_cache_count;

static int render_glob_match(const char *pattern, const char *text) {
    if (!pattern || !text) return 0;
    const char *star = NULL, *retry = NULL;
    while (*text) {
        if (*pattern == '?' || (*pattern && tolower((unsigned char)*pattern) ==
                                tolower((unsigned char)*text))) {
            pattern++; text++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = text;
        } else if (star) {
            pattern = star + 1;
            text = ++retry;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return *pattern == 0;
}

static void db_format_value(const char *raw, char *out, size_t outlen) {
    if (!raw || !outlen) return;
    int numeric = *raw != 0;
    for (const char *p = raw; *p; p++) {
        if ((*p >= '0' && *p <= '9') || *p == ' ' || *p == '.' ||
            *p == '-' || *p == '+' || *p == 'e' || *p == 'E') continue;
        numeric = 0; break;
    }
    if (numeric || strcmp(raw, "true") == 0 || strcmp(raw, "false") == 0 ||
        strcmp(raw, "True") == 0 || strcmp(raw, "False") == 0) {
        snprintf(out, outlen, "%s", raw);
        return;
    }
    if (raw[0] == '"' && raw[strlen(raw) - 1] == '"') {
        snprintf(out, outlen, "%s", raw);
        return;
    }
    size_t used = 0;
    out[used++] = '"';
    for (const unsigned char *p = (const unsigned char *)raw; *p && used + 2 < outlen; p++) {
        if (*p == '"' || *p == '\\') out[used++] = '\\';
        if (*p == '\n' || *p == '\r') continue;
        out[used++] = (char)*p;
    }
    out[used++] = '"'; out[used] = 0;
}

static int render_parse_rule(char *line, enum render_rule_scope default_scope,
                             enum render_rule_phase default_phase,
                             struct render_rule *rule) {
    char *text = trim_space(line);
    if (!*text || *text == '#') return 0;
    rule->scope = default_scope;
    rule->phase = default_phase;
    char *assignment;
    /* Legacy datablocks.cfg entries were simply Object.field = value. */
    char *separator = strpbrk(text, " \t");
    char *dot = strrchr(text, '.');
    char *equals = strchr(text, '=');
    if (dot && equals && (!separator || dot < separator)) {
        assignment = text;
    } else {
        if (!separator) return -1;
        char *remainder = separator + 1;
        *separator = 0;
        char *second = trim_space(remainder);
        char *space = strpbrk(second, " \t");
        if (!space) return -1;
        *space++ = 0;
        if (strcmp(text, "client") == 0) rule->scope = RENDER_SCOPE_CLIENT;
        else if (strcmp(text, "server") == 0) rule->scope = RENDER_SCOPE_SERVER;
        else if (strcmp(text, "both") == 0) rule->scope = RENDER_SCOPE_BOTH;
        else return -1;
        if (strcmp(second, "connect") == 0) rule->phase = RENDER_PHASE_CONNECT;
        else if (strcmp(second, "frame") == 0) rule->phase = RENDER_PHASE_FRAME;
        else return -1;
        assignment = trim_space(space);
    }
    equals = strchr(assignment, '=');
    if (!equals) return -1;
    *equals++ = 0;
    char *lhs = trim_space(assignment);
    char *rhs = trim_space(equals);
    dot = strrchr(lhs, '.');
    if (!dot || !*rhs) return -1;
    *dot++ = 0;
    char *obj = trim_space(lhs), *field = trim_space(dot);
    if (!*obj || !*field || strlen(obj) >= sizeof rule->object_pattern ||
        strlen(field) >= sizeof rule->field_pattern || strlen(rhs) >= sizeof rule->value)
        return -1;
    if (strchr(rhs, '\n') || strchr(rhs, '\r')) return -1;
    snprintf(rule->object_pattern, sizeof rule->object_pattern, "%s", obj);
    snprintf(rule->field_pattern, sizeof rule->field_pattern, "%s", field);
    db_format_value(rhs, rule->value, sizeof rule->value);
    return 1;
}

static size_t render_load_rules_file(const char *path, int object_rules_file) {
    if (!path || !*path) return 0;
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "[t2_hook] render modifier config unavailable: %s (%s)\n",
                path, strerror(errno));
        return 0;
    }
    size_t loaded = 0;
    char line[768];
    unsigned long line_number = 0;
    while (fgets(line, sizeof line, file)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(file)) {
            int ch; while ((ch = fgetc(file)) != '\n' && ch != EOF) {}
            fprintf(stderr, "[t2_hook] render modifier line too long: %s:%lu\n", path, line_number);
            continue;
        }
        if (render_rule_count >= RENDER_RULE_LIMIT) {
            fprintf(stderr, "[t2_hook] render modifier rule limit (%d) reached\n", RENDER_RULE_LIMIT);
            break;
        }
        struct render_rule rule;
        int parsed = render_parse_rule(line, RENDER_SCOPE_BOTH, RENDER_PHASE_FRAME, &rule);
        if (parsed < 0 || (parsed > 0 && object_rules_file && rule.phase != RENDER_PHASE_FRAME)) {
            fprintf(stderr, "[t2_hook] invalid render modifier: %s:%lu\n", path, line_number);
            continue;
        }
        if (!parsed) continue;
        render_rules[render_rule_count++] = rule;
        loaded++;
    }
    fclose(file);
    fprintf(stderr, "[t2_hook] loaded %zu render modifier rules from %s\n", loaded, path);
    return loaded;
}

static int render_name_is_safe(const char *name, int field_name) {
    if (!name || !*name) return 0;
    const unsigned char *first = (const unsigned char *)name;
    if (!(isalpha(*first) || *first == '_' || (!field_name && (*first == '$' || *first == ':'))))
        return 0;
    for (const unsigned char *p = first; *p; p++) {
        if (isalnum(*p) || *p == '_' || (!field_name && (*p == ':' || *p == '$'))) continue;
        return 0;
    }
    return 1;
}

static int render_append_assignment(const char *object_name, const char *field_name,
                                    const char *value) {
    if (!render_modifier_output || !render_name_is_safe(object_name, 0) ||
        !render_name_is_safe(field_name, 1) || render_modifier_output_truncated) return 0;
    int written = snprintf(render_modifier_output + render_modifier_output_used,
        RENDER_MODIFIER_SCRIPT_LIMIT - render_modifier_output_used,
        "if(isObject(%s)) %s.%s = %s;\n", object_name, object_name, field_name, value);
    if (written < 0 || (size_t)written >= RENDER_MODIFIER_SCRIPT_LIMIT - render_modifier_output_used) {
        render_modifier_output_truncated = 1;
        return 0;
    }
    render_modifier_output_used += (size_t)written;
    render_modifier_matches++;
    return 1;
}

static int render_rule_applies(const struct render_rule *rule, const char *object_name,
                               const char *class_name, const char *field_name) {
    const char *selector = rule->object_pattern;
    if (strncmp(selector, "class:", 6) == 0) {
        if (!render_glob_match(selector + 6, class_name)) return 0;
    } else if (!render_glob_match(selector, object_name)) return 0;
    return render_glob_match(rule->field_pattern, field_name);
}

static int render_rule_object_applies(const struct render_rule *rule, const char *object_name,
                                      const char *class_name) {
    const char *selector = rule->object_pattern;
    if (strncmp(selector, "class:", 6) == 0)
        return render_glob_match(selector + 6, class_name);
    return render_glob_match(selector, object_name);
}

static int render_rules_have_phase(enum render_rule_phase phase) {
    for (size_t i = 0; i < render_rule_count; i++)
        if (render_rules[i].phase == phase) return 1;
    return 0;
}

static struct render_field_cache *render_fields_for_object(void *obj) {
    void *rep = rv_class_rep(&render_modifier_memory, obj);
    if (!rep || !rv_memory_range(&render_modifier_memory, rep, 0x40, PROT_READ)) return NULL;
    for (size_t i = 0; i < render_field_cache_count; i++)
        if (render_field_caches[i].class_rep == rep) return &render_field_caches[i];
    if (render_field_cache_count >= RENDER_FIELD_CACHE_CLASSES) return NULL;
    struct render_field_cache *cache = &render_field_caches[render_field_cache_count++];
    memset(cache, 0, sizeof *cache);
    cache->class_rep = rep;
    for (size_t slot = 4; slot < 0x40; slot += 4) {
        void *fields = rv_pointer(rep, slot);
        if (!rv_memory_range(&render_modifier_memory, fields, 24, PROT_READ)) continue;
        for (size_t count_slot = 4; count_slot < 0x40; count_slot += 4) {
            if (count_slot == slot) continue;
            uint32_t count = rv_word(rep, count_slot);
            if (!count || count > RENDER_FIELD_CACHE_FIELDS ||
                !rv_memory_range(&render_modifier_memory, fields, (size_t)count * 24, PROT_READ))
                continue;
            for (uint32_t field_index = 0; field_index < count; field_index++) {
                void *field = (unsigned char *)fields + (size_t)field_index * 24;
                const char *name = rv_string(&render_modifier_memory, rv_pointer(field, 0), 128);
                uint32_t offset = rv_word(field, 8), verified_offset = 0;
                if (!name || offset > 0x10000 || !render_name_is_safe(name, 1) ||
                    !rv_persist_field_offset(&render_modifier_memory, obj, name, &verified_offset) ||
                    verified_offset != offset) continue;
                int duplicate = 0;
                for (size_t i = 0; i < cache->count; i++)
                    if (strcmp(cache->names[i], name) == 0) { duplicate = 1; break; }
                if (!duplicate && cache->count < RENDER_FIELD_CACHE_FIELDS)
                    cache->names[cache->count++] = name;
            }
        }
    }
    return cache;
}

/* The per-object ConsoleMethod uses the same registered Field metadata and
   SimFieldDictionary used by the hook's other safe field readers. */
static const char *render_modifier_object_cb(void *obj, S32 argc, const char **argv) {
    if (!render_modifier_collecting || render_modifier_output_truncated || argc != 4 ||
        !obj || !render_modifier_output) return "";
    const char *object_name = argv && argv[2] ? argv[2] : "";
    const char *class_name = argv && argv[3] ? argv[3] : "";
    if (!*object_name || !*class_name) return "";
    struct render_field_cache *fields = NULL;
    int fields_loaded = 0;
    for (size_t r = 0; r < render_rule_count; r++) {
        const struct render_rule *rule = &render_rules[r];
        if (rule->phase != render_modifier_active_phase) continue;
        int scope_ok = rule->scope == RENDER_SCOPE_BOTH ||
            (rule->scope == RENDER_SCOPE_CLIENT && !target->dedicated) ||
            (rule->scope == RENDER_SCOPE_SERVER && target->dedicated);
        if (!scope_ok || !render_rule_object_applies(rule, object_name, class_name)) continue;
        if (!fields_loaded) {
            fields = render_fields_for_object(obj);
            fields_loaded = 1;
        }
        const char *seen_fields[1024]; size_t seen_count = 0;
        for (size_t field_index = 0; fields && field_index < fields->count; field_index++) {
            const char *name = fields->names[field_index];
            if (render_rule_applies(rule, object_name, class_name, name)) {
                seen_fields[seen_count++] = name;
                (void)render_append_assignment(object_name, name, rule->value);
            }
        }
        if (!rv_memory_range(&render_modifier_memory,
                (unsigned char *)obj + SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET,
                sizeof(uint32_t), PROT_READ)) continue;
        void *dictionary = rv_pointer(obj, SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET);
        if (!rv_memory_range(&render_modifier_memory, dictionary,
                SHAZBOT_SIM_FIELD_BUCKET_COUNT * sizeof(uint32_t), PROT_READ)) continue;
        for (size_t bucket = 0; bucket < SHAZBOT_SIM_FIELD_BUCKET_COUNT; bucket++) {
            void *entry = rv_pointer(dictionary, bucket * sizeof(uint32_t));
            void *visited[128]; size_t visited_count = 0;
            while (entry && visited_count < 128) {
                int duplicate = 0;
                for (size_t i = 0; i < visited_count; i++) if (visited[i] == entry) duplicate = 1;
                if (duplicate || !rv_memory_range(&render_modifier_memory, entry, 12, PROT_READ)) break;
                visited[visited_count++] = entry;
                const char *name = rv_string(&render_modifier_memory, rv_pointer(entry, 0), 128);
                int seen = 0;
                for (size_t s = 0; name && s < seen_count; s++)
                    if (strcmp(seen_fields[s], name) == 0) { seen = 1; break; }
                if (name && !seen && seen_count < sizeof seen_fields / sizeof seen_fields[0] &&
                    render_name_is_safe(name, 1) &&
                    render_rule_applies(rule, object_name, class_name, name)) {
                    seen_fields[seen_count++] = name;
                    (void)render_append_assignment(object_name, name, rule->value);
                }
                entry = rv_pointer(entry, 8);
            }
        }
    }
    return "";
}

static const char render_modifier_scan_script[] =
    "function t2launch_scanModifierGroup(%group) {"
    " if (!isObject(%group) || $T2Launch::Render::Scanned >= 4096) return;"
    " for (%i = 0; %i < %group.getCount(); %i++) {"
    "  if ($T2Launch::Render::Scanned >= 4096) return;"
    "  %object = %group.getObject(%i); if (!isObject(%object)) continue;"
    "  $T2Launch::Render::Scanned++;"
    "  %object.t2launch_collectRenderModifiers(%object.getName(), %object.getClassName());"
    "  %class = %object.getClassName();"
    "  %length = strLen(%class);"
    "  if (%class $= \"SimGroup\" || %class $= \"SimSet\" ||"
    "      getSubStr(%class, 0, 3) $= \"Gui\" ||"
    "      (%length >= 5 && getSubStr(%class, %length - 5, 5) $= \"Group\"))"
    "   t2launch_scanModifierGroup(%object);"
    " }"
    "}"
    "function t2launch_collectRenderModifiers() {"
    " $T2Launch::Render::Scanned = 0;"
    " if (isObject(RootGroup)) t2launch_scanModifierGroup(RootGroup);"
    "} ";

static const char render_modifier_context_script[] =
    "function t2launch_renderModifierContext() {"
    " %connection = \"\"; %mission = \"\";"
    " %serverConnections = \"\";"
    " if (isObject(ServerConnection)) %connection = ServerConnection.getId();"
    " if (isObject(MissionGroup)) %mission = MissionGroup.getId();"
    " if (isObject(ServerGroup)) %serverConnections = ServerGroup.getCount();"
    " return %connection @ \":\" @ %mission @ \":\" @ %serverConnections;"
    "}";

static void render_modifiers_apply(enum render_rule_phase phase) {
    if (!render_rule_count || !render_rules_have_phase(phase) || !Con__evaluate ||
        !render_modifier_method_registered || !target || !rv_memory_snapshot(&render_modifier_memory)) return;
    render_modifier_output_storage[0] = 0;
    render_modifier_output = render_modifier_output_storage;
    render_modifier_output_used = render_modifier_matches = 0;
    render_modifier_output_truncated = 0;
    render_modifier_collecting = 1;
    render_modifier_active_phase = phase;
    (void)Con__evaluate("t2launch_collectRenderModifiers();", 0, NULL, 0);
    render_modifier_collecting = 0;
    render_modifier_output = NULL;
    if (render_modifier_output_used) {
        (void)Con__evaluate(render_modifier_output_storage, 0, "t2launch/render-modifiers.cs", 0);
        static int frame_count_reported;
        if (phase == RENDER_PHASE_CONNECT || !frame_count_reported) {
            fprintf(stderr, "[t2_hook] applied %zu render modifier matches (phase=%s)\n",
                    render_modifier_matches, phase == RENDER_PHASE_CONNECT ? "connect" : "frame");
            if (phase == RENDER_PHASE_FRAME) frame_count_reported = 1;
        }
    }
    if (render_modifier_output_truncated && !render_modifier_truncation_reported) {
        fprintf(stderr, "[t2_hook] render modifier output capped at %u bytes\n",
                (unsigned)RENDER_MODIFIER_SCRIPT_LIMIT);
        render_modifier_truncation_reported = 1;
    }
}

static void install_render_modifiers(void) {
    if (render_modifiers_installed || !Con__evaluate || !Con__addMethodS) return;
    render_modifiers_installed = 1;
    (void)render_load_rules_file(hook_option_str("render.datablock_rules"), 0);
    (void)render_load_rules_file(hook_option_str("render.object_rules"), 1);
    /* Preserve the legacy environment variable and extra hook-option key. */
    const char *legacy = getenv("T2LAUNCH_DATABLOCK_CONFIG");
    if (!legacy || !*legacy) legacy = hook_option_str("datablock.config");
    (void)render_load_rules_file(legacy, 0);
    if (!render_rule_count) return;
    Con__addMethodS("SimObject", "t2launch_collectRenderModifiers", render_modifier_object_cb,
        "t2launch_collectRenderModifiers(name, class) - internal render-rule collector", 4, 4);
    render_modifier_method_registered = 1;
    (void)Con__evaluate(render_modifier_scan_script, 0, "t2launch/render-modifiers.cs", 0);
    (void)Con__evaluate(render_modifier_context_script, 0, "t2launch/render-modifiers.cs", 0);
    render_modifiers_apply(RENDER_PHASE_CONNECT);
    const char *context = Con__evaluate("t2launch_renderModifierContext();", 0, NULL, 0);
    if (context) snprintf(render_modifier_last_context, sizeof render_modifier_last_context, "%s", context);
}

static void render_modifiers_tick(void) {
    if (!registered) return;
    if (target && target->dedicated) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return;
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
        if (render_modifier_last_tick_ns && now_ns >= render_modifier_last_tick_ns &&
            now_ns - render_modifier_last_tick_ns < 16666667ULL) return;
        render_modifier_last_tick_ns = now_ns;
    }
    if (!render_modifiers_installed) install_render_modifiers();
    if (!render_modifiers_installed || !render_modifier_method_registered) return;
    render_modifiers_apply(RENDER_PHASE_FRAME);
    const char *context = Con__evaluate("t2launch_renderModifierContext();", 0, NULL, 0);
    if (render_rules_have_phase(RENDER_PHASE_CONNECT) && context &&
        strcmp(context, render_modifier_last_context) != 0) {
        render_modifiers_apply(RENDER_PHASE_CONNECT);
        snprintf(render_modifier_last_context, sizeof render_modifier_last_context, "%s", context);
    }
}

/* Small T2-native RadarHUD. It deliberately uses the client's *visible*
   ShapeBase set for opposing players; teammates and the assigned ClientTarget
   may be shown independently. The T1 memory offsets/map palette are not used. */
#define RADAR_MARKER_LIMIT 96
#define RADAR_WORLD_RANGE 600.0f
#define RADAR_TERRAIN_SIDE 64
#define RADAR_TERRAIN_STRIDE 4
#define RADAR_VIEWPORT 0x0ba2
#define RADAR_PROJECTION 0x1701
#define RADAR_MODELVIEW 0x1700
#define RADAR_MATRIX_MODE 0x0ba0
#define RADAR_PROJECTION_MATRIX 0x0ba7
#define RADAR_MODELVIEW_MATRIX 0x0ba6
#define RADAR_ALL_ATTRIB_BITS 0x000fffff
#define RADAR_LINES 0x0001
#define RADAR_LINE_LOOP 0x0002
#define RADAR_TRIANGLE_FAN 0x0006
#define RADAR_QUADS 0x0007
#define RADAR_BLEND 0x0be2
#define RADAR_DEPTH_TEST 0x0b71
#define RADAR_TEXTURE_2D 0x0de1
#define RADAR_TEXTURE_ENV 0x2300
#define RADAR_TEXTURE_ENV_MODE 0x2200
#define RADAR_MODULATE 0x2100
#define RADAR_CULL_FACE 0x0b44
#define RADAR_SCISSOR_TEST 0x0c11
#define RADAR_ALPHA_TEST 0x0bc0
#define RADAR_SRC_ALPHA 0x0302
#define RADAR_ONE_MINUS_SRC_ALPHA 0x0303
#define RADAR_COMPASS_MARKER_COUNT 5
static void sky_sincos(float angle, float *sine, float *cosine);

struct radar_marker { float dx, dy; unsigned char kind; };
struct radar_terrain {
    uint16_t height[RADAR_TERRAIN_SIDE * RADAR_TERRAIN_SIDE];
    uint8_t material[RADAR_TERRAIN_SIDE * RADAR_TERRAIN_SIDE];
    float origin_x, origin_y, square_size;
    uint16_t minimum, maximum;
    int valid;
};
static struct radar_marker radar_markers[RADAR_MARKER_LIMIT];
static size_t radar_marker_count;
static uint64_t radar_last_refresh_ns;
static int radar_data_valid;
static struct radar_terrain radar_terrain_map;
static float radar_origin[3];
static float radar_forward_x, radar_forward_y;
static uint32_t hud_demo_compass_marker_ids[RADAR_COMPASS_MARKER_COUNT];
static uint64_t hud_demo_compass_last_state_ns;
static int hud_demo_compass_cached_enabled;
static float hud_demo_compass_view_projection[16];
static float hud_demo_compass_eye[3];
static int hud_demo_compass_viewport[4];
static int hud_demo_compass_camera_valid;
static int sky_rotation_hook_attempted;
static int sky_rotation_hook_installed;
static void *sky_rotation_trampoline;
static float sky_rotation_angle;
static uint64_t sky_rotation_last_ns;

typedef struct {
    void (*GetIntegerv)(unsigned, int *);
    void (*GetFloatv)(unsigned, float *);
    void (*PushAttrib)(unsigned);
    void (*PopAttrib)(void);
    void (*MatrixMode)(unsigned);
    void (*PushMatrix)(void);
    void (*PopMatrix)(void);
    void (*LoadIdentity)(void);
    void (*Ortho)(double, double, double, double, double, double);
    void (*Disable)(unsigned);
    void (*Enable)(unsigned);
    void (*BlendFunc)(unsigned, unsigned);
    void (*Color4f)(float, float, float, float);
    void (*TexEnvi)(unsigned, unsigned, int);
    void (*BindTexture)(unsigned, uint32_t);
    void (*TexCoord2f)(float, float);
    void (*Begin)(unsigned);
    void (*End)(void);
    void (*Vertex2f)(float, float);
    void (*LineWidth)(float);
} radar_gl_api;
static radar_gl_api radar_gl;
static int radar_gl_state;

static int radar_resolve_gl(void) {
    if (radar_gl_state) return radar_gl_state > 0;
    radar_gl_state = -1;
    void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    void *scope = lib ? lib : RTLD_DEFAULT;
#define RADAR_GL(field, name) do { \
    *(void **)(&radar_gl.field) = dlsym(scope, name); \
    if (!radar_gl.field) return 0; \
} while (0)
    RADAR_GL(GetIntegerv, "glGetIntegerv");
    RADAR_GL(GetFloatv, "glGetFloatv");
    RADAR_GL(PushAttrib, "glPushAttrib"); RADAR_GL(PopAttrib, "glPopAttrib");
    RADAR_GL(MatrixMode, "glMatrixMode"); RADAR_GL(PushMatrix, "glPushMatrix");
    RADAR_GL(PopMatrix, "glPopMatrix"); RADAR_GL(LoadIdentity, "glLoadIdentity");
    RADAR_GL(Ortho, "glOrtho"); RADAR_GL(Disable, "glDisable");
    RADAR_GL(Enable, "glEnable"); RADAR_GL(BlendFunc, "glBlendFunc");
    RADAR_GL(Color4f, "glColor4f"); RADAR_GL(TexEnvi, "glTexEnvi");
    RADAR_GL(BindTexture, "glBindTexture");
    RADAR_GL(TexCoord2f, "glTexCoord2f"); RADAR_GL(Begin, "glBegin");
    RADAR_GL(End, "glEnd"); RADAR_GL(Vertex2f, "glVertex2f");
    RADAR_GL(LineWidth, "glLineWidth");
#undef RADAR_GL
    radar_gl_state = 1;
    return 1;
}

static int radar_console_enabled(void) {
    if (!Con__evaluate || !core_runtime_installed) return 0;
    const char *value = Con__evaluate("t2launch_hudDemoRadarEnabled();", 0, NULL, 0);
    return value && value[0] == '1' && value[1] == '\0';
}

static void radar_add_world_marker(float x, float y, unsigned char kind) {
    if (!isfinite(x) || !isfinite(y) || radar_marker_count >= RADAR_MARKER_LIMIT) return;
    radar_markers[radar_marker_count++] = (struct radar_marker){x, y, kind};
}

static size_t radar_collect_visible_enemies(const struct rv_memory *memory, void *visible,
                                            void *dictionary, uint32_t self_id,
                                            int local_team, const float origin[3],
                                            struct radar_marker *out, size_t capacity) {
    if (!memory || !visible || !dictionary || !self_id || !origin || !out || !capacity ||
        !rv_memory_range(memory, visible, 0x3c, PROT_READ)) return 0;
    uint32_t count = rv_word(visible, 0x30);
    void *objects = rv_pointer(visible, 0x38);
    if (count > 4096 || (count && !rv_memory_range(memory, objects,
                                      (size_t)count * sizeof(uint32_t), PROT_READ))) return 0;
    size_t used = 0;
    for (uint32_t i = 0; i < count && used < capacity; i++) {
        void *object = rv_pointer(objects, (size_t)i * sizeof(uint32_t));
        const char *name = rv_class_name(memory, object);
        if (!name || strcmp(name, "Player") != 0 ||
            !rv_memory_range(memory, object, 0x2d4, PROT_READ)) continue;
        uint32_t id = rv_word(object, 0x20);
        if (!id || id == self_id || !(rv_word(object, 0x40) & 2) ||
            rv_team(memory, object) == local_team ||
            rv_find_object(memory, dictionary, id) != object) continue;
        float position[3];
        if (!rv_position(memory, object, RV_PLAYER, position)) continue;
        out[used++] = (struct radar_marker){
            position[0] - origin[0], position[1] - origin[1], 1
        };
    }
    return used;
}

struct radar_point { float x, y; };
static float radar_inverse_sqrt(float value) {
    union { float f; uint32_t u; } estimate = {value};
    estimate.u = 0x5f375a86u - (estimate.u >> 1);
    float y = estimate.f;
    return y * (1.5f - 0.5f * value * y * y);
}

/* Use the current control object's eye transform for both Player and Camera.
   Camera is the observer/demo-playback control object; no Player is required. */
static int radar_control_view(const struct rv_memory *memory, void *connection,
                              void *dictionary, float origin[3], float forward[2],
                              uint32_t *object_id, int *team_out) {
    if (!memory || !connection || !dictionary || !origin || !forward || !object_id || !team_out ||
        !rv_memory_range(memory, connection, SHAZBOT_LOCAL_CONTROL_OFFSET + sizeof(uint32_t),
                         PROT_READ)) return 0;
    void *object = rv_pointer(connection, SHAZBOT_LOCAL_CONTROL_OFFSET);
    if (!rv_memory_range(memory, object, 0x164, PROT_READ)) return 0;
    const char *class_name = rv_class_name(memory, object);
    if (!class_name || (strcmp(class_name, "Player") != 0 && strcmp(class_name, "Camera") != 0 &&
        strcmp(class_name, "Vehicle") != 0 && strcmp(class_name, "WheeledVehicle") != 0 &&
        strcmp(class_name, "FlyingVehicle") != 0 && strcmp(class_name, "HoverVehicle") != 0 &&
        strcmp(class_name, "Turret") != 0 && strcmp(class_name, "StaticShape") != 0 &&
        strcmp(class_name, "Item") != 0)) return 0;
    uint32_t id = rv_word(object, 0x20);
    if (!id || rv_find_object(memory, dictionary, id) != object) return 0;

    void *vtable = rv_pointer(object, 0);
    if (!rv_memory_range(memory, vtable, SHAZBOT_EYE_TRANSFORM_VTABLE_OFFSET + 4, PROT_READ)) return 0;
    void *eye_method = rv_pointer(vtable, SHAZBOT_EYE_TRANSFORM_VTABLE_OFFSET);
    if (!rv_memory_range(memory, eye_method, 1, PROT_EXEC)) return 0;
    float matrix[16];
    memset(matrix, 0, sizeof matrix);
    ((void (*)(void *, float *))eye_method)(object, matrix);
    origin[0] = matrix[3]; origin[1] = matrix[7]; origin[2] = matrix[11];
    if (!isfinite(origin[0]) || !isfinite(origin[1]) || !isfinite(origin[2])) return 0;
    float fx = matrix[1], fy = matrix[5];
    float length2 = fx * fx + fy * fy;
    if (!isfinite(fx) || !isfinite(fy) || !isfinite(length2) || length2 < 1.0e-8f) return 0;
    float inverse = radar_inverse_sqrt(length2);
    forward[0] = fx * inverse;
    forward[1] = fy * inverse;
    if (!isfinite(forward[0]) || !isfinite(forward[1])) return 0;

    int team = RV_OTHER_TEAM;
    if (rv_memory_range(memory, connection, SHAZBOT_LOCAL_SENSOR_GROUP_OFFSET + 4, PROT_READ))
        team = (int32_t)rv_word(connection, SHAZBOT_LOCAL_SENSOR_GROUP_OFFSET);
    if (team <= 0 && strcmp(class_name, "Camera") != 0)
        team = rv_team(memory, object);
    if (team <= 0) team = RV_OTHER_TEAM;
    *object_id = id;
    *team_out = team;
    return 1;
}

/* Torque's Player/Camera console bindings do not consistently expose
   getEyeVector() across the shipped client builds. Read the same native eye
   transform used by RadarHUD and return a heading for the optional demo HUD. */
static const char *hook_hud_demo_heading_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    static char heading_text[32];
    heading_text[0] = 0;
    if (target->dedicated) return heading_text;

    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory) ||
        !rv_memory_range(&memory, (void *)SHAZBOT_CONNECTION, sizeof(uint32_t), PROT_READ) ||
        !rv_memory_range(&memory, (void *)SHAZBOT_ID_DICTIONARY, sizeof(uint32_t), PROT_READ))
        return heading_text;
    void *connection = rv_pointer((void *)SHAZBOT_CONNECTION, 0);
    void *dictionary = rv_pointer((void *)SHAZBOT_ID_DICTIONARY, 0);
    float origin[3], forward[2];
    uint32_t object_id;
    int team;
    if (!radar_control_view(&memory, connection, dictionary, origin, forward,
                            &object_id, &team)) return heading_text;
    /* Compass bearings increase clockwise: +world X is east from +world Y north. */
    float y = forward[0], x = forward[1];
    float ay = y < 0.0f ? -y : y, ax = x < 0.0f ? -x : x;
    float ratio = ax >= ay ? ay / ax : ax / ay;
    /* atan(r) ~= r * (45 - 14.32 * (1 - r)); accurate enough for a HUD tick
       and avoids adding a libm dependency to the hook's native test fixtures. */
    float angle = ratio * (45.0f - 14.32f * (1.0f - ratio));
    if (ay > ax) angle = 90.0f - angle;
    if (x < 0.0f) angle = 180.0f - angle;
    if (y < 0.0f) angle = -angle;
    float heading = angle;
    if (heading < 0.0f) heading += 360.0f;
    if (heading >= 360.0f) heading -= 360.0f;
    snprintf(heading_text, sizeof(heading_text), "%.3f", heading);
    return heading_text;
}

static const char *hook_hud_demo_compass_markers_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (target->dedicated) return "client only";
    if (argc != RADAR_COMPASS_MARKER_COUNT + 1)
        return "expected north, east, south, west, and strip bitmap IDs";
    uint32_t ids[RADAR_COMPASS_MARKER_COUNT];
    for (size_t i = 0; i < RADAR_COMPASS_MARKER_COUNT; i++) {
        if (!argv[i + 1] || !argv[i + 1][0]) return "invalid bitmap control ID";
        errno = 0;
        char *end = NULL;
        unsigned long value = strtoul(argv[i + 1], &end, 10);
        if (errno || end == argv[i + 1] || !end || *end || !value || value > UINT32_MAX)
            return "invalid bitmap control ID";
        ids[i] = (uint32_t)value;
    }
    memcpy(hud_demo_compass_marker_ids, ids, sizeof ids);
    return "ok";
}

static int radar_capture_terrain(const struct rv_memory *memory, void *dictionary) {
    radar_terrain_map.valid = 0;
    if (!memory || !dictionary) return 0;
    void *terrain = NULL;
    unsigned int visited = 0;
    for (size_t bucket = 0; bucket < 4096 && visited < 131072 && !terrain; bucket++) {
        void *object = rv_pointer(dictionary, bucket * sizeof(uint32_t));
        for (unsigned int chain = 0; object && chain < 4096 && visited++ < 131072; chain++) {
            if (!rv_memory_range(memory, object, 0x24, PROT_READ)) break;
            void *next = rv_pointer(object, 0x10);
            const char *name = rv_class_name(memory, object);
            if (name && strcmp(name, "TerrainBlock") == 0) terrain = object;
            object = next;
        }
    }
    if (!terrain) return 0;

    uint32_t terrain_file_offset, square_size_offset;
    if (!rv_persist_field_offset(memory, terrain, "terrainFile", &terrain_file_offset) ||
        !rv_persist_field_offset(memory, terrain, "squareSize", &square_size_offset) ||
        terrain_file_offset < 4 * sizeof(uint32_t)) return 0;
    const size_t height_offset = terrain_file_offset - 4 * sizeof(uint32_t);
    const uint32_t square_size = rv_word(terrain, square_size_offset);
    if (square_size == 0 || square_size > 128) return 0;

    const uint16_t *height_map = (const uint16_t *)rv_pointer(terrain, height_offset);
    const uint8_t *material_map = (const uint8_t *)rv_pointer(terrain, height_offset - 4);
    if (!rv_memory_range(memory, height_map, 256u * 256u * sizeof(uint16_t), PROT_READ) ||
        !rv_memory_range(memory, material_map, 256u * 256u * 2u, PROT_READ)) return 0;

    float terrain_position[3];
    if (!rv_position(memory, terrain, RV_SHAPE, terrain_position)) return 0;
    uint16_t minimum = UINT16_MAX, maximum = 0;
    for (size_t i = 0; i < 256u * 256u; i++) {
        uint16_t height;
        memcpy(&height, height_map + i, sizeof height);
        if (height < minimum) minimum = height;
        if (height > maximum) maximum = height;
    }
    for (size_t y = 0; y < RADAR_TERRAIN_SIDE; y++) {
        for (size_t x = 0; x < RADAR_TERRAIN_SIDE; x++) {
            size_t terrain_index = (y * RADAR_TERRAIN_STRIDE + 2) * 256 +
                                   (x * RADAR_TERRAIN_STRIDE + 2);
            size_t sample_index = y * RADAR_TERRAIN_SIDE + x;
            memcpy(&radar_terrain_map.height[sample_index], height_map + terrain_index,
                   sizeof(uint16_t));
            radar_terrain_map.material[sample_index] = material_map[terrain_index * 2 + 1];
        }
    }
    radar_terrain_map.origin_x = terrain_position[0] + square_size * 2.0f;
    radar_terrain_map.origin_y = terrain_position[1] + square_size * 2.0f;
    radar_terrain_map.square_size = (float)square_size * RADAR_TERRAIN_STRIDE;
    radar_terrain_map.minimum = minimum;
    radar_terrain_map.maximum = maximum;
    radar_terrain_map.valid = 1;
    return 1;
}

static int radar_project(float dx, float dy, float forward_x, float forward_y,
                         float range, float cx, float cy, float radius,
                         struct radar_point *point) {
    if (!point || !isfinite(dx) || !isfinite(dy) || !isfinite(forward_x) ||
        !isfinite(forward_y) || !isfinite(range) || range <= 0.0f ||
        !isfinite(cx) || !isfinite(cy) || !isfinite(radius) || radius <= 0.0f) return 0;
    /* A forward-facing radar rotates its terrain and contacts with the view. */
    float nx = (dx * forward_y - dy * forward_x) / range;
    float ny = (dx * forward_x + dy * forward_y) / range;
    if (nx * nx + ny * ny > 1.0f) return 0;
    point->x = cx + nx * radius;
    point->y = cy - ny * radius;
    return isfinite(point->x) && isfinite(point->y);
}

static void radar_refresh_data(uint64_t now_ns) {
    if (radar_last_refresh_ns && now_ns >= radar_last_refresh_ns &&
        now_ns - radar_last_refresh_ns < 100000000ULL) return;
    radar_last_refresh_ns = now_ns;
    radar_marker_count = 0;
    radar_data_valid = 0;

    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory) ||
        !rv_memory_range(&memory, (void *)SHAZBOT_CONNECTION, 4, PROT_READ) ||
        !rv_memory_range(&memory, (void *)SHAZBOT_ID_DICTIONARY, 4, PROT_READ)) return;
    void *connection = rv_pointer((void *)SHAZBOT_CONNECTION, 0);
    void *dictionary = rv_pointer((void *)SHAZBOT_ID_DICTIONARY, 0);
    float origin[3], forward[2];
    uint32_t self_id;
    int local_team;
    if (!radar_control_view(&memory, connection, dictionary, origin, forward,
                            &self_id, &local_team)) return;
    memcpy(radar_origin, origin, sizeof radar_origin);
    radar_forward_x = forward[0];
    radar_forward_y = forward[1];
    (void)radar_capture_terrain(&memory, dictionary);

    /* Teammates are useful even when off-camera. In observer mode the control
       object has no team, so use all players in the visible set as contacts. */
    uint32_t visited = 0;
    for (size_t bucket = 0; local_team != RV_OTHER_TEAM && bucket < 4096 &&
         visited < 131072; bucket++) {
        void *object = rv_pointer(dictionary, bucket * 4);
        for (unsigned int chain = 0; object && chain < 4096 && visited++ < 131072; chain++) {
            if (!rv_memory_range(&memory, object, 0x24, PROT_READ)) break;
            void *next = rv_pointer(object, 0x10);
            const char *name = rv_class_name(&memory, object);
            uint32_t id = rv_word(object, 0x20);
            if (name && strcmp(name, "Player") == 0 && id && id != self_id &&
                rv_memory_range(&memory, object, 0x2d4, PROT_READ) &&
                (rv_word(object, 0x40) & 2) && rv_team(&memory, object) == local_team) {
                float position[3];
                if (rv_position(&memory, object, RV_PLAYER, position))
                    radar_add_world_marker(position[0] - origin[0],
                                          position[1] - origin[1], 0);
            }
            object = next;
        }
    }

    if (rv_memory_range(&memory, (void *)SHAZBOT_VISIBLE_SHAPEBASE_SET, 4, PROT_READ)) {
        void *visible = rv_pointer((void *)SHAZBOT_VISIBLE_SHAPEBASE_SET, 0);
        struct radar_marker enemies[RADAR_MARKER_LIMIT];
        size_t enemy_count = radar_collect_visible_enemies(&memory, visible, dictionary,
                                      self_id, local_team, origin, enemies,
                                      RADAR_MARKER_LIMIT - radar_marker_count);
        for (size_t i = 0; i < enemy_count; i++)
            radar_add_world_marker(enemies[i].dx, enemies[i].dy, enemies[i].kind);
    }

    /* The first assigned ClientTarget is the T2 client waypoint. */
    if (rv_memory_range(&memory, (void *)SHAZBOT_CLIENT_TARGET_LIST, 4, PROT_READ)) {
        void *targets = rv_pointer((void *)SHAZBOT_CLIENT_TARGET_LIST, 0);
        float zero[3] = {0, 0, 0}, delta[3], velocity[3];
        if (shazbot_assigned_task_delta(&memory, targets, dictionary, NULL,
                                         origin, zero, delta, velocity, NULL))
            radar_add_world_marker(delta[0], delta[1], 2);
    }
    radar_data_valid = 1;
}

static void radar_vertex(float x, float y) { radar_gl.Vertex2f(x, y); }

static void radar_draw_dot(float x, float y, float radius,
                           float r, float g, float b) {
    radar_gl.Color4f(r, g, b, 1.0f);
    radar_gl.Begin(RADAR_QUADS);
    radar_vertex(x - radius, y - radius); radar_vertex(x + radius, y - radius);
    radar_vertex(x + radius, y + radius); radar_vertex(x - radius, y + radius);
    radar_gl.End();
}

static void radar_draw_terrain(float cx, float cy, float radius) {
    if (!radar_terrain_map.valid || radar_terrain_map.square_size <= 0.0f) return;
    static const float material_colors[8][3] = {
        {0.13f, 0.31f, 0.16f}, {0.27f, 0.34f, 0.18f},
        {0.38f, 0.29f, 0.16f}, {0.36f, 0.35f, 0.24f},
        {0.22f, 0.30f, 0.28f}, {0.33f, 0.32f, 0.22f},
        {0.18f, 0.28f, 0.34f}, {0.38f, 0.38f, 0.35f},
    };
    float height_range = (float)radar_terrain_map.maximum - radar_terrain_map.minimum;
    if (height_range < 1.0f) height_range = 1.0f;
    float tile = radar_terrain_map.square_size * radius / RADAR_WORLD_RANGE;
    if (tile < 1.0f) tile = 1.0f;
    if (tile > 9.0f) tile = 9.0f;
    float edge = radius - tile * 0.75f;
    float edge2 = edge * edge;

    radar_gl.Begin(RADAR_QUADS);
    for (size_t y = 0; y < RADAR_TERRAIN_SIDE; y++) {
        for (size_t x = 0; x < RADAR_TERRAIN_SIDE; x++) {
            size_t index = y * RADAR_TERRAIN_SIDE + x;
            float world_x = radar_terrain_map.origin_x + x * radar_terrain_map.square_size;
            float world_y = radar_terrain_map.origin_y + y * radar_terrain_map.square_size;
            struct radar_point p;
            if (!radar_project(world_x - radar_origin[0], world_y - radar_origin[1],
                               radar_forward_x, radar_forward_y, RADAR_WORLD_RANGE,
                               cx, cy, radius, &p)) continue;
            float px = p.x - cx, py = p.y - cy;
            if (px * px + py * py > edge2) continue;

            float elevation = ((float)radar_terrain_map.height[index] -
                               radar_terrain_map.minimum) / height_range;
            if (elevation < 0.0f) elevation = 0.0f;
            if (elevation > 1.0f) elevation = 1.0f;
            float shade = 0.48f + elevation * 0.52f;
            const float *base = material_colors[radar_terrain_map.material[index] & 7];
            radar_gl.Color4f(base[0] * shade, base[1] * shade, base[2] * shade, 0.9f);
            radar_vertex(p.x - tile * 0.5f, p.y - tile * 0.5f);
            radar_vertex(p.x + tile * 0.5f, p.y - tile * 0.5f);
            radar_vertex(p.x + tile * 0.5f, p.y + tile * 0.5f);
            radar_vertex(p.x - tile * 0.5f, p.y + tile * 0.5f);
        }
    }
    radar_gl.End();
}

static void radar_draw_overlay(void) {
    if (!radar_console_enabled() || !radar_resolve_gl()) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return;
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    radar_refresh_data(now_ns);
    if (!radar_data_valid) return;

    int viewport[4], matrix_mode = RADAR_MODELVIEW;
    radar_gl.GetIntegerv(RADAR_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    radar_gl.GetIntegerv(RADAR_MATRIX_MODE, &matrix_mode);
    radar_gl.PushAttrib(RADAR_ALL_ATTRIB_BITS);
    radar_gl.MatrixMode(RADAR_PROJECTION); radar_gl.PushMatrix(); radar_gl.LoadIdentity();
    radar_gl.Ortho(0, viewport[2], viewport[3], 0, -1, 1);
    radar_gl.MatrixMode(RADAR_MODELVIEW); radar_gl.PushMatrix(); radar_gl.LoadIdentity();
    radar_gl.Disable(RADAR_TEXTURE_2D); radar_gl.Disable(RADAR_DEPTH_TEST);
    radar_gl.Disable(RADAR_CULL_FACE); radar_gl.Disable(RADAR_SCISSOR_TEST);
    radar_gl.Disable(RADAR_ALPHA_TEST); radar_gl.Enable(RADAR_BLEND);
    radar_gl.BlendFunc(RADAR_SRC_ALPHA, RADAR_ONE_MINUS_SRC_ALPHA);

    const float radius = 74.0f, cx = 104.0f, cy = (float)viewport[3] - 104.0f;
    static const float turn_cos = 0.9914448614f, turn_sin = 0.1305261922f;
    float circle_x = radius, circle_y = 0.0f;
    radar_gl.Color4f(0.015f, 0.05f, 0.035f, 0.78f);
    radar_gl.Begin(RADAR_TRIANGLE_FAN); radar_vertex(cx, cy);
    for (int i = 0; i <= 48; i++) {
        radar_vertex(cx + circle_x, cy + circle_y);
        float next_x = circle_x * turn_cos - circle_y * turn_sin;
        circle_y = circle_x * turn_sin + circle_y * turn_cos;
        circle_x = next_x;
    }
    radar_gl.End();
    radar_draw_terrain(cx, cy, radius);
    radar_gl.LineWidth(1.0f); radar_gl.Color4f(0.1f, 0.85f, 0.45f, 0.8f);
    for (int ring = 1; ring <= 2; ring++) {
        float rr = radius * ring / 2.0f;
        circle_x = rr; circle_y = 0.0f;
        radar_gl.Begin(RADAR_LINE_LOOP);
        for (int i = 0; i < 48; i++) {
            radar_vertex(cx + circle_x, cy + circle_y);
            float next_x = circle_x * turn_cos - circle_y * turn_sin;
            circle_y = circle_x * turn_sin + circle_y * turn_cos;
            circle_x = next_x;
        }
        radar_gl.End();
    }
    radar_gl.Begin(RADAR_LINES);
    radar_vertex(cx - radius, cy); radar_vertex(cx + radius, cy);
    radar_vertex(cx, cy - radius); radar_vertex(cx, cy + radius);
    radar_gl.End();
    radar_draw_dot(cx, cy, 3.0f, 1.0f, 1.0f, 1.0f);

    for (size_t i = 0; i < radar_marker_count; i++) {
        struct radar_marker *m = &radar_markers[i];
        struct radar_point point;
        if (!radar_project(m->dx, m->dy, radar_forward_x, radar_forward_y,
                           RADAR_WORLD_RANGE, cx, cy, radius, &point)) continue;
        if (m->kind == 0) radar_draw_dot(point.x, point.y, 2.5f, 0.25f, 0.65f, 1.0f);
        else if (m->kind == 1) radar_draw_dot(point.x, point.y, 3.0f, 1.0f, 0.25f, 0.15f);
        else radar_draw_dot(point.x, point.y, 3.5f, 1.0f, 0.9f, 0.1f);
    }

    radar_gl.MatrixMode(RADAR_MODELVIEW); radar_gl.PopMatrix();
    radar_gl.MatrixMode(RADAR_PROJECTION); radar_gl.PopMatrix();
    radar_gl.MatrixMode((unsigned)matrix_mode); radar_gl.PopAttrib();
}

/* Project a T2 Z-up world point through the exact matrices active while the
   game's sky was rendered. The horizon ring is at the camera eye's height. */
static int hud_demo_compass_project(const float view_projection[16], const float eye[3],
                                    const int camera_viewport[4], float screen_width,
                                    float screen_height,
                                    float bearing_degrees, float *screen_x, float *screen_y) {
    if (!view_projection || !eye || !camera_viewport || !screen_x || !screen_y ||
        !isfinite(bearing_degrees) || camera_viewport[2] <= 0 || camera_viewport[3] <= 0 ||
        !isfinite(screen_width) || !isfinite(screen_height) ||
        screen_width <= 0.0f || screen_height <= 0.0f) return 0;
    float sine, cosine;
    sky_sincos(bearing_degrees * (3.14159265358979323846f / 180.0f), &sine, &cosine);
    const float point[4] = {
        eye[0] + sine * 1000.0f,
        eye[1] + cosine * 1000.0f,
        eye[2],
        1.0f
    };
    float clip_x = 0.0f, clip_y = 0.0f, clip_w = 0.0f;
    for (unsigned int i = 0; i < 4; i++) {
        clip_x += view_projection[i] * point[i];
        clip_y += view_projection[4 + i] * point[i];
        clip_w += view_projection[12 + i] * point[i];
    }
    if (!isfinite(clip_x) || !isfinite(clip_y) || !isfinite(clip_w) || clip_w <= 1.0e-4f)
        return 0;
    float nx = clip_x / clip_w, ny = clip_y / clip_w;
    if (!isfinite(nx) || !isfinite(ny) || nx < -1.1f || nx > 1.1f) return 0;
    /* Scale NDC into the current overlay viewport, not the scene's render
       target size; this also follows a post-process render-scale resolve. */
    *screen_x = (nx + 1.0f) * 0.5f * screen_width;
    *screen_y = (1.0f - ny) * 0.5f * screen_height;
    return isfinite(*screen_x) && isfinite(*screen_y);
}

struct hud_demo_bitmap_texture {
    uint32_t gl_name;
    uint32_t texture_width, texture_height;
    uint32_t bitmap_width, bitmap_height;
};

static int hud_demo_bitmap_texture_get(const struct rv_memory *memory, void *dictionary,
                                      uint32_t id, struct hud_demo_bitmap_texture *texture) {
    if (!memory || !dictionary || !id || !texture) return 0;
    void *control = rv_find_object(memory, dictionary, id);
    if (!control || !rv_memory_range(memory, control, 0x20, PROT_READ)) return 0;
    const char *class_name = rv_class_name(memory, control);
    if (!class_name || strcmp(class_name, "HudBitmapCtrl") != 0) return 0;
    uint32_t bitmap_offset;
    if (!rv_persist_field_offset(memory, control, "bitmap", &bitmap_offset) ||
        bitmap_offset < sizeof(uint32_t) ||
        !rv_memory_range(memory, (unsigned char *)control + bitmap_offset, sizeof(uint32_t), PROT_READ))
        return 0;
    /* HudBitmapCtrl stores TextureHandle immediately before its registered
       bitmap StringTableEntry; TextureHandle is one native pointer. */
    void *handle = rv_pointer(control, bitmap_offset - sizeof(uint32_t));
    if (!handle || !rv_memory_range(memory, handle, 0x2c, PROT_READ)) return 0;
    texture->gl_name = rv_word(handle, 0x0c);
    texture->texture_width = rv_word(handle, 0x1c);
    texture->texture_height = rv_word(handle, 0x20);
    texture->bitmap_width = rv_word(handle, 0x24);
    texture->bitmap_height = rv_word(handle, 0x28);
    return texture->gl_name && texture->texture_width && texture->texture_height &&
           texture->bitmap_width && texture->bitmap_height;
}

static int hud_demo_compass_state(uint64_t now_ns) {
    if (!Con__evaluate || !core_runtime_installed) return 0;
    if (hud_demo_compass_last_state_ns && now_ns >= hud_demo_compass_last_state_ns &&
        now_ns - hud_demo_compass_last_state_ns < 50000000ULL) {
        return hud_demo_compass_cached_enabled;
    }
    hud_demo_compass_last_state_ns = now_ns;
    hud_demo_compass_cached_enabled = 0;
    const char *text = Con__evaluate("t2launch_hudDemoCompassEnabled();", 0, NULL, 0);
    hud_demo_compass_cached_enabled = text && text[0] == '1' && text[1] == 0;
    return hud_demo_compass_cached_enabled;
}

static void hud_demo_compass_draw_quad(float x, float y, float width, float height,
                                       const struct hud_demo_bitmap_texture *texture,
                                       float u0, float v0, float u1, float v1) {
    radar_gl.BindTexture(RADAR_TEXTURE_2D, texture->gl_name);
    radar_gl.Color4f(1.0f, 1.0f, 1.0f, 0.95f);
    radar_gl.Begin(RADAR_QUADS);
    radar_gl.TexCoord2f(u0, v0); radar_vertex(x - width * 0.5f, y - height * 0.5f);
    radar_gl.TexCoord2f(u1, v0); radar_vertex(x + width * 0.5f, y - height * 0.5f);
    radar_gl.TexCoord2f(u1, v1); radar_vertex(x + width * 0.5f, y + height * 0.5f);
    radar_gl.TexCoord2f(u0, v1); radar_vertex(x - width * 0.5f, y + height * 0.5f);
    radar_gl.End();
}

static void hud_demo_compass_draw(void) {
    if (target->dedicated || !hud_demo_compass_camera_valid) return;
    hud_demo_compass_camera_valid = 0;
    if (!radar_resolve_gl()) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return;
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    if (!hud_demo_compass_state(now_ns)) return;
    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory) ||
        !rv_memory_range(&memory, (void *)SHAZBOT_ID_DICTIONARY, sizeof(uint32_t), PROT_READ)) return;
    void *dictionary = rv_pointer((void *)SHAZBOT_ID_DICTIONARY, 0);
    struct hud_demo_bitmap_texture markers[RADAR_COMPASS_MARKER_COUNT];
    for (size_t i = 0; i < RADAR_COMPASS_MARKER_COUNT; i++)
        if (!hud_demo_bitmap_texture_get(&memory, dictionary,
            hud_demo_compass_marker_ids[i], &markers[i])) return;

    int viewport[4], matrix_mode = RADAR_MODELVIEW;
    radar_gl.GetIntegerv(RADAR_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0 ||
        hud_demo_compass_viewport[2] <= 0 || hud_demo_compass_viewport[3] <= 0) return;
    radar_gl.GetIntegerv(RADAR_MATRIX_MODE, &matrix_mode);
    radar_gl.PushAttrib(RADAR_ALL_ATTRIB_BITS);
    radar_gl.MatrixMode(RADAR_PROJECTION); radar_gl.PushMatrix(); radar_gl.LoadIdentity();
    radar_gl.Ortho(0, viewport[2], viewport[3], 0, -1, 1);
    radar_gl.MatrixMode(RADAR_MODELVIEW); radar_gl.PushMatrix(); radar_gl.LoadIdentity();
    radar_gl.Disable(RADAR_DEPTH_TEST); radar_gl.Disable(RADAR_CULL_FACE);
    radar_gl.Disable(RADAR_SCISSOR_TEST); radar_gl.Disable(RADAR_ALPHA_TEST);
    radar_gl.Enable(RADAR_BLEND); radar_gl.Enable(RADAR_TEXTURE_2D);
    radar_gl.BlendFunc(RADAR_SRC_ALPHA, RADAR_ONE_MINUS_SRC_ALPHA);
    radar_gl.TexEnvi(RADAR_TEXTURE_ENV, RADAR_TEXTURE_ENV_MODE, RADAR_MODULATE);

    float scale = (float)viewport[3] / 720.0f;
    if (scale < 1.0f) scale = 1.0f;
    radar_gl.LineWidth(1.0f);
    for (int degrees = 0; degrees < 360; degrees += 15) {
        float x, y;
        if (!hud_demo_compass_project(hud_demo_compass_view_projection, hud_demo_compass_eye,
                                     hud_demo_compass_viewport, (float)viewport[2],
                                     (float)viewport[3],
                                     (float)degrees, &x, &y)) continue;
        if (degrees % 90 == 0) {
            size_t index = (size_t)(degrees / 90);
            float marker_width = markers[index].bitmap_width * scale;
            float marker_height = markers[index].bitmap_height * scale;
            float u1 = (float)markers[index].bitmap_width / markers[index].texture_width;
            float v1 = (float)markers[index].bitmap_height / markers[index].texture_height;
            hud_demo_compass_draw_quad(x, y, marker_width, marker_height,
                                       &markers[index], 0.0f, 0.0f, u1, v1);
        } else {
            const struct hud_demo_bitmap_texture *strip = &markers[4];
            float half_width = degrees % 45 == 0 ? 7.0f : 1.5f;
            float source_center = 4.5f + (float)degrees;
            float u0 = (source_center - half_width) / (float)strip->texture_width;
            float u1 = (source_center + half_width) / (float)strip->texture_width;
            float v1 = (float)strip->bitmap_height / strip->texture_height;
            hud_demo_compass_draw_quad(x, y, 2.0f * half_width * scale,
                strip->bitmap_height * scale, strip, u0, 0.0f, u1, v1);
        }
    }
    radar_gl.BindTexture(RADAR_TEXTURE_2D, 0);
    radar_gl.MatrixMode(RADAR_MODELVIEW); radar_gl.PopMatrix();
    radar_gl.MatrixMode(RADAR_PROJECTION); radar_gl.PopMatrix();
    radar_gl.MatrixMode((unsigned)matrix_mode); radar_gl.PopAttrib();
}

/* The #25034 Sky::renderSkyBox entry at 0x08222390 reads the ten Sky::mPoints
   vertices beginning at this+0xd18, then submits the cube faces. Rotate those
   vertices only around that native call and restore the object immediately;
   cloud geometry, scene state, networked fields, and other Sky instances are
   otherwise untouched. Angle units match the T1 reference (radians/second). */
#define SKY_RENDER_SLICES_ADDRESS 0x08222390
#define SKY_POINTS_OFFSET 0x0d18
#define SKY_POINTS_BYTES (10 * 3 * sizeof(float))
static const unsigned char sky_render_slices_original[] = {
    0x55, 0x89, 0xe5, 0x81, 0xec, 0xac, 0x00, 0x00, 0x00
};
static const float sky_pi = 3.14159265358979323846f;
static const float sky_tau = 6.28318530717958647692f;

static void sky_sincos(float angle, float *sine, float *cosine) {
    while (angle > sky_pi) angle -= sky_tau;
    while (angle < -sky_pi) angle += sky_tau;
    float cosine_sign = 1.0f;
    if (angle > sky_pi * 0.5f) {
        angle = sky_pi - angle;
        cosine_sign = -1.0f;
    } else if (angle < -sky_pi * 0.5f) {
        angle = -sky_pi - angle;
        cosine_sign = -1.0f;
    }
    float x2 = angle * angle;
    *sine = angle * (1.0f + x2 * (-0.1666666716f + x2 *
            (0.0083333477f + x2 * (-0.0001984090f))));
    *cosine = cosine_sign *
        (1.0f + x2 * (-0.5f + x2 * (0.0416666418f + x2 * -0.0013888378f)));
}

static void sky_rotate_points(unsigned char *sky, float angle) {
    float sine, cosine;
    sky_sincos(angle, &sine, &cosine);
    float *points = (float *)(sky + SKY_POINTS_OFFSET);
    for (size_t i = 0; i < 10; i++) {
        float x = points[i * 3], y = points[i * 3 + 1];
        points[i * 3] = x * cosine - y * sine;
        points[i * 3 + 1] = x * sine + y * cosine;
    }
}

static int hud_demo_compass_compose_view_projection(const float gl_modelview[16],
                                                    const float gl_projection[16],
                                                    float view_projection[16], float eye[3]) {
    if (!gl_modelview || !gl_projection || !view_projection || !eye) return 0;
    float modelview[16], projection[16];
    /* GL matrices are column-major; store the equivalent row-major matrices
       used by the Torch compass projection formula. */
    for (unsigned int row = 0; row < 4; row++)
        for (unsigned int column = 0; column < 4; column++) {
            modelview[row * 4 + column] = gl_modelview[column * 4 + row];
            projection[row * 4 + column] = gl_projection[column * 4 + row];
        }
    for (unsigned int row = 0; row < 4; row++)
        for (unsigned int column = 0; column < 4; column++) {
            float value = 0.0f;
            for (unsigned int k = 0; k < 4; k++)
                value += projection[row * 4 + k] * modelview[k * 4 + column];
            view_projection[row * 4 + column] = value;
        }
    float tx = modelview[3], ty = modelview[7], tz = modelview[11];
    eye[0] = -(modelview[0] * tx + modelview[4] * ty + modelview[8] * tz);
    eye[1] = -(modelview[1] * tx + modelview[5] * ty + modelview[9] * tz);
    eye[2] = -(modelview[2] * tx + modelview[6] * ty + modelview[10] * tz);
    for (size_t i = 0; i < 16; i++)
        if (!isfinite(view_projection[i])) return 0;
    for (size_t i = 0; i < 3; i++)
        if (!isfinite(eye[i])) return 0;
    return 1;
}

static void sky_capture_compass_view(void) {
    hud_demo_compass_camera_valid = 0;
    if (!radar_resolve_gl()) return;
    float gl_modelview[16], gl_projection[16];
    int viewport[4];
    radar_gl.GetFloatv(RADAR_MODELVIEW_MATRIX, gl_modelview);
    radar_gl.GetFloatv(RADAR_PROJECTION_MATRIX, gl_projection);
    radar_gl.GetIntegerv(RADAR_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0 ||
        !hud_demo_compass_compose_view_projection(gl_modelview, gl_projection,
            hud_demo_compass_view_projection, hud_demo_compass_eye)) return;
    memcpy(hud_demo_compass_viewport, viewport, sizeof viewport);
    hud_demo_compass_camera_valid = 1;
}

static float sky_rotation_speed(void) {
    if (!Con__evaluate || !core_runtime_installed) return 0.0f;
    const char *text = Con__evaluate("t2launch_skyRotationSpeed();", 0, NULL, 0);
    if (!text || !text[0]) return 0.0f;
    char *end = NULL;
    float speed = strtof(text, &end);
    return end != text && !*end && isfinite(speed) && speed >= -20.0f && speed <= 20.0f
        ? speed : 0.0f;
}

static void sky_rotation_update(uint64_t now_ns, float speed) {
    if (!sky_rotation_last_ns || now_ns < sky_rotation_last_ns) {
        sky_rotation_last_ns = now_ns;
        return;
    }
    uint64_t elapsed_ns = now_ns - sky_rotation_last_ns;
    sky_rotation_last_ns = now_ns;
    if (elapsed_ns > 250000000ULL) elapsed_ns = 250000000ULL;
    sky_rotation_angle += speed * ((float)elapsed_ns / 1000000000.0f);
    while (sky_rotation_angle > sky_pi) sky_rotation_angle -= sky_tau;
    while (sky_rotation_angle < -sky_pi) sky_rotation_angle += sky_tau;
}

typedef void (*sky_render_slices_t)(void *sky, float lower_band_height, float upper_alpha);
static sky_render_slices_t sky_render_slices_original_fn;

static void sky_render_slices_wrapper(void *sky_object, float lower_band_height,
                                      float upper_alpha) {
    sky_capture_compass_view();
    float speed = sky_rotation_speed();
    if (speed == 0.0f) {
        sky_rotation_last_ns = 0;
        sky_render_slices_original_fn(sky_object, lower_band_height, upper_alpha);
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        sky_render_slices_original_fn(sky_object, lower_band_height, upper_alpha);
        return;
    }
    sky_rotation_update((uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec, speed);
    struct rv_memory memory;
    unsigned char saved[SKY_POINTS_BYTES];
    if (!sky_object || !rv_memory_snapshot(&memory) ||
        !rv_memory_range(&memory, (unsigned char *)sky_object + SKY_POINTS_OFFSET,
                         SKY_POINTS_BYTES, PROT_READ | PROT_WRITE)) {
        sky_render_slices_original_fn(sky_object, lower_band_height, upper_alpha);
        return;
    }
    memcpy(saved, (unsigned char *)sky_object + SKY_POINTS_OFFSET, sizeof saved);
    sky_rotate_points((unsigned char *)sky_object, sky_rotation_angle);
    sky_render_slices_original_fn(sky_object, lower_band_height, upper_alpha);
    memcpy((unsigned char *)sky_object + SKY_POINTS_OFFSET, saved, sizeof saved);
}

static int install_sky_rotation_hook(void) {
    if (sky_rotation_hook_attempted) return sky_rotation_hook_installed;
    sky_rotation_hook_attempted = 1;
    int protection;
    if (target != &game_targets[0] || target->dedicated ||
        !readable_range(SKY_RENDER_SLICES_ADDRESS, sizeof sky_render_slices_original,
                        &protection) || !(protection & PROT_EXEC) ||
        memcmp((void *)SKY_RENDER_SLICES_ADDRESS, sky_render_slices_original,
               sizeof sky_render_slices_original) != 0) {
        fprintf(stderr, "[t2_hook] Sky rotation refused: unsupported T2 Sky renderer\n");
        return 0;
    }
    unsigned char *trampoline = mmap(NULL, 32, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return 0;
    memcpy(trampoline, sky_render_slices_original, sizeof sky_render_slices_original);
    trampoline[sizeof sky_render_slices_original] = 0xe9;
    int32_t back = (int32_t)((SKY_RENDER_SLICES_ADDRESS + sizeof sky_render_slices_original) -
                            ((uintptr_t)trampoline + sizeof sky_render_slices_original + 5));
    memcpy(trampoline + sizeof sky_render_slices_original + 1, &back, sizeof back);
    __builtin___clear_cache((char *)trampoline,
                            (char *)trampoline + sizeof sky_render_slices_original + 5);
    if (mprotect(trampoline, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(trampoline, 32);
        return 0;
    }
    sky_rotation_trampoline = trampoline;
    sky_render_slices_original_fn = (sky_render_slices_t)trampoline;

    unsigned char replacement[sizeof sky_render_slices_original];
    replacement[0] = 0xe9;
    int32_t to_wrapper = (int32_t)((uintptr_t)sky_render_slices_wrapper -
                                   (SKY_RENDER_SLICES_ADDRESS + 5));
    memcpy(replacement + 1, &to_wrapper, sizeof to_wrapper);
    memset(replacement + 5, 0x90, sizeof replacement - 5);
    const struct code_patch patch = {SKY_RENDER_SLICES_ADDRESS,
        sky_render_slices_original, sizeof sky_render_slices_original, 0,
        replacement, sizeof replacement};
    if (!apply_code_patches("Sky rotation", &patch, 1)) {
        munmap(trampoline, 32);
        sky_rotation_trampoline = NULL;
        sky_render_slices_original_fn = NULL;
        return 0;
    }
    sky_rotation_hook_installed = 1;
    fprintf(stderr, "[t2_hook] T2 sky rotation hook installed; set pref::skyRotationSpeed (radians/sec)\n");
    return 1;
}

/* Run deferred TorqueScript changes after a rendered frame on the game thread. */
void SDL_GL_SwapBuffers(void) {
    if (!real_sdl_gl_swap_buffers)
        real_sdl_gl_swap_buffers = (sdl_gl_swap_buffers_t)next_symbol(
            "SDL_GL_SwapBuffers");
    if (!real_sdl_gl_swap_buffers) return;

    int on_game_thread = game_main_thread_known && pthread_equal(pthread_self(), game_main_thread);
    if (on_game_thread && rt.active) rt_present();
    else if (on_game_thread) apply_gamma_pass();
    if (on_game_thread) {
        radar_draw_overlay();
        hud_demo_compass_draw();
    }
    real_sdl_gl_swap_buffers();
    if (on_game_thread && rt.active) gl_rt.BindFramebuffer(GL_FRAMEBUFFER_, rt_draw_fbo());
    cap_frame_rate();
    if (!sdl_gl_swap_hook_reported) {
        sdl_gl_swap_hook_reported = 1;
        game_main_thread = pthread_self();
        game_main_thread_known = 1;
        fprintf(stderr, "[t2_hook] SDL_GL_SwapBuffers main-loop hook active\n");
    }
    if (!registered)
        register_hook_commands();
    if (on_game_thread) render_modifiers_tick();
    if (!registered || !Con__evaluate ||
        !game_main_thread_known ||
        !pthread_equal(pthread_self(), game_main_thread)) {
        if (hook_option_bool("telemetry.shazbot") && !shazbot_poll_gate_reported) {
            char message[192];
            snprintf(message, sizeof message,
                     "telemetry poll blocked before client polling (registered=%d, evaluator=%d, main_thread=%d, current_thread=%d)",
                     !!registered, Con__evaluate != NULL, !!game_main_thread_known,
                     game_main_thread_known && pthread_equal(pthread_self(), game_main_thread));
            shazbot_report_once(&shazbot_poll_gate_reported, message);
        }
        return;
    }

    if (hook_option_bool("telemetry.shazbot_actions") || shazbot_action_trigger_known)
        shazbot_poll_actions();
    poll_shazbot_telemetry();
    shazbot_poll_notifications();

    if (vsync_reapply) {
        vsync_reapply = 0;
        set_swap_interval(vsync_requested ? 1 : 0);
    }

    /* SDL 1.2 mode changes can leave stale damage on accelerated XWayland
       surfaces. A full Canvas repaint clears the same stale regions that a
       pointer-expose event otherwise repairs. Limit the extra work to the
       first two seconds after a mode change. */
    if (video_mode_repaint_frames > 0) {
        unsigned int frame = __sync_fetch_and_sub(&video_mode_repaint_frames, 1);
        if (frame % 6 == 1)
            Con__evaluate("if (isObject(Canvas)) Canvas.repaint();", 0, NULL, 0);
    }

    /* Torque redraws only dirty GUI regions and expects the back buffer to keep
       the previous frame; hardware drivers hand back undefined or older
       buffers, which shows as flicker and ghost cursors. Canvas.reset() marks
       the whole canvas dirty so the next frame is drawn in full. */
    static int full_redraw = -1;
    if (full_redraw < 0) {
        full_redraw = hook_option_bool("video.full_redraw");
        if (full_redraw) fprintf(stderr, "[t2_hook] Full canvas redraw every frame enabled\n");
    }
    if (full_redraw)
        Con__evaluate("if (isObject(Canvas)) Canvas.reset();", 0, NULL, 0);

    /* Named, newline-separated scripts let Torque report "<file> Line: N"
       instead of an unattributed "Syntax error in input." */
    if (!core_runtime_installed) {
        Con__evaluate(core_runtime_script, 0, "t2launch/core.cs", 0);
        core_runtime_installed = 1;
    }
    if (!target->dedicated && hook_option_bool("telemetry.shazbot") &&
        hook_option_bool("hook.console_commands") && !shazbot_hud_runtime_script_loaded) {
        Con__evaluate(shazbot_hud_telemetry_script, 0, "t2launch/shazbot_hud.cs", 0);
        shazbot_hud_runtime_script_loaded = 1;
        fprintf(stderr, "[t2_hook] Shazbot HUD weapon availability/selection tracking enabled\n");
    }
    /* The TribesNext handlers call the hook's console commands. */
    if (!tribesnext_runtime_installed) {
        tribesnext_runtime_installed = 1;
        if (hook_option_bool("hook.tribesnext") && hook_option_bool("hook.console_commands")) {
            Con__evaluate(tribesnext_runtime_script, 0, "t2launch/tribesnext.cs", 0);
            fprintf(stderr, "[t2_hook] Installed runtime TribesNext authentication handlers\n");
        }
    }

    time_t now = time(NULL);
    if (now != (time_t)-1 && hook_started_at && now - hook_started_at >= 2 &&
        !shell_audio_setup_done &&
        (!last_shell_audio_setup || now - last_shell_audio_setup >= 1)) {
        last_shell_audio_setup = now;
        /* Built-ins such as isObject() leave the evaluation's string result
           untouched; a script function's return value is reliable. */
        const char *gui_ready = Con__evaluate("t2launch_launchGuiReady();", 0, NULL, 0);
        if (gui_ready && strcmp(gui_ready, "1") == 0) {
            Con__evaluate("audioDetect();", 0, NULL, 0);
            Con__evaluate(
                "if (!$Audio::initialized && $pref::Audio::drivers $= \"OpenAL\") {"
                " $Audio::defaultDriver = \"native\";"
                " $Audio::initialized = audioSetDriver(\"OpenAL\");"
                " if ($Audio::initialized) $pref::Audio::activeDriver = \"OpenAL\";"
                "}",
                0, NULL, 0);
            if (hook_option_bool("audio.launch_reset"))
                Con__evaluate(
                    "if ($Audio::initialized && !$T2Launch::AudioResetDone) {"
                    " $T2Launch::AudioResetDone = 1; audioSetDriver(\"none\");"
                    " $Audio::initialized = audioSetDriver($pref::Audio::activeDriver);"
                    " if ($Audio::initialized) $HudHandle[shellScreen] = alxPlay(ShellScreenHumSound, 0, 0, 0);"
                    "}",
                    0, NULL, 0);
            if (!player_pane_override_installed && hook_option_bool("hook.tribes_tab")) {
                Con__evaluate(player_pane_button_click_override, 0, "t2launch/browser.cs", 0);
                player_pane_override_installed = 1;
                fprintf(stderr, "[t2_hook] Installed runtime PlayerPane TRIBES handler\n");
            }
            if (!net_options_override_installed && hook_option_bool("net.packet_limits")) {
                Con__evaluate(net_options_override, 0, "t2launch/network.cs", 0);
                net_options_override_installed = 1;
                fprintf(stderr, "[t2_hook] Installed runtime network options ranges 64/s and 1000 bytes\n");
            }
            if (!depot_downloads_installed && hook_option_bool("net.depot_downloads") &&
                hook_option_bool("hook.console_commands")) {
                Con__evaluate(depot_downloads_script, 0, "t2launch/depot.cs", 0);
                depot_downloads_installed = 1;
                fprintf(stderr, "[t2_hook] Installed runtime TribesNext depot downloads\n");
            }
            if (!discord_presence_installed && hook_option_bool("discord.presence")) {
                discord_presence_installed = 1;
                Con__evaluate(discord_presence_script, 0, "t2launch/discord.cs", 0);
                start_discord_presence();
            }
            if (!vsync_support_installed && hook_option_bool("video.vsync")) {
                Con__evaluate("$SwapIntervalSupported = 1;"
                              " setVerticalSync(!$pref::Video::disableVerticalSync);", 0, NULL, 0);
                vsync_support_installed = 1;
            }
            if (!filter_max_bots_override_installed && hook_option_bool("browser.many_bots")) {
                Con__evaluate(filter_max_bots_override, 0, "t2launch/filters.cs", 0);
                filter_max_bots_override_installed = 1;
                fprintf(stderr, "[t2_hook] Installed runtime filter Max Bots range 0-255\n");
            }
            /* A bare expression is not a TorqueScript statement; call a
               function so the evaluation returns the value. */
            const char *audio_state = Con__evaluate("t2launch_audioState();", 0, NULL, 0);
            fprintf(stderr, "[t2_hook] Audio state initialized|active|available: %s\n",
                    audio_state ? audio_state : "unavailable");
            /* Demo-review automation: once the shell is up, auto-start playback
               of a named demo, replicating StartSelectedDemo() (recordings.cs)
               without the UI list. Gated by env so it is inert otherwise. */
            const char *autoplay = getenv("T2LAUNCH_AUTOPLAY_DEMO");
            if (autoplay && *autoplay && !strchr(autoplay, '"')) {
                char script[768];
                Con__evaluate(autoplay_demo_exit_package, 0,
                              "t2launch/autoplay_demo_exit.cs", 0);
                shazbot_autoplay_demo_active = 1;
                snprintf(script, sizeof script,
                    "$T2Launch::AutoplayDemo = \"%s\";"
                    "function t2launch_autoplayDemo() {"
                    " if (playDemo($T2Launch::AutoplayDemo)) {"
                    "  alxEnableForceFeedback(false); resetGameState();"
                    "  Canvas.setContent(PlayGui); loadDemoSettings();"
                    "  $DemoPlaybackIndex = 5;"
                    "  echo(\"t2launch: autoplay demo started\");"
                    " } else { echo(\"t2launch: autoplay demo FAILED to load\"); }"
                    "}"
                    "schedule(1500, 0, \"t2launch_autoplayDemo\");",
                    autoplay);
                Con__evaluate(script, 0, "t2launch/autoplay.cs", 0);
                fprintf(stderr, "[t2_hook] Demo autoplay scheduled: %s\n", autoplay);
            }
            shell_audio_setup_done = 1;
        }
    }
    /* The audio driver restarts above (and the stock initAudio before them) leave voice capture disabled:
       alxIsEnabled("capture") is 0 although alxCaptureInit() returned 1, so push-to-talk showed "Recording"
       but never read the microphone. Initialise it again once, after the audio setup has settled. */
    if (shell_audio_setup_done && !voice_capture_fix_done && now != (time_t)-1 &&
        last_shell_audio_setup && now - last_shell_audio_setup >= 5) {
        voice_capture_fix_done = 1;
        Con__evaluate(
            "if ($Audio::initialized && $pref::Audio::enableVoiceCapture && !alxIsEnabled(\"capture\"))"
            " $Audio::captureInitialized = alxCaptureInit();",
            0, NULL, 0);
    }
    if (shell_audio_setup_done && now != (time_t)-1)
        poll_discord_presence(now);
    update_review_patches();
    update_review_model_diagnostic((review_applied_mask & review_group_mask(REVIEW_CORE)) != 0);
}

/* The dedicated server never swaps buffers. Platform::process (0x081CF400)
   ends each main-loop pass with usleep(1000), so the hook's main-thread work
   runs there instead. */
static void server_tick(void) {
    static int core_installed, tribesnext_installed;
    static time_t last_ready_check;
    if (!registered)
        register_hook_commands();
    render_modifiers_tick();
    if (!registered || !hook_option_bool("hook.console_commands")) return;
    if (!core_installed) {
        Con__evaluate(server_core_script, 0, "t2launch/server.cs", 0);
        core_installed = 1;
    }
    time_t now = time(NULL);
    if (tribesnext_installed || now == last_ready_check) return;
    last_ready_check = now;
    /* The package wraps GameConnection methods from the stock server scripts,
       so wait for them; ServerGroup exists once the server has started. */
    const char *ready = Con__evaluate("t2launch_serverReady();", 0, NULL, 0);
    if (!ready || strcmp(ready, "1") != 0) return;
    tribesnext_installed = 1;
    if (!hook_option_bool("server.tribesnext_auth")) return;
    Con__evaluate(server_tribesnext_script, 0, "t2launch/tribesnext_server.cs", 0);
    fprintf(stderr, "[t2_hook] Installed runtime TribesNext (T2CSRI) server authentication\n");
}

int usleep(useconds_t microseconds) {
    static int (*real_usleep)(useconds_t);
    if (!real_usleep) real_usleep = (int (*)(useconds_t))next_symbol("usleep");
    if (target->dedicated && game_main_thread_known && pthread_equal(pthread_self(), game_main_thread))
        server_tick();
    return real_usleep ? real_usleep(microseconds) : 0;
}

/* Observe SDL 1.2 mode changes without replacing its return surface or flags. */
/* ── Windowed position across fullscreen toggles ─────────────────────────
 * SDL 1.2.11 (the bundled one) re-maps its windowed window after a
 * fullscreen without remembering where it was, so the window manager places
 * it, which on a multi-monitor desktop can be another monitor. Remember the
 * window's frame position when leaving windowed mode and put it back on
 * return: as SDL_VIDEO_WINDOW_POS (SDL reads it as a USPosition hint) and
 * with XMoveWindow in case the window stayed mapped. */
typedef struct {
    unsigned char major, minor, patch;
    int subsystem;
    void *display;
    unsigned long window;
    void (*lock_func)(void);
    void (*unlock_func)(void);
    unsigned long fswindow, wmwindow;
    unsigned char spare[64];  /* later SDL versions add fields */
} sdl_syswm_info_t;

static int windowed_pos_saved, windowed_x, windowed_y;

static void window_pos_text(int x, int y, char *buffer, size_t size) {
    snprintf(buffer, size, "%d,%d", x, y);
}

/* SDL's X display and windowed (WM-managed) window, or 0. */
static int sdl_wm_window(sdl_syswm_info_t *info) {
    static int (*get_wm_info)(sdl_syswm_info_t *);
    if (!get_wm_info) *(void **)&get_wm_info = dlsym(RTLD_DEFAULT, "SDL_GetWMInfo");
    if (!get_wm_info) return 0;
    memset(info, 0, sizeof(*info));
    info->major = 1; info->minor = 2; info->patch = 0;
    return get_wm_info(info) == 1 && info->display && info->wmwindow;
}

static struct {
    int state;
    int (*QueryTree)(void *, unsigned long, unsigned long *, unsigned long *, unsigned long **, unsigned *);
    int (*GetWindowAttributes)(void *, unsigned long, void *);
    int (*MoveWindow)(void *, unsigned long, int, int);
    int (*Flush)(void *);
    int (*Free)(void *);
} xlib;

static int resolve_xlib(void) {
    if (xlib.state) return xlib.state > 0;
    xlib.state = -1;
    *(void **)&xlib.QueryTree = dlsym(RTLD_DEFAULT, "XQueryTree");
    *(void **)&xlib.GetWindowAttributes = dlsym(RTLD_DEFAULT, "XGetWindowAttributes");
    *(void **)&xlib.MoveWindow = dlsym(RTLD_DEFAULT, "XMoveWindow");
    *(void **)&xlib.Flush = dlsym(RTLD_DEFAULT, "XFlush");
    *(void **)&xlib.Free = dlsym(RTLD_DEFAULT, "XFree");
    if (!xlib.QueryTree || !xlib.GetWindowAttributes || !xlib.MoveWindow || !xlib.Flush || !xlib.Free)
        return 0;
    xlib.state = 1;
    return 1;
}

/* Remember where the windowed window's frame (its ancestor just below the
   root) sits on the desktop. */
static void save_windowed_position(void) {
    sdl_syswm_info_t info;
    if (!sdl_wm_window(&info) || !resolve_xlib()) return;
    if (info.lock_func) info.lock_func();
    unsigned long window = info.wmwindow, root = 0, parent = 0, *children = NULL;
    unsigned count = 0;
    for (int depth = 0; depth < 8; depth++) {
        if (!xlib.QueryTree(info.display, window, &root, &parent, &children, &count)) {
            parent = 0;
            break;
        }
        if (children) xlib.Free(children);
        children = NULL;
        if (!parent || parent == root) break;
        window = parent;
    }
    /* XWindowAttributes starts with int x, y; leave room for the rest. */
    int attributes[64] = {0};
    if (parent == root && root && xlib.GetWindowAttributes(info.display, window, attributes)) {
        windowed_x = attributes[0];
        windowed_y = attributes[1];
        windowed_pos_saved = 1;
    }
    if (info.unlock_func) info.unlock_func();
}

static void move_windowed_window(void) {
    sdl_syswm_info_t info;
    if (!sdl_wm_window(&info) || !resolve_xlib()) return;
    if (info.lock_func) info.lock_func();
    xlib.MoveWindow(info.display, info.wmwindow, windowed_x, windowed_y);
    xlib.Flush(info.display);
    if (info.unlock_func) info.unlock_func();
    fprintf(stderr, "[t2_hook] Windowed position restored to %d,%d\n", windowed_x, windowed_y);
}

void *SDL_SetVideoMode(int width, int height, int bpp, uint32_t flags) {
    static int have_mode;
    static uint32_t last_flags;
    if (!real_sdl_set_video_mode)
        real_sdl_set_video_mode = (sdl_set_video_mode_t)next_symbol(
            "SDL_SetVideoMode");
    if (!real_sdl_set_video_mode) return NULL;
    if (!(flags & SDL_FULLSCREEN_FLAG) && hook_option_bool("video.borderless"))
        flags |= SDL_NOFRAME_FLAG;
    /* video.render_scale: a fullscreen below the native size opens at the
       native size, and the game's frame is scaled into it at swap. */
    int real_w = width, real_h = height, scale = 0;
    if ((flags & SDL_OPENGL_FLAG) && (flags & SDL_FULLSCREEN_FLAG) && render_scale_mode() &&
        width > 0 && height > 0) {
        int native_w, native_h;
        if (native_fullscreen_size(&native_w, &native_h) && (native_w > width || native_h > height)) {
            real_w = native_w;
            real_h = native_h;
            scale = render_scale_mode();
        }
    }
    int was_fullscreen = have_mode && (last_flags & SDL_FULLSCREEN_FLAG);
    int to_fullscreen = (flags & SDL_FULLSCREEN_FLAG) != 0;
    if (have_mode && !was_fullscreen && to_fullscreen) save_windowed_position();
    int restore = was_fullscreen && !to_fullscreen && windowed_pos_saved;
    char previous_pos[64] = "", position[32];
    int had_pos = 0;
    if (restore) {
        const char *current = getenv("SDL_VIDEO_WINDOW_POS");
        if (current) {
            had_pos = 1;
            snprintf(previous_pos, sizeof(previous_pos), "%s", current);
        }
        window_pos_text(windowed_x, windowed_y, position, sizeof(position));
        setenv("SDL_VIDEO_WINDOW_POS", position, 1);
    }
    rt_teardown();
    void *surface = real_sdl_set_video_mode(real_w, real_h, bpp, flags);
    if (!surface && scale) {  /* the native mode was refused: do what the game asked */
        real_w = width;
        real_h = height;
        scale = 0;
        surface = real_sdl_set_video_mode(width, height, bpp, flags);
    }
    if (restore) {
        if (had_pos) setenv("SDL_VIDEO_WINDOW_POS", previous_pos, 1);
        else unsetenv("SDL_VIDEO_WINDOW_POS");
        if (surface) move_windowed_window();
    }
    if (surface) {
        have_mode = 1;
        last_flags = flags;
    }
    if (surface && vsync_requested >= 0) vsync_reapply = 1;
    if (surface) {
        /* A new context needs a new texture; the old one went with its context. */
        gamma_width = real_w;
        gamma_height = real_h;
        gl_gamma.texture = 0;
        gl_gamma.texture_width = gl_gamma.texture_height = 0;
        if (gl_gamma.state > 0) gl_gamma.state = 0;
        if (gl_rt.state > 0) gl_rt.state = 0;
        if ((flags & SDL_OPENGL_FLAG) && render_target_wanted())
            rt_setup(width, height, real_w, real_h, scale, msaa_samples());
    }
    if (surface && width > 0 && height > 0) {
        __sync_lock_test_and_set(&video_mode_repaint_frames, 120);
        fprintf(stderr, "[t2_hook] Queued full-canvas refresh after video mode %dx%d\n",
                width, height);
    }
    return surface;
}

/* ── SHA1 ────────────────────────────────────────────────────────────── */

static uint32_t rol32(uint32_t v, int n) { return n ? (v << n) | (v >> (32 - n)) : v; }

static void sha1(const uint8_t *data, int len, char out[41]) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    uint8_t blk[64];
    int pos = 0, t;
    uint64_t bits = (uint64_t)len * 8;
    uint32_t w[80],a,b,c,d,e,f,k,tmp;

    while (len - pos >= 64) {
        for (t=0;t<16;t++) w[t]=((uint32_t)data[pos+t*4]<<24)|((uint32_t)data[pos+t*4+1]<<16)|((uint32_t)data[pos+t*4+2]<<8)|data[pos+t*4+3];
        for (t=16;t<80;t++) w[t]=rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];
        for (t=0;t<80;t++){
            if(t<20){f=(b&c)|((~b)&d);k=0x5A827999;}
            else if(t<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(t<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else{f=b^c^d;k=0xCA62C1D6;}
            tmp=rol32(a,5)+f+e+k+w[t];e=d;d=c;c=rol32(b,30);b=a;a=tmp;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;pos+=64;
    }
    memset(blk,0,64);memcpy(blk,data+pos,len-pos);blk[len-pos]=0x80;
    if(len-pos>=56){
        for (t=0;t<16;t++) w[t]=((uint32_t)blk[t*4]<<24)|((uint32_t)blk[t*4+1]<<16)|((uint32_t)blk[t*4+2]<<8)|blk[t*4+3];
        for (t=16;t<80;t++) w[t]=rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
        a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];
        for (t=0;t<80;t++){
            if(t<20){f=(b&c)|((~b)&d);k=0x5A827999;}
            else if(t<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(t<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else{f=b^c^d;k=0xCA62C1D6;}
            tmp=rol32(a,5)+f+e+k+w[t];e=d;d=c;c=rol32(b,30);b=a;a=tmp;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
        memset(blk,0,64);
    }
    for(t=0;t<8;t++) blk[56+t]=(bits>>(56-t*8))&0xFF;
    for (t=0;t<16;t++) w[t]=((uint32_t)blk[t*4]<<24)|((uint32_t)blk[t*4+1]<<16)|((uint32_t)blk[t*4+2]<<8)|blk[t*4+3];
    for (t=16;t<80;t++) w[t]=rol32(w[t-3]^w[t-8]^w[t-14]^w[t-16],1);
    a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];
    for (t=0;t<80;t++){
        if(t<20){f=(b&c)|((~b)&d);k=0x5A827999;}
        else if(t<40){f=b^c^d;k=0x6ED9EBA1;}
        else if(t<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
        else{f=b^c^d;k=0xCA62C1D6;}
        tmp=rol32(a,5)+f+e+k+w[t];e=d;d=c;c=rol32(b,30);b=a;a=tmp;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    for(t=0;t<5;t++) sprintf(out+t*8,"%08x",h[t]);
}

/* Streaming SHA-256 for identifying the executable that owns the addresses a
   script wants to patch. This is an identity check, not a signature or trust
   boundary; writes are separately gated by hook.mem_patch. */
struct sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t used;
};

static uint32_t sha256_rotr(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32 - count));
}

static void sha256_transform(struct sha256_context *context, const uint8_t block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t words[64];
    for (unsigned int i = 0; i < 16; i++)
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (unsigned int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(words[i - 15], 7) ^ sha256_rotr(words[i - 15], 18) ^
                      (words[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(words[i - 2], 17) ^ sha256_rotr(words[i - 2], 19) ^
                      (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = context->state[0], b = context->state[1];
    uint32_t c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5];
    uint32_t g = context->state[6], h = context->state[7];
    for (unsigned int i = 0; i < 64; i++) {
        uint32_t sum1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + sum1 + choice + constants[i] + words[i];
        uint32_t sum0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c;
    context->state[3] += d; context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}

static void sha256_init(struct sha256_context *context) {
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(context->state, initial, sizeof initial);
    context->bit_count = 0;
    context->used = 0;
}

static void sha256_update(struct sha256_context *context, const uint8_t *data, size_t length) {
    context->bit_count += (uint64_t)length * 8;
    while (length) {
        size_t available = sizeof context->block - context->used;
        size_t copied = length < available ? length : available;
        memcpy(context->block + context->used, data, copied);
        context->used += copied;
        data += copied;
        length -= copied;
        if (context->used == sizeof context->block) {
            sha256_transform(context, context->block);
            context->used = 0;
        }
    }
}

static void sha256_final(struct sha256_context *context, char output[65]) {
    uint64_t bits = context->bit_count;
    context->block[context->used++] = 0x80;
    if (context->used > 56) {
        memset(context->block + context->used, 0, 64 - context->used);
        sha256_transform(context, context->block);
        context->used = 0;
    }
    memset(context->block + context->used, 0, 56 - context->used);
    for (unsigned int i = 0; i < 8; i++)
        context->block[63 - i] = (uint8_t)(bits >> (i * 8));
    sha256_transform(context, context->block);
    for (unsigned int i = 0; i < 8; i++)
        snprintf(output + i * 8, 9, "%08x", context->state[i]);
    output[64] = 0;
}

static char mem_patch_executable_sha256[65];
static int mem_patch_executable_sha256_state;

/* Hash the running image via /proc/self/exe so argv[0], symlinks, and the
   current working directory cannot select a different binary. Cache success
   or failure for this process; the executable cannot change under it. */
static const char *mem_patch_executable_hash(void) {
    if (mem_patch_executable_sha256_state > 0) return mem_patch_executable_sha256;
    if (mem_patch_executable_sha256_state < 0) return NULL;
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) goto failed;
    struct sha256_context context;
    sha256_init(&context);
    uint8_t buffer[16384];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            close(fd);
            goto failed;
        }
        sha256_update(&context, buffer, (size_t)count);
    }
    close(fd);
    sha256_final(&context, mem_patch_executable_sha256);
    mem_patch_executable_sha256_state = 1;
    return mem_patch_executable_sha256;
failed:
    mem_patch_executable_sha256_state = -1;
    return NULL;
}

/* ── Big Number ops for RSA (kept minimal to avoid stack issues) ────── */
/* Full bn_ functions removed - using inline code in callback */

/* ── Console Callbacks ──────────────────────────────────────────────── */

static const char* hook_test_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    return "ok_from_c";
}

static const char* shazbot_command_label_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (!hook_option_bool("telemetry.shazbot_command_labels") ||
        !shazbot_command_label_capture_active) return "disabled";
    if (argc != 3) return "usage: shazbot_command_label <id> <pressed>";
    char *end = NULL;
    long command = strtol(argv[1], &end, 10);
    if (!end || *end || command < 0 || command >= SHAZBOT_COMMAND_ACTIONS)
        return "invalid command id";
    end = NULL;
    long pressed = strtol(argv[2], &end, 10);
    if (!end || *end || (pressed != 0 && pressed != 1)) return "pressed must be 0 or 1";
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return "clock unavailable";
    uint64_t timestamp = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    fprintf(stderr, "[t2_hook] Shazbot: command label monotonic_ns=%llu id=%ld pressed=%ld\n",
            (unsigned long long)timestamp, command, pressed);
    return "ok";
}

static const char* shazbot_test_weapon_slot_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (!hook_option_bool("telemetry.shazbot_auto_weapon_slot_test") ||
        !hook_option_bool("telemetry.shazbot_command_labels") ||
        !shazbot_command_label_capture_active) return "disabled";
    if (argc != 2) return "usage: shazbot_test_weapon_slot <slot-id-0-to-5>";
    char *end = NULL;
    long slot = strtol(argv[1], &end, 10);
    if (!end || *end || slot < 0 || slot >= 6) return "slot must be in 0..5";
    if (!Con__evaluate || !shazbot_command_scripts[slot]) return "console unavailable";
    char slot_text[8];
    (void)snprintf(slot_text, sizeof slot_text, "%ld", slot);
    const char *label_arguments[] = {"shazbot_command_label", slot_text, "1"};
    const char *label_result = shazbot_command_label_cb(NULL, 3, label_arguments);
    if (!label_result || strcmp(label_result, "ok") != 0) return "command label unavailable";
    (void)Con__evaluate(shazbot_command_scripts[slot], 0,
                        "t2launch/shazbot_weapon_slot_test.cs", 0);
    fprintf(stderr, "[t2_hook] Shazbot scripted weapon-slot command id=%ld name=%s\n",
            slot, shazbot_command_names[slot]);
    return "ok";
}

static const char *shazbot_hud_weapon_clear_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argv;
    if (argc != 1) return "usage: shazbot_hud_weapon_clear()";
    shazbot_hud_weapon_clear_state();
    return "ok";
}

static const char *shazbot_hud_weapon_item_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc != 4) return "usage: shazbot_hud_weapon_item(hudSlot, name, addItem)";
    char *end = NULL;
    long hud_slot = strtol(argv[1], &end, 10);
    if (!end || *end || hud_slot < -1 || hud_slot > 255) return "invalid HUD slot";
    end = NULL;
    long add_item = strtol(argv[3], &end, 10);
    if (!end || *end || (add_item != 0 && add_item != 1)) return "addItem must be 0 or 1";
    shazbot_hud_weapon_update_state((int)hud_slot, argv[2], (int)add_item);
    return "ok";
}

static const char *shazbot_hud_weapon_active_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc != 2) return "usage: shazbot_hud_weapon_active(name)";
    shazbot_hud_weapon_active_state(argv[1]);
    return "ok";
}

static const char* shazbot_demo_record_start_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    if (!hook_option_bool("telemetry.shazbot_command_labels")) return "disabled";
    shazbot_command_label_capture_active = 1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return "clock unavailable";
    uint64_t timestamp = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    fprintf(stderr, "[t2_hook] Shazbot: demo recording start monotonic_ns=%llu\n",
            (unsigned long long)timestamp);
    return "ok";
}

static const char* shazbot_demo_record_stop_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    if (!hook_option_bool("telemetry.shazbot_command_labels")) return "disabled";
    shazbot_command_label_capture_active = 0;
    return "ok";
}

static const char* shazbot_command_capture_start_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    if (!hook_option_bool("telemetry.shazbot_command_labels")) return "disabled";
    shazbot_command_label_capture_active = 1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return "clock unavailable";
    uint64_t timestamp = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    fprintf(stderr, "[t2_hook] Shazbot: command capture start monotonic_ns=%llu\n",
            (unsigned long long)timestamp);
    return "ok";
}

static const char* shazbot_command_capture_stop_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    if (!hook_option_bool("telemetry.shazbot_command_labels")) return "disabled";
    shazbot_command_label_capture_active = 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0) {
        uint64_t timestamp = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
        fprintf(stderr, "[t2_hook] Shazbot: command capture stop monotonic_ns=%llu\n",
                (unsigned long long)timestamp);
    }
    return "ok";
}

static const char* hook_profile_browser_wake_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    if (!profile_browser_wake_seen) {
        profile_browser_wake_seen = 1;
        fprintf(stderr, "[t2_hook] Runtime Community Browser hooks confirmed active\n");
    }
    return "ok";
}

static const char* hook_sha1_cb(void *obj, S32 argc, const char **argv) {
    static char hex[41];
    (void)obj; (void)argc;
    if (argc < 2) return "usage: hook_sha1 <string>";
    sha1((const uint8_t *)argv[1], strlen(argv[1]), hex);
    return hex;
}

static int run_rsa_helper(const char *base, const char *exp, const char *mod,
                          char *result, size_t result_size) {
    int input_pipe[2], output_pipe[2];
    char helper[4096];
    const char *configured = getenv("T2LAUNCH_RSA_HELPER");
    if (configured && configured[0] && strlen(configured) < sizeof(helper))
        snprintf(helper, sizeof(helper), "%s", configured);
    else
        game_path(helper, sizeof(helper), "rsa_helper");
    if (!helper[0] || access(helper, X_OK) != 0)
        return 0;
    if (pipe(input_pipe) != 0) return 0;
    if (pipe(output_pipe) != 0) {
        close(input_pipe[0]); close(input_pipe[1]);
        return 0;
    }
    pid_t child = fork();
    if (child == 0) {
        close(input_pipe[1]); close(output_pipe[0]);
        dup2(input_pipe[0], STDIN_FILENO);
        dup2(output_pipe[1], STDOUT_FILENO);
        close(input_pipe[0]); close(output_pipe[1]);
        char *args[] = {helper, "--stdin", NULL};
        execv(args[0], args);
        _exit(1);
    }
    close(input_pipe[0]); close(output_pipe[1]);
    if (child < 0) {
        close(input_pipe[1]); close(output_pipe[0]);
        return 0;
    }
    char request[16384];
    int request_len = snprintf(request, sizeof(request), "%s\n%s\n%s\n", base, exp, mod);
    int write_ok = request_len > 0 && (size_t)request_len < sizeof(request);
    int written = 0;
    while (write_ok && written < request_len) {
        ssize_t count = write(input_pipe[1], request + written, request_len - written);
        if (count <= 0) { write_ok = 0; break; }
        written += (int)count;
    }
    close(input_pipe[1]);
    ssize_t length = read(output_pipe[0], result, result_size - 1);
    close(output_pipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (!write_ok || length <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        result[0] = 0;
        return 0;
    }
    result[length] = 0;
    result[strcspn(result, "\r\n")] = 0;
    for (int i = 0; result[i]; i++)
        if (result[i] >= 'A' && result[i] <= 'F') result[i] += 32;
    return 1;
}

static const char* hook_rsa_mod_exp_cb(void *obj, S32 argc, const char **argv) {
    static char result[2048];
    (void)obj;
    if (argc < 4) return "usage";
    if (!run_rsa_helper(argv[1], argv[2], argv[3], result, sizeof(result)))
        return "ERR:rsa";
    return result;
}

static const char* hook_rand64_cb(void *obj, S32 argc, const char **argv) {
    static char buf[17];
    (void)obj; (void)argc; (void)argv;
    uint64_t val;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&val, sizeof(val), 1, f) != 1) {
            srand((unsigned)time(NULL) ^ (unsigned)getpid());
            val = ((uint64_t)rand() << 32) | (uint64_t)rand();
        }
        fclose(f);
    } else {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        val = ((uint64_t)rand() << 32) | (uint64_t)rand();
    }
    sprintf(buf, "%llx", (unsigned long long)val);
    return buf;
}

static int hex2nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#define MEM_PATCH_MAX_BYTES 4096
#define MEM_PATCH_MAX_PAGES 16

static const char *mem_patch_mode(void) {
    const char *mode = hook_option_str("hook.mem_patch");
    if (mode && (strcmp(mode, "off") == 0 || strcmp(mode, "warn") == 0 ||
                 strcmp(mode, "enabled") == 0)) return mode;
    return "warn";  /* malformed or unknown values never enable writes */
}

static void mem_patch_message(const char *reason, const char *digest) {
    char message[384], script[512];
    if (digest)
        snprintf(message, sizeof message, "T2Launch memPatch: %s Current executable SHA-256: sha256:%s",
                 reason, digest);
    else
        snprintf(message, sizeof message, "T2Launch memPatch: %s Current executable SHA-256 unavailable",
                 reason);
    fprintf(stderr, "[t2_hook] %s\n", message);
    /* reason and digest are internal constants/hex output, never script input. */
    if (Con__evaluate && snprintf(script, sizeof script, "echo(\"%s\");", message) < (int)sizeof script)
        (void)Con__evaluate(script, 0, "t2launch/memPatch.cs", 0);
}

static int mem_patch_parse_address(const char *text, uintptr_t *address) {
    if (!text || !*text || !address) return 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (!*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (!isxdigit(*p)) return 0;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 16);
    if (errno == ERANGE || !end || *end || value > UINTPTR_MAX) return 0;
    *address = (uintptr_t)value;
    return 1;
}

/* Preserve TribesNEXT's old hex-pair convention: an odd leading nibble is
   padded on the left ("ABC" -> 0A BC). */
static int mem_patch_parse_bytes(const char *text, unsigned char *bytes,
                                 size_t capacity, size_t *length) {
    if (!text || !bytes || !length) return 0;
    size_t digits = strlen(text);
    if (!digits || digits > MEM_PATCH_MAX_BYTES * 2 || (digits + 1) / 2 > capacity) return 0;
    size_t at = 0, output = 0;
    if (digits & 1) {
        int nibble = hex2nibble(text[at++]);
        if (nibble < 0) return 0;
        bytes[output++] = (unsigned char)nibble;
    }
    while (at < digits) {
        int high = hex2nibble(text[at++]);
        int low = hex2nibble(text[at++]);
        if (high < 0 || low < 0) return 0;
        bytes[output++] = (unsigned char)((high << 4) | low);
    }
    *length = output;
    return output > 0;
}

static int mem_patch_checksum_matches(const char *supplied, const char *actual) {
    if (!supplied || !actual) return 0;
    if (strncasecmp(supplied, "sha256:", 7) == 0) supplied += 7;
    if (strlen(supplied) != 64) return 0;
    for (size_t i = 0; i < 64; i++) {
        if (hex2nibble(supplied[i]) < 0 || tolower((unsigned char)supplied[i]) != actual[i]) return 0;
    }
    return 1;
}

static int mem_patch_write(uintptr_t address, const unsigned char *bytes, size_t length,
                           const char **failure) {
    if (!bytes || !length || length > MEM_PATCH_MAX_BYTES || address > UINTPTR_MAX - length) {
        if (failure) *failure = "invalid patch range or length";
        return 0;
    }
    int protection = 0;
    if (!readable_range(address, length, &protection)) {
        if (failure) *failure = "target is not in a readable memory mapping";
        return 0;
    }
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((unsigned long)page_size & ((unsigned long)page_size - 1)) != 0) {
        if (failure) *failure = "invalid system page size";
        return 0;
    }
    uintptr_t page_mask = (uintptr_t)page_size - 1;
    uintptr_t first_page = address & ~page_mask;
    uintptr_t last_page = (address + length - 1) & ~page_mask;
    size_t page_count = (size_t)((last_page - first_page) / (uintptr_t)page_size) + 1;
    if (page_count > MEM_PATCH_MAX_PAGES) {
        if (failure) *failure = "patch spans too many pages";
        return 0;
    }
    uintptr_t pages[MEM_PATCH_MAX_PAGES];
    size_t changed = 0;
    for (size_t i = 0; i < page_count; i++) {
        pages[i] = first_page + i * (uintptr_t)page_size;
        if (mprotect((void *)pages[i], (size_t)page_size, protection | PROT_WRITE) != 0) {
            for (size_t restore = changed; restore > 0; restore--)
                (void)mprotect((void *)pages[restore - 1], (size_t)page_size, protection);
            if (failure) *failure = "could not make target page writable";
            return 0;
        }
        changed++;
    }
    volatile unsigned char *destination = (volatile unsigned char *)address;
    for (size_t i = 0; i < length; i++) destination[i] = bytes[i];
    __builtin___clear_cache((char *)address, (char *)(address + length));
    int restored = 1;
    for (size_t i = page_count; i > 0; i--)
        if (mprotect((void *)pages[i - 1], (size_t)page_size, protection) != 0) restored = 0;
    if (!restored) {
        if (failure) *failure = "patch applied, but original page permissions could not be restored";
        return -1;
    }
    return 1;
}

static const char *mem_patch_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    const char *mode = mem_patch_mode();
    if (strcmp(mode, "off") == 0) return "";

    const char *digest = mem_patch_executable_hash();
    if (strcmp(mode, "warn") == 0) {
        mem_patch_message(argc < 4 ? "legacy two-argument call refused; add the third SHA-256 argument."
                                   : "warn-only mode; memory patch refused.", digest);
        return "warn-only";
    }
    if (argc < 4 || !argv[1] || !argv[2] || !argv[3]) {
        mem_patch_message("legacy two-argument call refused; add the third SHA-256 argument.", digest);
        return "checksum required";
    }
    if (argc != 4) {
        mem_patch_message("expected address, patch bytes, and one SHA-256 argument; patch refused.", digest);
        return "invalid arguments";
    }
    if (!digest) {
        mem_patch_message("executable checksum unavailable; patch refused.", NULL);
        return "checksum unavailable";
    }
    if (!mem_patch_checksum_matches(argv[3], digest)) {
        mem_patch_message("executable checksum mismatch; patch refused.", digest);
        return "checksum mismatch";
    }
    uintptr_t address;
    unsigned char bytes[MEM_PATCH_MAX_BYTES];
    size_t length = 0;
    if (!mem_patch_parse_address(argv[1], &address) ||
        !mem_patch_parse_bytes(argv[2], bytes, sizeof bytes, &length)) {
        mem_patch_message("invalid hexadecimal address or patch; patch refused.", digest);
        return "invalid patch";
    }
    const char *failure = "patch refused";
    int write_status = mem_patch_write(address, bytes, length, &failure);
    if (write_status <= 0) {
        mem_patch_message(failure, digest);
        return write_status < 0 ? "applied; permission restore failed" : "patch refused";
    }
    fprintf(stderr, "[t2_hook] memPatch: applied %zu bytes at 0x%08lx\n",
            length, (unsigned long)address);
    return "ok";
}

static void bytes_to_hex(const uint8_t *data, int len, char *out) {
    for (int i = 0; i < len; i++)
        sprintf(out + i*2, "%02x", data[i]);
    out[len*2] = '\0';
}

static const char* hook_tn_ipv4_hex_cb(void *obj, S32 argc, const char **argv) {
    static char result[9];
    struct in_addr address;
    (void)obj;
    result[0] = 0;
    if (argc < 2 || !argv[1] || inet_pton(AF_INET, argv[1], &address) != 1)
        return result;
    bytes_to_hex((const uint8_t *)&address.s_addr, 4, result);
    return result;
}

/* T2CSRI's eight hex digits back to a dotted IPv4 address, or "". */
static const char* hook_tn_hex_ipv4_cb(void *obj, S32 argc, const char **argv) {
    static char result[16];
    (void)obj;
    result[0] = 0;
    if (argc < 2 || !argv[1] || strlen(argv[1]) != 8) return result;
    int bytes[4];
    for (int i = 0; i < 4; i++) {
        int high = hex2nibble(argv[1][2 * i]), low = hex2nibble(argv[1][2 * i + 1]);
        if (high < 0 || low < 0) return result;
        bytes[i] = high << 4 | low;
    }
    snprintf(result, sizeof(result), "%d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]);
    return result;
}

/* The TribesNext authentication server's RSA modulus (public exponent 3),
   from TribesNext's t2Dll.cpp. It signs each account certificate
   "name\tguid\te\tn\tsignature": signature^3 mod N is the SHA-1 of the
   first four fields. */
static const char tribesnext_root_modulus[] =
    "a2303d7c7d3ff28daa2dfd4f7fddcbb939aa461a3a298f22f4cbc8f87c6e584479861b1a638ea2daf87a18420f65b486"
    "cbe70453ce4f5f38a55b30026457eab3ef2716aa409d25cb8b809a9ef06e3bcdc5b37f0d975cca972601d6a6d4be10e8"
    "e5994708e878ab846caa58466eea79ccd1b49f9f06e4d6b335ba2b47b6dc81a04167a2c2fa2b9f847c1375cb6fef1a3c"
    "7ced04f750a9605fab584d2d2a4bbed6d41d4c2eeb5e223ac3bc6d8ac025f4b25bb1058998f768025baea18272a3bdda"
    "b58f94a9cb4a14452eaf7a50812e012fc42feb5d986183da134f168b447e4da6e00c351bc679b098b78bfdf3e17efcd6"
    "5ec3d2098a14426957db5e30fa01292961f3caf3aadb12aff45782dd9f8737953491c90ad4df8b76ef2559b4ef3bae95"
    "7f964cf4c9ded3d2171c9d190ba5a6a079a3d75f2c583217db626bd4074a2559794b475a9c2384096aa2b4816d2e61c1"
    "44d1de14164b77428a5329da3a4a8174b6a84d8733495ac6bc7efa981f23b4f401a6216e18771856721c74ff136c1cbe"
    "7449de0dff26c04f940d3ce31ef369e3b09932ffe7ffb347554bd2171dc5c79337a6d0dad0f8aa5f45bf054d0d0e5b73"
    "eaa83e6fdcaf4cbf2422697aa570823c682d76087b6815b0212c80e3ed550420a45f70533214619efc468548af45b6e7"
    "3fec0acbae55b08c978c0c3de09c79a50626502c49b541b7bf002508cc5a8095";

static int is_hex_text(const char *text, size_t length) {
    if (!length) return 0;
    for (size_t i = 0; i < length; i++)
        if (hex2nibble(text[i]) < 0) return 0;
    return 1;
}

/* "1" when a T2CSRI account certificate is well formed and signed by the
   TribesNext root key, else "0". */
static const char* hook_tn_verify_cert_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 2 || !argv[1]) return "0";
    const char *cert = argv[1], *fields[5];
    size_t lengths[5];
    const char *field = cert;
    for (int i = 0; i < 5; i++) {
        const char *tab = strchr(field, '\t');
        if ((i < 4) != (tab != NULL)) return "0";
        fields[i] = field;
        lengths[i] = tab ? (size_t)(tab - field) : strlen(field);
        if (!lengths[i]) return "0";
        field = tab ? tab + 1 : field;
    }
    for (size_t i = 0; i < lengths[1]; i++)
        if (fields[1][i] < '0' || fields[1][i] > '9') return "0";
    if (lengths[1] > 10 || !is_hex_text(fields[2], lengths[2]) || !is_hex_text(fields[3], lengths[3]) ||
        !is_hex_text(fields[4], lengths[4]) || lengths[4] > sizeof(tribesnext_root_modulus) - 1)
        return "0";
    size_t signed_length = (size_t)(fields[4] - cert) - 1;
    char digest[41], signature[1025], recovered[2048];
    sha1((const uint8_t *)cert, (int)signed_length, digest);
    memcpy(signature, fields[4], lengths[4]);
    signature[lengths[4]] = 0;
    if (!run_rsa_helper(signature, "3", tribesnext_root_modulus, recovered, sizeof(recovered))) {
        fprintf(stderr, "[t2_hook] TribesNext certificate check: RSA helper failed\n");
        return "0";
    }
    /* Compare as numbers: both sides without leading zeros, either case. */
    const char *a = digest, *b = recovered;
    while (*a == '0' && a[1]) a++;
    while (*b == '0' && b[1]) b++;
    return strcasecmp(a, b) == 0 ? "1" : "0";
}

/* The stock server sends each challenge reply the flag "this server has a
   WON certificate", and a client without one then refuses to connect
   (CHR_NOT_AUTHENTICATED). Send it only to clients that sent a WON
   certificate themselves (the pending connection's certificate at
   +0x2154), so wonemu players still authenticate both ways while
   TribesNext players connect and use T2CSRI. A server without a WON
   certificate also turns away every client off its own Class B network;
   skip that LAN-only check, as TribesNext servers do. A server with one
   ignores server-browser pings and info queries flagged as coming from a
   client without one (flag bit 0), so TribesNext players' browsers time it
   out; answer those as well. */
#define SERVER_REPLY_CERT_FLAG 0x0822B604
#define SERVER_LAN_CHECK       0x0822BC85
#define SERVER_PING_QUERY_SKIP 0x08235323  /* jne past the ping reply */
#define SERVER_INFO_QUERY_SKIP 0x0823601E  /* jne past the info reply */
static const unsigned char server_reply_cert_flag_original[] =
    {0x83, 0x3d, 0x34, 0x60, 0x6b, 0x08, 0x00, 0x0f, 0x95, 0xc0};
static const unsigned char server_lan_check_original[] =
    {0x83, 0x3d, 0x34, 0x60, 0x6b, 0x08, 0x00, 0x0f, 0x85, 0x42, 0x03, 0x00, 0x00};
static const unsigned char server_ping_query_skip_original[] = {0x0f, 0x85, 0x30, 0x02, 0x00, 0x00};
static const unsigned char server_info_query_skip_original[] = {0x0f, 0x85, 0xfb, 0x05, 0x00, 0x00};

/* Without a configured public address, use the one run-game looked up from
   TribesNext (T2LAUNCH_DETECTED_PUBLIC_ADDRESS). */
static void use_detected_public_address(void) {
    const char *configured = hook_option_str("server.public_address");
    const char *detected = getenv("T2LAUNCH_DETECTED_PUBLIC_ADDRESS");
    struct in_addr address;
    if (configured && configured[0]) {
        fprintf(stderr, "[t2_hook] TribesNext logins must name %s, a bind address or the player's LAN\n",
                configured);
    } else if (detected && inet_pton(AF_INET, detected, &address) == 1) {
        store_hook_option("server.public_address", detected);
        fprintf(stderr, "[t2_hook] TribesNext logins must name %s (looked up), a bind address or the "
                "player's LAN\n", detected);
    } else {
        fprintf(stderr, "[t2_hook] Public address unknown: TribesNext players can join only from the LAN "
                "or through a bind address; set the server's public IPv4 address\n");
    }
}

static int install_server_tribesnext_auth(void) {
    /* cmpl $0,0x2154(%esi), keeping the setne that follows */
    static const unsigned char client_cert_flag[] = {0x83, 0xbe, 0x54, 0x21, 0x00, 0x00, 0x00};
    /* jmp to the accept path (0x0822BFD4), then nops */
    static const unsigned char accept_any[] =
        {0xe9, 0x4a, 0x03, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    static const unsigned char answer_query[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const struct code_patch patches[] = {
        {SERVER_REPLY_CERT_FLAG, server_reply_cert_flag_original, sizeof(server_reply_cert_flag_original),
         0, client_cert_flag, sizeof(client_cert_flag)},
        {SERVER_LAN_CHECK, server_lan_check_original, sizeof(server_lan_check_original),
         0, accept_any, sizeof(accept_any)},
        {SERVER_PING_QUERY_SKIP, server_ping_query_skip_original, sizeof(server_ping_query_skip_original),
         0, answer_query, sizeof(answer_query)},
        {SERVER_INFO_QUERY_SKIP, server_info_query_skip_original, sizeof(server_info_query_skip_original),
         0, answer_query, sizeof(answer_query)},
    };
    if (!apply_code_patches("TribesNext server", patches, sizeof(patches) / sizeof(patches[0]))) return 0;
    fprintf(stderr, "[t2_hook] Server: clients without a WON certificate may connect and query it "
            "(memory only)\n");
    return 1;
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int decode_base64(const char *input, uint8_t *output, int capacity) {
    unsigned int value = 0;
    int bits = -8, length = 0;
    for (int i = 0; input[i]; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int digit = base64_value(c);
        if (digit < 0) return -1;
        value = (value << 6) | (unsigned int)digit;
        bits += 6;
        if (bits >= 0) {
            if (length >= capacity) return -1;
            output[length++] = (uint8_t)((value >> bits) & 0xff);
            bits -= 8;
        }
    }
    return length;
}

static int parse_hex_bytes(const char *hex, uint8_t *bytes, int capacity) {
    size_t length = strlen(hex);
    if ((length & 1) || length / 2 > (size_t)capacity) return -1;
    for (size_t i = 0; i < length; i += 2) {
        int hi = hex2nibble(hex[i]), lo = hex2nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        bytes[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(length / 2);
}

static void rc4_drop2048_crypt(const uint8_t *key, int keylen,
                               const uint8_t *input, uint8_t *output, int length) {
    uint8_t state[256];
    for (int i = 0; i < 256; i++) state[i] = (uint8_t)i;
    int i = 0, j = 0;
    for (int k = 0; k < 256; k++) {
        j = (j + state[k] + key[k % keylen]) & 0xff;
        uint8_t temp = state[k]; state[k] = state[j]; state[j] = temp;
    }
    i = 0; j = 0;
    for (int k = 0; k < 2048 + length; k++) {
        i = (i + 1) & 0xff;
        j = (j + state[i]) & 0xff;
        uint8_t temp = state[i]; state[i] = state[j]; state[j] = temp;
        uint8_t stream = state[(state[i] + state[j]) & 0xff];
        if (k >= 2048) output[k - 2048] = input[k - 2048] ^ stream;
    }
    memset(state, 0, sizeof(state));
}

static const char* hook_tn_unlock_cb(void *obj, S32 argc, const char **argv) {
    static char result[32];
    char password[512] = {0};
    char line[32768], cert_copy[20001], private_entry[8192];
    char fields[5][4097];
    uint8_t decoded_store[8192], cipher[4096], plaintext[4096];
    char key[41];
    char private_hex[8193], check_hash[41];
    char store_path[4096];
    FILE *file = NULL;
    int ok = 0;
    const char *failure_reason = "stored credentials are invalid";
    (void)obj;
    if (argc < 3 || !argv[1] || !argv[2] || strlen(argv[1]) >= sizeof(tribesnext_username) ||
        strlen(argv[2]) >= sizeof(password)) return "invalid";
    snprintf(password, sizeof(password), "%s", argv[2]);
    memset(tribesnext_password, 0, sizeof(tribesnext_password));
    snprintf(tribesnext_username, sizeof(tribesnext_username), "%s", argv[1]);
    tribesnext_certificate[0] = 0;
    tribesnext_modulus[0] = tribesnext_exponent[0] = tribesnext_private_exponent[0] = 0;

    file = fopen(game_path(store_path, sizeof(store_path), "base/public.store"), "r");
    if (!file) { failure_reason = "public.store is unavailable"; goto cleanup; }
    while (fgets(line, sizeof(line), file)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcasecmp(line, argv[1]) == 0) {
            *tab = '\t';
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) >= sizeof(tribesnext_certificate)) goto cleanup;
            snprintf(tribesnext_certificate, sizeof(tribesnext_certificate), "%s", line);
            break;
        }
    }
    fclose(file); file = NULL;
    if (!tribesnext_certificate[0]) {
        failure_reason = "account certificate is missing";
        goto cleanup;
    }
    snprintf(cert_copy, sizeof(cert_copy), "%s", tribesnext_certificate);
    int count = 0;
    char *save = NULL;
    for (char *field = strtok_r(cert_copy, "\t", &save); field;
         field = strtok_r(NULL, "\t", &save)) {
        if (count >= 5 || strlen(field) >= sizeof(fields[0])) goto cleanup;
        snprintf(fields[count++], sizeof(fields[0]), "%s", field);
    }
    if (count != 5 || strcasecmp(fields[0], argv[1]) != 0 ||
        !fields[1][0] || !fields[2][0] || !fields[3][0] || !fields[4][0]) {
        failure_reason = "account certificate format is invalid";
        goto cleanup;
    }
    for (int i = 0; fields[3][i]; i++) if (hex2nibble(fields[3][i]) < 0) goto cleanup;
    for (int i = 0; fields[2][i]; i++) if (hex2nibble(fields[2][i]) < 0) goto cleanup;
    snprintf(tribesnext_exponent, sizeof(tribesnext_exponent), "%s", fields[2]);
    snprintf(tribesnext_modulus, sizeof(tribesnext_modulus), "%s", fields[3]);

    file = fopen(game_path(store_path, sizeof(store_path), "base/private.store"), "r");
    if (!file) { failure_reason = "private.store is unavailable"; goto cleanup; }
    private_entry[0] = 0;
    while (fgets(line, sizeof(line), file)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcasecmp(line, argv[1]) == 0) {
            snprintf(private_entry, sizeof(private_entry), "%s", tab + 1);
            private_entry[strcspn(private_entry, "\r\n")] = 0;
            break;
        }
    }
    fclose(file); file = NULL;
    if (!private_entry[0]) {
        failure_reason = "account private-key entry is missing";
        goto cleanup;
    }
    char *entry = private_entry;
    if (!strchr(entry, ':')) {
        int decoded_len = decode_base64(entry, decoded_store, sizeof(decoded_store) - 1);
        if (decoded_len <= 0 || decoded_len >= (int)sizeof(decoded_store)) {
            failure_reason = "private-key store encoding is invalid";
            goto cleanup;
        }
        decoded_store[decoded_len] = 0;
        entry = (char *)decoded_store;
    }
    char *colon = strchr(entry, ':');
    if (!colon) { failure_reason = "private-key store format is invalid"; goto cleanup; }
    *colon = 0;
    const char *nonce = entry;
    const char *cipher_hex = colon + 1;
    if (strlen(nonce) != 40) { failure_reason = "private-key nonce is invalid"; goto cleanup; }
    char password_nonce[1024];
    if (snprintf(password_nonce, sizeof(password_nonce), "%s%s", password, nonce) >= (int)sizeof(password_nonce)) goto cleanup;
    sha1((const uint8_t *)password_nonce, strlen(password_nonce), key);
    int cipher_len = parse_hex_bytes(cipher_hex, cipher, sizeof(cipher));
    if (cipher_len <= 0) { failure_reason = "private-key ciphertext is invalid"; goto cleanup; }
    rc4_drop2048_crypt((const uint8_t *)key, 40, cipher, plaintext, cipher_len);
    bytes_to_hex(plaintext, cipher_len, private_hex);
    sha1((const uint8_t *)private_hex, (int)strlen(private_hex), check_hash);
    if (strcasecmp(check_hash, nonce) != 0) {
        failure_reason = "entered password did not unlock private.store";
        goto cleanup;
    }
    snprintf(tribesnext_private_exponent, sizeof(tribesnext_private_exponent), "%s", private_hex);
    snprintf(tribesnext_password, sizeof(tribesnext_password), "%s", password);
    ok = 1;

cleanup:
    if (file) fclose(file);
    memset(password, 0, sizeof(password));
    memset(password_nonce, 0, sizeof(password_nonce));
    memset(key, 0, sizeof(key));
    memset(cipher, 0, sizeof(cipher));
    memset(plaintext, 0, sizeof(plaintext));
    memset(private_hex, 0, sizeof(private_hex));
    memset(decoded_store, 0, sizeof(decoded_store));
    memset(fields, 0, sizeof(fields));
    memset(private_entry, 0, sizeof(private_entry));
    if (!ok) {
        fprintf(stderr, "[t2_hook] TribesNext credential unlock failed: %s\n", failure_reason);
        memset(tribesnext_password, 0, sizeof(tribesnext_password));
        memset(tribesnext_private_exponent, 0, sizeof(tribesnext_private_exponent));
        tribesnext_certificate[0] = 0;
        tribesnext_modulus[0] = tribesnext_exponent[0] = 0;
        snprintf(result, sizeof(result), "invalid");
        return result;
    }
    snprintf(result, sizeof(result), "ok");
    fprintf(stderr, "[t2_hook] TribesNext stored-key unlock succeeded\n");
    return result;
}

static int reload_tribesnext_certificate(void) {
    static char line[32768];
    static char copy[20001];
    static char fields[5][4097];
    char store_path[4096];
    FILE *file;
    if (!tribesnext_username[0] || !tribesnext_modulus[0] || !tribesnext_exponent[0])
        return 0;
    file = fopen(game_path(store_path, sizeof(store_path), "base/public.store"), "r");
    if (!file) return 0;
    line[0] = 0;
    while (fgets(line, sizeof(line), file)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcasecmp(line, tribesnext_username) == 0) {
            *tab = '\t';
            line[strcspn(line, "\r\n")] = 0;
            break;
        }
        line[0] = 0;
    }
    fclose(file);
    if (!line[0] || strlen(line) >= sizeof(copy)) return 0;
    snprintf(copy, sizeof(copy), "%s", line);
    int count = 0;
    char *save = NULL;
    for (char *field = strtok_r(copy, "\t", &save); field;
         field = strtok_r(NULL, "\t", &save)) {
        if (count >= 5 || strlen(field) >= sizeof(fields[0])) return 0;
        snprintf(fields[count++], sizeof(fields[0]), "%s", field);
    }
    if (count != 5 || strcasecmp(fields[0], tribesnext_username) != 0) return 0;
    if (strcasecmp(fields[2], tribesnext_exponent) != 0 ||
        strcasecmp(fields[3], tribesnext_modulus) != 0) {
        if (!tribesnext_password[0]) return 0;
        const char *unlock_args[] = {"hook_tn_unlock", tribesnext_username, tribesnext_password};
        if (strcmp(hook_tn_unlock_cb(NULL, 3, unlock_args), "ok") != 0) return 0;
        return tribesnext_certificate[0] != 0;
    }
    snprintf(tribesnext_certificate, sizeof(tribesnext_certificate), "%s", line);
    fprintf(stderr, "[t2_hook] Loaded refreshed TribesNext account certificate\n");
    memset(copy, 0, sizeof(copy));
    memset(fields, 0, sizeof(fields));
    return 1;
}

static const char* hook_tn_certificate_length_cb(void *obj, S32 argc, const char **argv) {
    static char result[16];
    (void)obj; (void)argc; (void)argv;
    if (!reload_tribesnext_certificate()) {
        result[0] = '0'; result[1] = 0;
        return result;
    }
    snprintf(result, sizeof(result), "%lu", (unsigned long)strlen(tribesnext_certificate));
    return result;
}

static const char* hook_tn_certificate_chunk_cb(void *obj, S32 argc, const char **argv) {
    static char result[201];
    (void)obj;
    result[0] = 0;
    if (argc < 2 || !argv[1] || !tribesnext_certificate[0]) return result;
    char *end = NULL;
    long offset = strtol(argv[1], &end, 10);
    size_t cert_length = strlen(tribesnext_certificate);
    if (!end || *end || offset < 0 || (size_t)offset >= cert_length) return result;
    size_t chunk_length = cert_length - (size_t)offset;
    if (chunk_length > 200) chunk_length = 200;
    memcpy(result, tribesnext_certificate + offset, chunk_length);
    result[chunk_length] = 0;
    return result;
}

static const char* hook_tn_decrypt_cb(void *obj, S32 argc, const char **argv) {
    static char result[4096];
    (void)obj;
    if (argc < 2 || !tribesnext_private_exponent[0] || !tribesnext_modulus[0] ||
        strlen(argv[1]) >= sizeof(result)) return "";
    for (int i = 0; argv[1][i]; i++) if (hex2nibble(argv[1][i]) < 0) return "";
    if (!run_rsa_helper(argv[1], tribesnext_private_exponent, tribesnext_modulus,
                        result, sizeof(result))) {
        fprintf(stderr, "[t2_hook] TribesNext server challenge RSA operation failed\n");
        return "";
    }
    fprintf(stderr, "[t2_hook] TribesNext server challenge decrypted\n");
    return result;
}

static const char* hook_tn_clear_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    memset(tribesnext_certificate, 0, sizeof(tribesnext_certificate));
    memset(tribesnext_modulus, 0, sizeof(tribesnext_modulus));
    memset(tribesnext_exponent, 0, sizeof(tribesnext_exponent));
    memset(tribesnext_private_exponent, 0, sizeof(tribesnext_private_exponent));
    memset(tribesnext_username, 0, sizeof(tribesnext_username));
    memset(tribesnext_password, 0, sizeof(tribesnext_password));
    return "ok";
}

static const char* hook_tn_trace_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 2 || !argv[1]) return "invalid";
    if (strcmp(argv[1], "poke") == 0)
        fprintf(stderr, "[t2_hook] T2CSRI server authentication request received\n");
    else if (strcmp(argv[1], "challenge_chunk") == 0)
        fprintf(stderr, "[t2_hook] T2CSRI encrypted challenge chunk received\n");
    else if (strcmp(argv[1], "decrypt_challenge") == 0)
        fprintf(stderr, "[t2_hook] T2CSRI challenge decryption started\n");
    else if (strcmp(argv[1], "response_sent") == 0)
        fprintf(stderr, "[t2_hook] T2CSRI challenge response sent\n");
    else if (strcmp(argv[1], "certificate_sent") == 0) {
        char digest[41];
        size_t cert_length = strlen(tribesnext_certificate);
        sha1((const uint8_t *)tribesnext_certificate, (int)cert_length, digest);
        fprintf(stderr, "[t2_hook] T2CSRI account certificate chunks sent length=%lu sha1=%s\n",
                (unsigned long)cert_length, digest);
    }
    else if (strcmp(argv[1], "client_challenge_sent") == 0)
        fprintf(stderr, "[t2_hook] T2CSRI client challenge sent\n");
    else
        return "invalid";
    return "ok";
}

static const char* hook_tn_join_trace_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 3 || !argv[1] || !argv[2]) return "invalid";
    fprintf(stderr, "[t2_hook] Join requested for %s (TribesNext key ready=%s)\n",
            argv[1], strcmp(argv[2], "1") == 0 ? "yes" : "no");
    return "ok";
}

static const char* hook_tn_ip_trace_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 3 || !argv[1] || !argv[2]) return "invalid";
    fprintf(stderr, "[t2_hook] T2CSRI challenge target IP=%s hex=%s\n", argv[1], argv[2]);
    return "ok";
}

static const char *hook_option_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    if (argc < 2 || !argv[1]) return "";
    const char *value = hook_option_str(argv[1]);
    return value ? value : "";
}

/* Session-only A/B controls. The configured value changes immediately; code
   changes use the same verified, demo-gated transition path as each swap. */
static const char *review_feature_cb(enum review_patch_group group, const char *key,
                                     S32 argc, const char **argv) {
    static char result[160];
    if (target->dedicated) return "client only";
    if (argc < 1 || argc > 2 || (argc == 2 && (!argv[1] ||
        (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0))))
        return "expected 0 or 1, or no argument to query";
    if (argc == 2) store_hook_option(key, argv[1]);
    int main_thread = game_main_thread_known && pthread_equal(pthread_self(), game_main_thread);
    if (main_thread) update_review_patches();
    int configured = hook_option_bool(key);
    int applied = (review_applied_mask & review_group_mask(group)) != 0;
    /* Autobeacon uses the core dispatcher, which stays installed even when
       target reassignment is disabled. Report its actual playback gate. */
    if (group == REVIEW_CORE)
        applied = applied && configured && hook_option_bool("review.cheat_view") && review_demo_playing();
    size_t length = (size_t)snprintf(result, sizeof result, "%s configured=%d applied=%d", key,
        configured, applied);
    /* Expose the actual recorded mission's stock far distance for range tests,
       rather than assuming every mission uses the 1000-unit fallback. */
    if (group == REVIEW_RANGE && main_thread) {
        struct rv_memory memory;
        const char *unknown = " stock_far=unknown";
        float stock = 0;
        if (rv_memory_snapshot(&memory) &&
            rv_memory_range(&memory, (void *)0x08495184, 4, PROT_READ)) {
            void *graph = rv_pointer((void *)0x08495184, 0);
            if (rv_memory_range(&memory, graph, 0xe8, PROT_READ)) {
                void *scene = rv_pointer(graph, 0xe4);
                if (rv_memory_range(&memory, scene, 0xe30, PROT_READ)) stock = rv_float(scene, 0xe2c);
                else if (!scene) stock = 1000.0f;  /* camera-query fallback */
            }
        }
        if (isfinite(stock) && stock > 0)
            snprintf(result + length, sizeof result - length, " stock_far=%.1f review_far=20000", stock);
        else
            snprintf(result + length, sizeof result - length, "%s", unknown);
    }
    if (argc == 2) fprintf(stderr, "[t2_hook] Review control: %s\n", result);
    return result;
}
static const char *hook_review_range_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    return review_feature_cb(REVIEW_RANGE, "review.extended_range", argc, argv);
}
static const char *hook_review_global_beacons_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    return review_feature_cb(REVIEW_GLOBAL_BEACONS, "review.global_beacons", argc, argv);
}
static const char *hook_review_ffi_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    return review_feature_cb(REVIEW_FFI, "review.ffi", argc, argv);
}
static const char *hook_review_no_cloak_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    return review_feature_cb(REVIEW_NO_CLOAK, "review.no_cloak", argc, argv);
}
static const char *hook_review_autobeacon_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    return review_feature_cb(REVIEW_CORE, "review.autobeacon", argc, argv);
}

/* Review asset selection and cache diagnostics. Loki #25034:
   ResourceManager+0x884 is the dictionary; ResourceObject+0x34 is the cached
   instance. OpenStream builds its OS filename from +0x20/+0x24. The diagnostic
   reads this cache rather than getFileCRC's independent file lookup. */
#define RV_RESOURCE_MANAGER 0x08493de4u
#define RV_MODEL_FIND_CALL 0x0810766bu
#define RV_BITMAP_FIND_CALL 0x081085a3u
#define RV_HUD_WAKE_CALL 0x082fb974u
#define RV_TEXTURE_LIST 0x08494194u
static const unsigned char rv_model_find_orig[] = {0xe8, 0x54, 0x10, 0x00, 0x00};
static const unsigned char rv_bitmap_find_orig[] = {0xe8, 0x1c, 0x01, 0x00, 0x00};
static const unsigned char rv_hud_wake_orig[] = {0xe8, 0x7f, 0x85, 0xe4, 0xff};
typedef void *(*rv_resource_find_t)(void *, const char *, unsigned char);
static rv_resource_find_t rv_native_resource_find = (rv_resource_find_t)0x081086c4;
static unsigned char (*rv_native_hud_wake)(void *) = (unsigned char (*)(void *))0x08143ef8;
static const struct {
    uintptr_t address;
    size_t length;
    unsigned char bytes[6];
} rv_resource_anchors[] = {
    {0x0825d15c, 5, {0xa1, 0xe4, 0x3d, 0x49, 0x08}},
    {0x08107a57, 6, {0x81, 0xc3, 0x84, 0x08, 0x00, 0x00}},
    {0x081061a2, 3, {0x8b, 0x5b, 0x08}},
    {0x0810785e, 6, {0x8b, 0x47, 0x20, 0x8b, 0x57, 0x24}},
    {0x0825d18e, 6, {0x89, 0xb0, 0xb8, 0x00, 0x00, 0x00}},
    {0x0825d11f, 3, {0x8b, 0x41, 0x44}},
};

/* Native lookup accepts the first matching resource in a collision chain.
   Registration's ordering is not reliable when volume and loose entries mix.
   Prefer the real loose counterpart for review assets BEFORE shape preload;
   never replace an already-created TSShape or alter the resource dictionary. */
static void *rv_prefer_loose_resource(void *original) {
    if (!original) return NULL;
    const char *path = rv_pointer(original, 0x18);
    const char *name = rv_pointer(original, 0x1c);
    uint32_t flags = rv_word(original, 0x14);
    if (!path || !name || (flags & 2) ||
        !((strncmp(path, "shapes", 6) == 0 && (!path[6] || path[6] == '/')) ||
          (strncmp(path, "textures", 8) == 0 && (!path[8] || path[8] == '/')))) return original;
    char backing[1024];
    int length = snprintf(backing, sizeof backing, "base/%s", path);
    if (length < 0 || (size_t)length >= sizeof backing) return original;
    void *resource = rv_pointer(original, 8);
    for (unsigned int visited = 0; resource && visited < 65536; visited++) {
        uint32_t candidate_flags = rv_word(resource, 0x14);
        if (rv_pointer(resource, 0x18) == path && rv_pointer(resource, 0x1c) == name &&
            (candidate_flags & 3) == 2 && ((candidate_flags ^ flags) & 8) == 0) {
            const char *disk_path = rv_pointer(resource, 0x20);
            const char *disk_name = rv_pointer(resource, 0x24);
            if (disk_path && disk_name && strcmp(disk_path, backing) == 0 &&
                strcmp(disk_name, name) == 0) return resource;
        }
        resource = rv_pointer(resource, 8);
    }
    return original;
}

static void *review_model_find(void *manager, const char *name, unsigned char force) {
    void *original = rv_native_resource_find(manager, name, force);
    if (target->dedicated || !hook_option_bool("review.cheat_view") ||
        !hook_option_bool("review.replace_models")) return original;
    void *preferred = rv_prefer_loose_resource(original);
    if (preferred != original && !rv_pointer(preferred, 0x34))
        fprintf(stderr, "[t2_hook] Review asset selection: %s/%s -> %s/%s (loose before volume)\n",
            (const char *)rv_pointer(preferred, 0x18), (const char *)rv_pointer(preferred, 0x1c),
            (const char *)rv_pointer(preferred, 0x20), (const char *)rv_pointer(preferred, 0x24));
    return preferred;
}

/* Bitmap instances bypass ResourceManager::load and are later uploaded to GL.
   Log actual successful source lookups separately from decoded/cache evidence. */
static void *review_bitmap_find(void *manager, const char *name, unsigned char force) {
    void *resource = rv_native_resource_find(manager, name, force);
    if (target->dedicated || !hook_option_bool("review.cheat_view") ||
        !hook_option_bool("review.replace_models")) return resource;
    resource = rv_prefer_loose_resource(resource);
    if (!resource || !pthread_equal(pthread_self(), game_main_thread))
        return resource;
    const char *path = rv_pointer(resource, 0x18);
    if (!path || strncmp(path, "textures", 8) != 0 || (path[8] && path[8] != '/')) return resource;
    static struct { void *resource; const char *name, *backing_path, *backing_name; } seen[1024];
    static size_t count;
    const char *file = rv_pointer(resource, 0x1c);
    const char *backing_path = rv_pointer(resource, 0x20);
    const char *backing_name = rv_pointer(resource, 0x24);
    for (size_t i = 0; i < count; i++)
        if (seen[i].resource == resource && seen[i].name == file &&
            seen[i].backing_path == backing_path && seen[i].backing_name == backing_name) return resource;
    if (count == sizeof seen / sizeof seen[0]) {
        static int reported_limit;
        if (!reported_limit) fprintf(stderr, "[t2_hook] Review bitmap source log limit reached (1024 unique sources)\n");
        reported_limit = 1;
        return resource;
    }
    seen[count].resource = resource; seen[count].name = file;
    seen[count].backing_path = backing_path; seen[count].backing_name = backing_name; count++;
    if (file && backing_name)
        fprintf(stderr, "[t2_hook] Review bitmap source: logical=\"%s/%s\" backing=\"%s%s%s\" "
            "kind=%s resource=%p\n", path, file, backing_path ? backing_path : "",
            backing_path && *backing_path ? "/" : "", backing_name,
            rv_word(resource, 0x14) & 1 ? "volume" : rv_word(resource, 0x14) & 2 ? "loose" : "other", resource);
    return resource;
}

/* Beacon settings from the supplied hm2 PlayGui.gui, applied before the native
   GameTSCtrl acquires its texture handles. Their reference counting stays native. */
static int rv_apply_review_hud(void *hud, int textures_available) {
    if (!hud || !textures_available || target->dedicated || !hook_option_bool("review.cheat_view") ||
        !hook_option_bool("review.replace_models")) return 0;
    static const char *names[] = {"gui/happy_beaconbase", "gui/happy_beacontarget"};
    static const float colors[6][4] = {
        {0, 1, 1, .8f}, {0, 1, 1, .8f},
        {1, 1, 0, .8f}, {1, 1, 0, .8f},
        {1, 1, 1, 1}, {1, 1, 1, 1},
    };
    for (unsigned int i = 0; i < 2; i++) {
        uint32_t pointer = (uint32_t)(uintptr_t)names[i];
        memcpy((unsigned char *)hud + 0x1fc + i * 4, &pointer, 4);
    }
    uint32_t period = 4000, text_offset = 14000;
    float line_width = 1;
    memcpy((unsigned char *)hud + 0x204, &period, 4);
    *((unsigned char *)hud + 0x208) = 1;
    memcpy((unsigned char *)hud + 0x20c, colors, sizeof colors);
    memcpy((unsigned char *)hud + 0x26c, &line_width, 4);
    memcpy((unsigned char *)hud + 0x270, &text_offset, 4);
    *((unsigned char *)hud + 0x274) = 0;
    return 1;
}

static unsigned char review_hud_wake(void *hud) {
    if (!target->dedicated && hook_option_bool("review.cheat_view") && hook_option_bool("review.replace_models")) {
        char base_path[4096], target_path[4096];
        int available = access(game_path(base_path, sizeof base_path, "base/textures/gui/happy_beaconbase.png"), R_OK) == 0 &&
            access(game_path(target_path, sizeof target_path, "base/textures/gui/happy_beacontarget.png"), R_OK) == 0;
        if (rv_apply_review_hud(hud, available))
            fprintf(stderr, "[t2_hook] Review HUD: hm2 beacon presentation configured before native wake\n");
        else if (!available)
            fprintf(stderr, "[t2_hook] Review HUD: replacement images unavailable; keeping current presentation\n");
    }
    return rv_native_hud_wake(hud);
}

static void install_review_model_selection(void) {
    if (target->dedicated || !hook_option_bool("review.cheat_view") ||
        !hook_option_bool("review.replace_models")) return;
    unsigned char model_call[5] = {0xe8}, bitmap_call[5] = {0xe8}, hud_call[5] = {0xe8};
    int32_t relative = (int32_t)((uintptr_t)review_model_find - (RV_MODEL_FIND_CALL + sizeof model_call));
    memcpy(model_call + 1, &relative, sizeof relative);
    relative = (int32_t)((uintptr_t)review_bitmap_find - (RV_BITMAP_FIND_CALL + sizeof bitmap_call));
    memcpy(bitmap_call + 1, &relative, sizeof relative);
    relative = (int32_t)((uintptr_t)review_hud_wake - (RV_HUD_WAKE_CALL + sizeof hud_call));
    memcpy(hud_call + 1, &relative, sizeof relative);
    const struct code_patch patches[] = {
        {RV_MODEL_FIND_CALL, rv_model_find_orig, sizeof rv_model_find_orig, 0, model_call, sizeof model_call},
        {RV_BITMAP_FIND_CALL, rv_bitmap_find_orig, sizeof rv_bitmap_find_orig, 0, bitmap_call, sizeof bitmap_call},
        {RV_HUD_WAKE_CALL, rv_hud_wake_orig, sizeof rv_hud_wake_orig, 0, hud_call, sizeof hud_call},
    };
    if (apply_code_patches("Review asset lookup", patches, sizeof patches / sizeof patches[0]))
        fprintf(stderr, "[t2_hook] Review assets: prefer staged loose shapes and bitmap textures before native loading\n");
}

static const char *rv_dump_model_resources(const struct rv_memory *memory, void *manager,
                                          const char *filter, FILE *output, unsigned int *matches) {
    *matches = 0;
    if (!rv_memory_range(memory, manager, 0x890, PROT_READ)) return "resource manager unavailable";
    void *table = rv_pointer(manager, 0x884);
    uint32_t buckets = rv_word(manager, 0x88c);
    if (!buckets || buckets > 65536 ||
        !rv_memory_range(memory, table, (size_t)buckets * 4, PROT_READ))
        return "resource dictionary unavailable";
    unsigned int visited = 0;
    for (uint32_t bucket = 0; bucket < buckets; bucket++) {
        void *resource = rv_pointer(table, (size_t)bucket * 4);
        for (unsigned int chain = 0; resource; chain++) {
            if (chain >= 4096 || visited++ >= 131072) return "resource traversal limit reached";
            if (!rv_memory_range(memory, resource, 0x40, PROT_READ)) return "unreadable resource entry";
            const char *name = rv_string(memory, rv_pointer(resource, 0x1c), 256);
            size_t length = name ? strlen(name) : 0;
            if (length > 4 && strcasecmp(name + length - 4, ".dts") == 0 &&
                (!*filter || strcasestr(name, filter)) && rv_pointer(resource, 0x34)) {
                const char *path = rv_string(memory, rv_pointer(resource, 0x18), 1024);
                const char *backing_path = rv_string(memory, rv_pointer(resource, 0x20), 1024);
                const char *backing_name = rv_string(memory, rv_pointer(resource, 0x24), 256);
                /* A null directory means a root-level file, not an unreadable string. */
                if (!path && !rv_pointer(resource, 0x18)) path = "";
                if (!backing_path && !rv_pointer(resource, 0x20)) backing_path = "";
                if (!path || !backing_path || !backing_name) return "unreadable resource filename";
                if (*matches >= 1024) return "model output limit reached";
                uint32_t flags = rv_word(resource, 0x14);
                fprintf(output, "[t2_hook] Review model: logical=\"%s%s%s\" backing=\"%s%s%s\" "
                    "kind=%s offset=%u flags=0x%x cached_crc=0x%08x resource=%p instance=%p\n",
                    path, *path ? "/" : "", name,
                    backing_path, *backing_path ? "/" : "", backing_name,
                    flags & 1 ? "volume" : flags & 2 ? "loose" : "other",
                    rv_word(resource, 0x28), flags, rv_word(resource, 0x3c),
                    resource, rv_pointer(resource, 0x34));
                ++*matches;
            }
            resource = rv_pointer(resource, 8);
        }
    }
    return NULL;
}

/* Connect cached resources to actual ghost objects. These exact classes share
   ShapeBase+0x2d0 -> ShapeBaseData, whose preloaded resource is at +0xb8. */
static const char *rv_dump_model_users(const struct rv_memory *memory, void *dictionary,
                                     const char *filter, FILE *output, unsigned int *matches) {
    *matches = 0;
    if (!rv_memory_range(memory, dictionary, 4096 * 4, PROT_READ)) return "object dictionary unavailable";
    unsigned int visited = 0;
    for (unsigned int bucket = 0; bucket < 4096; bucket++) {
        void *object = rv_pointer(dictionary, bucket * 4);
        for (unsigned int chain = 0; object; chain++) {
            if (chain >= 4096 || visited++ >= 131072) return "object traversal limit reached";
            if (!rv_memory_range(memory, object, 0x24, PROT_READ)) return "unreadable object entry";
            const char *name = rv_class_name(memory, object);
            if (name && (strcmp(name, "Item") == 0 || strcmp(name, "StaticShape") == 0 ||
                         strcmp(name, "Player") == 0 || strcmp(name, "Turret") == 0) &&
                rv_memory_range(memory, object, 0x2d4, PROT_READ) && (rv_word(object, 0x40) & 2)) {
                void *data = rv_pointer(object, 0x2d0);
                if (rv_memory_range(memory, data, 0xbc, PROT_READ)) {
                    const char *shape = rv_string(memory, rv_pointer(data, 0x44), 256);
                    void *resource = rv_pointer(data, 0xb8);
                    if (shape && (!*filter || strcasestr(shape, filter)) &&
                        rv_memory_range(memory, resource, 0x38, PROT_READ) && rv_pointer(resource, 0x34)) {
                        if (*matches >= 1024) return "model user output limit reached";
                        fprintf(output, "[t2_hook] Review model use: object=%u class=%s datablock=%u "
                            "shape=\"%s\" resource=%p instance=%p\n",
                            rv_word(object, 0x20), name, rv_word(data, 0x20), shape,
                            resource, rv_pointer(resource, 0x34));
                        ++*matches;
                    }
                }
            }
            object = rv_pointer(object, 0x10);
        }
    }
    return NULL;
}

static const char *hook_review_models_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    static char result[192];
    if (target->dedicated) return "client only";
    if (!game_main_thread_known || !pthread_equal(pthread_self(), game_main_thread))
        return "game thread required";
    if (argc < 1 || argc > 2 || (argc == 2 && !argv[1])) return "expected an optional filename filter";
    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory)) return "memory map unavailable";
    for (size_t i = 0; i < sizeof rv_resource_anchors / sizeof rv_resource_anchors[0]; i++) {
        const void *site = (void *)rv_resource_anchors[i].address;
        size_t length = rv_resource_anchors[i].length;
        if (!rv_memory_range(&memory, site, length, PROT_READ | PROT_EXEC) ||
            memcmp(site, rv_resource_anchors[i].bytes, length) != 0)
            return "unsupported resource layout";
    }
    if (!rv_memory_range(&memory, (void *)RV_RESOURCE_MANAGER, 4, PROT_READ))
        return "resource manager unavailable";
    unsigned int matches;
    const char *error = rv_dump_model_resources(&memory, rv_pointer((void *)RV_RESOURCE_MANAGER, 0),
        argc == 2 ? argv[1] : "", stderr, &matches);
    unsigned int users = 0;
    const char *user_error = "object dictionary unavailable";
    if (rv_memory_range(&memory, (void *)RV_ID_DICTIONARY, 4, PROT_READ))
        user_error = rv_dump_model_users(&memory, rv_pointer((void *)RV_ID_DICTIONARY, 0),
            argc == 2 ? argv[1] : "", stderr, &users);
    if (!error) error = user_error;
    snprintf(result, sizeof result, "Review models: %u cached DTS resources, %u ghost model links%s%s; details in launcher log",
        matches, users, error ? "; incomplete: " : "", error ? error : "");
    fprintf(stderr, "[t2_hook] %s\n", result);
    return result;
}

static const char *rv_dump_texture_cache(const struct rv_memory *memory, void *texture,
                                       const char *filter, FILE *output, unsigned int *matches) {
    *matches = 0;
    for (unsigned int visited = 0; texture; visited++) {
        if (visited >= 65536) return "texture traversal limit reached";
        if (!rv_memory_range(memory, texture, 0x40, PROT_READ)) return "unreadable texture entry";
        const char *key = rv_string(memory, rv_pointer(texture, 0x14), 1024);
        if (!key && rv_pointer(texture, 0x14)) return "unreadable texture key";
        int selected = key && (filter ? strcasestr(key, filter) != NULL :
            strcasestr(key, "happy_") || strcasestr(key, "beacon") || strcasestr(key, "crosshair"));
        if (selected) {
            if (*matches >= 1024) return "texture output limit reached";
            fprintf(output, "[t2_hook] Review texture: key=\"%s\" gl=0x%x small_gl=0x%x "
                "width=%u height=%u type=%u refs=%u texture=%p\n", key,
                rv_word(texture, 0xc), rv_word(texture, 0x10), rv_word(texture, 0x24),
                rv_word(texture, 0x28), rv_word(texture, 0x34), rv_word(texture, 0x3c), texture);
            ++*matches;
        }
        texture = rv_pointer(texture, 0);
    }
    return NULL;
}

static const char *rv_dump_hud_textures(const struct rv_memory *memory, void *dictionary,
                                      uint32_t hud_id, FILE *output) {
    if (!hud_id || !rv_memory_range(memory, dictionary, 4096 * 4, PROT_READ)) return "HUD unavailable";
    void *hud = rv_find_object(memory, dictionary, hud_id);
    if (!rv_memory_range(memory, hud, 0x204, PROT_READ)) return "HUD unavailable";
    const char *roles[] = {"base", "target"};
    for (unsigned int i = 0; i < 2; i++) {
        const char *configured = rv_string(memory, rv_pointer(hud, 0x1fc + i * 4), 1024);
        void *texture = rv_pointer(hud, 0x1f4 + i * 4);
        const char *key = "<unloaded>";
        uint32_t gl = 0;
        if (!configured) return "unreadable HUD texture name";
        if (texture) {
            if (!rv_memory_range(memory, texture, 0x40, PROT_READ)) return "unreadable HUD texture handle";
            key = rv_string(memory, rv_pointer(texture, 0x14), 1024);
            if (!key) return "unreadable HUD texture key";
            gl = rv_word(texture, 0xc);
        }
        fprintf(output, "[t2_hook] Review HUD texture: hud=%u role=%s configured=\"%s\" "
            "loaded_key=\"%s\" gl=0x%x texture=%p\n", hud_id, roles[i], configured, key, gl, texture);
    }
    return NULL;
}

static const char *hook_review_textures_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    static char result[256];
    static const struct { uintptr_t address; size_t length; unsigned char bytes[6]; } anchors[] = {
        {0x08114eac, 5, {0xa1, 0x94, 0x41, 0x49, 0x08}},
        {0x08114eb1, 3, {0x89, 0x73, 0x14}},
        {0x08114f26, 3, {0x89, 0x53, 0x24}},
        {0x08114f35, 3, {0x89, 0x43, 0x28}},
        {0x082fb9c0, 6, {0x89, 0x87, 0xf8, 0x01, 0x00, 0x00}},
        {0x082fba17, 6, {0x89, 0x87, 0xf4, 0x01, 0x00, 0x00}},
        {0x082fcd32, 5, {0x68, 0xfc, 0x01, 0x00, 0x00}},
        {0x082fcd4d, 5, {0x68, 0x00, 0x02, 0x00, 0x00}},
    };
    if (target->dedicated) return "client only";
    if (!game_main_thread_known || !pthread_equal(pthread_self(), game_main_thread)) return "game thread required";
    if (argc < 1 || argc > 2 || (argc == 2 && !argv[1])) return "expected an optional texture-key filter";
    struct rv_memory memory;
    if (!rv_memory_snapshot(&memory)) return "memory map unavailable";
    for (size_t i = 0; i < sizeof anchors / sizeof anchors[0]; i++)
        if (!rv_memory_range(&memory, (void *)anchors[i].address, anchors[i].length, PROT_READ | PROT_EXEC) ||
            memcmp((void *)anchors[i].address, anchors[i].bytes, anchors[i].length) != 0)
            return "unsupported texture layout";
    if (!rv_memory_range(&memory, (void *)RV_TEXTURE_LIST, 4, PROT_READ)) return "texture list unavailable";
    unsigned int matches;
    const char *error = rv_dump_texture_cache(&memory, rv_pointer((void *)RV_TEXTURE_LIST, 0),
        argc == 2 ? argv[1] : NULL, stderr, &matches);
    const char *hud_error = "HUD unavailable";
    if (rv_memory_range(&memory, (void *)RV_ID_DICTIONARY, 4, PROT_READ))
        hud_error = rv_dump_hud_textures(&memory, rv_pointer((void *)RV_ID_DICTIONARY, 0), review_model_hud_id, stderr);
    snprintf(result, sizeof result, "Review textures: %u cache entries%s%s; HUD=%s; details in launcher log",
        matches, error ? "; incomplete: " : "", error ? error : "", hud_error ? hud_error : "captured");
    fprintf(stderr, "[t2_hook] %s\n", result);
    return result;
}

/* Loading also swaps with isPlayingDemo=true. Wait for the native gameplay HUD
   instead of a timeout; capture on the next swap after all borrowed state is restored. */
static void update_review_model_diagnostic(int active) {
    static int reported;
    if (!active || target->dedicated || !hook_option_bool("review.cheat_view")) {
        review_model_hud_seen = 0; review_model_hud_id = 0; reported = 0;
        return;
    }
    if (reported || !review_model_hud_seen) return;
    reported = 1;  /* Errors must not produce a per-frame retry/log flood. */
    const char *args[] = {"hook_review_models"};
    const char *status = hook_review_models_cb(NULL, 1, args);
    fprintf(stderr, "[t2_hook] Automatic review model diagnostic: %s\n", status);
    const char *texture_args[] = {"hook_review_textures"};
    status = hook_review_textures_cb(NULL, 1, texture_args);
    fprintf(stderr, "[t2_hook] Automatic review texture diagnostic: %s\n", status);
}

/* ── Registration thread ─────────────────────────────────────────────── */

/* ── VSync (video.vsync) ─────────────────────────────────────────────────
 * The Loki client never probes for a swap-control extension, so
 * $SwapIntervalSupported stays unset, the Options checkbox is disabled, and
 * setVerticalSync() does nothing. The hook replaces setVerticalSync() with
 * one that sets the GLX swap interval, and reapplies it after a video mode
 * change creates a new context. */
static int set_swap_interval(int interval) {
    typedef void *(*get_proc_t)(const unsigned char *);
    typedef void (*swap_ext_t)(void *, unsigned long, int);
    typedef int (*swap_int_t)(int);
    typedef void *(*current_display_t)(void);
    typedef unsigned long (*current_drawable_t)(void);
    void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    void *scope = gl ? gl : RTLD_DEFAULT;
    get_proc_t get_proc = (get_proc_t)dlsym(scope, "glXGetProcAddressARB");
    if (!get_proc) get_proc = (get_proc_t)dlsym(scope, "glXGetProcAddress");
    int done = 0;
    if (get_proc) {
        swap_ext_t swap_ext = (swap_ext_t)get_proc((const unsigned char *)"glXSwapIntervalEXT");
        current_display_t display = (current_display_t)dlsym(scope, "glXGetCurrentDisplay");
        current_drawable_t drawable = (current_drawable_t)dlsym(scope, "glXGetCurrentDrawable");
        if (swap_ext && display && drawable && display() && drawable()) {
            swap_ext(display(), drawable(), interval);
            done = 1;
        }
        swap_int_t swap_mesa = done ? NULL : (swap_int_t)get_proc((const unsigned char *)"glXSwapIntervalMESA");
        if (swap_mesa && swap_mesa(interval) == 0) done = 1;
        swap_int_t swap_sgi = done ? NULL : (swap_int_t)get_proc((const unsigned char *)"glXSwapIntervalSGI");
        if (swap_sgi && interval > 0 && swap_sgi(interval) == 0) done = 1;
    }
    if (gl) dlclose(gl);
    return done;
}

static const char *set_vertical_sync_cb(void *obj, S32 argc, const char **argv) {
    (void)obj;
    int enable = argc > 1 && (strcmp(argv[1], "1") == 0 || strcasecmp(argv[1], "true") == 0);
    vsync_requested = enable;
    int done = set_swap_interval(enable ? 1 : 0);
    fprintf(stderr, "[t2_hook] VSync %s%s\n", enable ? "on" : "off",
            done ? "" : " (no GLX swap-control extension)");
    return done ? "1" : "0";
}

/* One depot helper at a time; its single output line becomes the status. */
static pid_t depot_child = -1;
static int depot_output = -1;
static char depot_result[512];
static size_t depot_result_length;

static const char *hook_depot_fetch_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc;
    const char *helper = getenv("T2LAUNCH_DEPOT_HELPER");
    const char *home = getenv("HOME");
    char destination[4096];
    if (depot_child > 0 || !helper || !helper[0] || access(helper, X_OK) != 0 ||
        !home || !home[0] || !argv[1][0])
        return "0";
    snprintf(destination, sizeof(destination), "%s/.loki/tribes2/base", home);
    char **env = helper_environment();
    int output[2];
    if (!env) return "0";
    if (pipe(output) != 0) { free(env); return "0"; }
    char *args[] = {(char *)helper, "--dest", destination, "--", (char *)argv[1], NULL};
    pid_t child = fork();
    if (child == 0) {
        dup2(output[1], STDOUT_FILENO);
        close(output[0]); close(output[1]);
        execve(helper, args, env);
        _exit(127);
    }
    free(env);
    close(output[1]);
    if (child < 0) { close(output[0]); return "0"; }
    fcntl(output[0], F_SETFL, fcntl(output[0], F_GETFL) | O_NONBLOCK);
    depot_child = child;
    depot_output = output[0];
    depot_result_length = 0;
    depot_result[0] = 0;
    fprintf(stderr, "[t2_hook] Fetching %s from the TribesNext depot\n", argv[1]);
    return "1";
}

static void read_depot_output(void) {
    while (depot_result_length < sizeof(depot_result) - 1) {
        ssize_t count = read(depot_output, depot_result + depot_result_length,
                             sizeof(depot_result) - 1 - depot_result_length);
        if (count <= 0) break;
        depot_result_length += (size_t)count;
    }
    depot_result[depot_result_length] = 0;
}

static const char *hook_depot_status_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    if (depot_child <= 0) return "error no download is running";
    read_depot_output();
    int status = 0;
    pid_t done = waitpid(depot_child, &status, WNOHANG);
    if (done == 0) return "running";
    read_depot_output();
    close(depot_output);
    depot_output = -1;
    depot_child = -1;
    depot_result[strcspn(depot_result, "\r\n")] = 0;
    int ok = done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             strncmp(depot_result, "ok ", 3) == 0;
    if (!ok && strncmp(depot_result, "error ", 6) != 0)
        snprintf(depot_result, sizeof(depot_result), "error the depot helper failed");
    fprintf(stderr, "[t2_hook] TribesNext depot: %s\n", depot_result);
    return depot_result;
}

static int console_command_table_ready(uint32_t *table_size) {
    int protection = 0;
    if (!target) return 0;
    if (!readable_range(target->string_table, sizeof(uint32_t), &protection)) return 0;
    uint32_t console_ptr = *(volatile uint32_t *)target->string_table;
    if (console_ptr > UINT32_MAX - CONSOLE_TABLE_SIZE_OFFS) return 0;
    if (!console_ptr || !readable_range(console_ptr + CONSOLE_TABLE_SIZE_OFFS,
                                        sizeof(uint32_t), &protection)) return 0;
    uint32_t size = *(volatile uint32_t *)(console_ptr + CONSOLE_TABLE_SIZE_OFFS);
    if (!size) return 0;
    if (table_size) *table_size = size;
    return 1;
}

static int register_early_compat_commands(void) {
    if (early_compat_commands_registered || early_compat_commands_registering) return 1;
    uint32_t table_size = 0;
    if (!console_command_table_ready(&table_size)) return 0;
    if (!Con__addMethodS) Con__addMethodS = (con_addMethodS_t)target->con_add_method;
    if (!Con__addMethodS) return 0;
    early_compat_commands_registering = 1;
    Con__addMethodS(NULL, "memPatch", mem_patch_cb,
        "memPatch <hex-address> <hex-patch> [sha256:<64-hex>]", 3, 4);
    if (!target->dedicated) {
        Con__addMethodS(NULL, "hook_hud_demo_heading", hook_hud_demo_heading_cb,
            "hook_hud_demo_heading() - native heading for the HUD demo compass", 1, 1);
        Con__addMethodS(NULL, "hook_hud_demo_compass_markers", hook_hud_demo_compass_markers_cb,
            "hook_hud_demo_compass_markers(north, east, south, west, strip)", 6, 6);
    }
    early_compat_commands_registering = 0;
    early_compat_commands_registered = 1;
    fprintf(stderr, "[t2_hook] Registered early script compatibility commands (table_size=%u)\n",
            table_size);
    return 1;
}

static int register_hook_commands(void) {
    if (registered) return 1;
    uint32_t table_size = 0;
    if (!console_command_table_ready(&table_size)) return 0;

    Con__addMethodS = (con_addMethodS_t)target->con_add_method;
    (void)register_early_compat_commands();
    /* Replaces the stock no-op; not one of the optional hook_* commands. */
    if (!target->dedicated && hook_option_bool("video.vsync"))
        Con__addMethodS(NULL, "setVerticalSync", set_vertical_sync_cb,
            "setVerticalSync( <bool> )", 2, 2);
    if (hud_bar_store_value)
        Con__addMethodS("HudBarBaseCtrl", "getValue", hud_bar_get_value_cb,
            "bar.getValue() - the bar's current fill, 0 to 1", 2, 2);
    /* A safety override, not one of the optional hook_* commands. */
    if (!target->dedicated && hook_option_bool("security.safe_soundpacks"))
        install_safe_soundpacks();
    if (!hook_option_bool("hook.console_commands")) {
        registered = 1;  /* the console is ready; only the commands are skipped */
        fprintf(stderr, "[t2_hook] Console commands disabled by hook.console_commands\n");
        return 1;
    }
    Con__addMethodS(NULL, "hook_test", hook_test_cb, "hook_test - test command", 1, 1);
    Con__addMethodS(NULL, "hook_option", hook_option_cb,
        "hook_option <key> - value of a T2Launch hook option", 2, 2);
    if (!target->dedicated && hook_option_bool("telemetry.shazbot_command_labels")) {
        Con__addMethodS(NULL, "shazbot_command_label", shazbot_command_label_cb,
            "shazbot_command_label <id> <pressed> - record an opted-in command label", 3, 3);
        Con__addMethodS(NULL, "shazbot_test_weapon_slot", shazbot_test_weapon_slot_cb,
            "shazbot_test_weapon_slot <slot-id-0-to-5> - label and send one real weapon-slot command", 2, 2);
        Con__addMethodS(NULL, "shazbot_demo_record_start", shazbot_demo_record_start_cb,
            "shazbot_demo_record_start - mark a demo recording for Shazbot labels", 1, 1);
        Con__addMethodS(NULL, "shazbot_demo_record_stop", shazbot_demo_record_stop_cb,
            "shazbot_demo_record_stop - stop Shazbot command labels for this recording", 1, 1);
        Con__addMethodS(NULL, "shazbot_command_capture_start", shazbot_command_capture_start_cb,
            "shazbot_command_capture_start - mark a direct command-label capture", 1, 1);
        Con__addMethodS(NULL, "shazbot_command_capture_stop", shazbot_command_capture_stop_cb,
            "shazbot_command_capture_stop - finish a direct command-label capture", 1, 1);
    }
    if (!target->dedicated && hook_option_bool("telemetry.shazbot")) {
        Con__addMethodS(NULL, "shazbot_hud_weapon_clear", shazbot_hud_weapon_clear_cb,
            "shazbot_hud_weapon_clear() - reset HUD-mirrored weapon inventory", 1, 1);
        Con__addMethodS(NULL, "shazbot_hud_weapon_item", shazbot_hud_weapon_item_cb,
            "shazbot_hud_weapon_item(hudSlot, name, addItem) - mirror a HUD weapon inventory update", 4, 4);
        Con__addMethodS(NULL, "shazbot_hud_weapon_active", shazbot_hud_weapon_active_cb,
            "shazbot_hud_weapon_active(name) - mirror the selected HUD weapon", 2, 2);
    }
    if (!target->dedicated) {
        Con__addMethodS(NULL, "hook_review_range", hook_review_range_cb,
            "hook_review_range([enabled]) - query or change demo-review range for this session", 1, 2);
        Con__addMethodS(NULL, "hook_review_global_beacons", hook_review_global_beacons_cb,
            "hook_review_global_beacons([enabled]) - query or change demo beacon visibility for this session", 1, 2);
        Con__addMethodS(NULL, "hook_review_ffi", hook_review_ffi_cb,
            "hook_review_ffi([enabled]) - query or change demo IFF reconstruction for this session", 1, 2);
        Con__addMethodS(NULL, "hook_review_no_cloak", hook_review_no_cloak_cb,
            "hook_review_no_cloak([enabled]) - query or change demo no-cloak reconstruction for this session", 1, 2);
        Con__addMethodS(NULL, "hook_review_autobeacon", hook_review_autobeacon_cb,
            "hook_review_autobeacon([enabled]) - query or change demo target reassignment for this session", 1, 2);
        Con__addMethodS(NULL, "hook_review_models", hook_review_models_cb,
            "hook_review_models([filename filter]) - log cached DTS backing files without loading or changing models", 1, 2);
        Con__addMethodS(NULL, "hook_review_textures", hook_review_textures_cb,
            "hook_review_textures([key filter]) - log face/HUD texture cache and actual beacon HUD handles", 1, 2);
    }
    Con__addMethodS(NULL, "hook_sha1", hook_sha1_cb, "hook_sha1 <string> - SHA1 hash", 2, 2);
    Con__addMethodS(NULL, "hook_rsa_mod_exp", hook_rsa_mod_exp_cb, "hook_rsa_mod_exp <base> <exp> <mod>", 4, 4);
    Con__addMethodS(NULL, "hook_rand64", hook_rand64_cb, "hook_rand64 - random 64-bit hex", 1, 1);
    Con__addMethodS(NULL, "hook_profile_browser_wake", hook_profile_browser_wake_cb,
        "hook_profile_browser_wake - confirm runtime browser patch", 1, 1);
    Con__addMethodS(NULL, "hook_tn_unlock", hook_tn_unlock_cb,
        "hook_tn_unlock <username> <password> - unlock installed TribesNext key", 3, 3);
    Con__addMethodS(NULL, "hook_tn_certificate_length", hook_tn_certificate_length_cb,
        "hook_tn_certificate_length - get installed certificate length", 1, 1);
    Con__addMethodS(NULL, "hook_tn_certificate_chunk", hook_tn_certificate_chunk_cb,
        "hook_tn_certificate_chunk <offset> - get a safe certificate chunk", 2, 2);
    Con__addMethodS(NULL, "hook_tn_decrypt", hook_tn_decrypt_cb,
        "hook_tn_decrypt <hex> - decrypt TribesNext server challenge", 2, 2);
    Con__addMethodS(NULL, "hook_tn_clear", hook_tn_clear_cb,
        "hook_tn_clear - clear in-memory TribesNext credentials", 1, 1);
    Con__addMethodS(NULL, "hook_tn_trace", hook_tn_trace_cb,
        "hook_tn_trace <known_stage> [safe_state...] - report auth stages", 2, 7);
    Con__addMethodS(NULL, "hook_tn_join_trace", hook_tn_join_trace_cb,
        "hook_tn_join_trace <address> <ready> - safe join diagnostics", 3, 3);
    Con__addMethodS(NULL, "hook_tn_ip_trace", hook_tn_ip_trace_cb,
        "hook_tn_ip_trace <ip> <hex> - safe challenge address diagnostics", 3, 3);
    Con__addMethodS(NULL, "hook_tn_ipv4_hex", hook_tn_ipv4_hex_cb,
        "hook_tn_ipv4_hex <dotted_ipv4> - encode IPv4 in T2CSRI hex order", 2, 2);
    Con__addMethodS(NULL, "hook_tn_hex_ipv4", hook_tn_hex_ipv4_cb,
        "hook_tn_hex_ipv4 <hex> - decode a T2CSRI hex IPv4 address", 2, 2);
    Con__addMethodS(NULL, "hook_tn_verify_cert", hook_tn_verify_cert_cb,
        "hook_tn_verify_cert <certificate> - 1 if TribesNext signed the account certificate", 2, 2);
    if (!target->dedicated && hook_option_bool("net.depot_downloads")) {
        Con__addMethodS(NULL, "hook_depot_fetch", hook_depot_fetch_cb,
            "hook_depot_fetch <file> - fetch a missing map file from the TribesNext depot", 2, 2);
        Con__addMethodS(NULL, "hook_depot_status", hook_depot_status_cb,
            "hook_depot_status - running, ok <path> or error <reason>", 1, 1);
    }
    registered = 1;
    fprintf(stderr, "[t2_hook] Registered the hook commands on the game thread (table_size=%d)\n", table_size);
    return 1;
}

static int sdl_modes_is_any(sdl_rect_t **modes) {
    return modes == (sdl_rect_t **)(intptr_t)-1;
}

static sdl_rect_t **prefer_concrete_host_modes(sdl_rect_t **requested_modes,
                                                sdl_rect_t **fullscreen_modes) {
    if (!sdl_modes_is_any(requested_modes) || !fullscreen_modes ||
        sdl_modes_is_any(fullscreen_modes) || !fullscreen_modes[0])
        return requested_modes;
    return fullscreen_modes;
}

static unsigned int sdl_mode_count(sdl_rect_t **modes) {
    unsigned int count = 0;
    if (!modes || sdl_modes_is_any(modes)) return 0;
    while (modes[count] && count < 256) count++;
    return count;
}

#ifndef T2_HOOK_TEST
sdl_rect_t **SDL_ListModes(void *format, uint32_t flags) {
    if (!real_sdl_list_modes)
        real_sdl_list_modes = (sdl_list_modes_t)next_symbol("SDL_ListModes");
    if (!real_sdl_list_modes) return NULL;

    sdl_rect_t **modes = real_sdl_list_modes(format, flags);
    if (!sdl_modes_is_any(modes) || (flags & SDL_FULLSCREEN_FLAG) ||
        !hook_option_bool("video.xrandr_modes"))
        return modes;

    /* Tribes 2 treats SDL's windowed "any size" sentinel as a cue to show its
       small built-in 4:3 list. Ask SDL/XRandR for actual fullscreen modes and
       hand that concrete host-supported list back to the game's enumerator. */
    sdl_rect_t **fullscreen_modes = real_sdl_list_modes(
        format, flags | SDL_FULLSCREEN_FLAG);
    sdl_rect_t **selected = prefer_concrete_host_modes(modes, fullscreen_modes);
    if (selected != modes && !xrandr_mode_list_reported) {
        xrandr_mode_list_reported = 1;
        fprintf(stderr, "[t2_hook] Using %u host XRandR modes instead of Tribes 2's windowed fallback\n",
                sdl_mode_count(selected));
    }
    return selected;
}

static void __attribute__((constructor)) init(void) {
    if (!is_game_process()) return;
    game_main_thread = pthread_self();
    hook_started_at = time(NULL);
    char startup_dir[4096];
    game_path(startup_dir, sizeof(startup_dir), "");  /* capture cwd before the game can chdir */
    const char *won = getenv("T2LAUNCH_WONEMU");
    if (!is_supported_game()) {
        fprintf(stderr, "[t2_hook] Unsupported %s: console address check failed\n", target->description);
        if (won && strcmp(won, "1") == 0) _exit(1);
        return;
    }
    Con__evaluate = (con_evaluate_t)target->con_evaluate;
    Con__addMethodS = (con_addMethodS_t)target->con_add_method;
    if (!install_console_evaluate_gate())
        fprintf(stderr, "[t2_hook] Early memPatch registration unavailable; using main-loop registration fallback\n");
    fprintf(stderr, "[t2_hook] Target: %s (%s)\n", target->executable, target->description);
    if (won && strcmp(won, "1") == 0) {
        /* The client copies the embedded verifier during its own global
           initialization, and console_start.cs calls WONInit(), which reads
           the bootstrap strings, before createCanvas(). Patch both now. */
        if (!install_won_verifier() || !redirect_won_bootstrap()) _exit(1);
    }
    pthread_once(&hook_options_once, load_hook_options);
    /* Before main() calibrates or reads the timer, so game time never jumps. */
    if (hook_option_bool("runtime.monotonic_clock")) install_monotonic_clock();
    if (hook_option_bool("net.packet_limits")) install_net_packet_limits();
    install_build_identifier();
    install_demo_protocol_tag();
    if (target->dedicated) {
        game_main_thread_known = 1;  /* constructors run on the main thread */
        /* A headless server's stdout is a pipe or file, which the game would
           fill in 4 KB blocks and lose on a crash; write its log by line. */
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (hook_option_bool("server.tribesnext_auth") && hook_option_bool("hook.console_commands")) {
            install_server_tribesnext_auth();
            use_detected_public_address();
        }
        fprintf(stderr, "[t2_hook] Console command registration deferred to the server's main loop\n");
        return;
    }
    install_sky_rotation_hook();
    if (hook_option_bool("browser.many_bots")) install_browser_bot_limit();
    install_review_model_selection();
    if (hook_option_bool("ui.hud_get_value") || hook_option_bool("ui.vertical_fill") ||
        hook_option_bool("ui.bar_flip"))
        install_hud_bars(hook_option_bool("ui.hud_get_value"), hook_option_bool("ui.vertical_fill"),
                         hook_option_bool("ui.bar_flip"));
    if (hook_option_bool("ui.observer_iff")) install_observer_iff();
    if (hook_option_bool("ui.lan_join_anywhere")) install_lan_join_anywhere();
    if (hook_option_bool("ui.visible_distance")) install_visible_distance();
    if (hook_option_bool("ui.no_whiteout")) install_no_whiteout();
    if (hook_option_bool("ui.fov_range")) install_fov_range();
    if (hook_option_bool("ui.client_datablocks")) install_client_datablocks();
    if (hook_option_bool("net.native_auth") && hook_option_bool("hook.tribesnext"))
        install_native_auth();
    /* SDL reads this when it creates the window; a borderless window has no
       title bar to move it by. */
    if (hook_option_bool("video.borderless")) setenv("SDL_VIDEO_CENTERED", "1", 0);
    if (hook_option_bool("browser.web_links")) {
        const char *browser = getenv("LOKI_BROWSER");
        if (!browser || !browser[0]) setenv("LOKI_BROWSER", WEB_LINK_MARKER "%s", 1);
    }
    fprintf(stderr, "[t2_hook] Console command registration deferred to the game thread\n");
}
#endif
