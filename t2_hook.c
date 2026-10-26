#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef int S32;

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
};

static const struct game_target game_targets[] = {
    {"tribes2.dynamic", "client", 0, 0x080E85C0, 0x080E88F4, 0x08493DEC, 0x081CFA44,
     {0x084846B1, 0x084846CD, 0x084846E9}, 0x0849A860, 0x08495CEC, 0x08495CF0,
     {0x081F8EAE, 0x081FB382, 0x081FB683}},
    {"tribes2d.dynamic", "dedicated server", 1, 0x080E74B0, 0x080E77E4, 0x0848902C, 0x081CD014,
     {0x08479751, 0x0847976D, 0x08479789}, 0x0848F520, 0x0848ADCC, 0x0848ADD0,
     {0x081EDB4E, 0x081F0022, 0x081F0323}},
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
static int set_swap_interval(int interval);
static volatile int vsync_requested = -1;   /* -1: never set by the game */
static volatile int vsync_reapply = 0;
static volatile int profile_browser_wake_seen = 0;
static volatile int core_runtime_installed = 0;
static volatile int tribesnext_runtime_installed = 0;
static volatile int shell_audio_setup_done = 0;
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
    {"video.fps_limit", "0", NULL, 0, "", 0},
    {"video.borderless", "0", NULL, 0, "", 0},
    {"video.vsync", "1", NULL, 0, "", 0},
    {"video.gamma", "1", NULL, 0, "", 0},
    {"video.disable_locked_arrays", "0", NULL, 0, "", 0},
    {"net.depot_downloads", "0", NULL, 0, "", 0},
    {"net.native_auth", "0", NULL, 0, "", 0},
    {"discord.presence", "0", NULL, 0, "", 0},
    {"discord.app_id", "1563045749846056970", NULL, 0, "", 0},
    {"ui.hud_get_value", "1", NULL, 0, "", 0},
    {"ui.vertical_fill", "1", NULL, 0, "", 0},
    {"ui.bar_flip", "1", NULL, 0, "", 0},
    {"ui.observer_iff", "0", NULL, 0, "", 0},
    {"ui.lan_join_anywhere", "0", NULL, 0, "", 0},
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
    if (!real_system) real_system = (int (*)(const char *))dlsym(RTLD_NEXT, "system");
    return real_system ? real_system(command) : -1;
}

/* Installed whenever the hook runs: helpers the hook itself evaluates, the
 * server-browser repaint, and a provider entry for an empty audio menu. */
static const char core_runtime_script[] =
    "function t2launch_launchGuiReady() {\n"
    " if (isObject(LaunchGui)) return \"1\";\n"
    " return \"0\";\n"
    "}\n"
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
    long kbytes = strtol(hook_option_str("security.max_soundpack_kbytes"), NULL, 10);
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
        real_getaddrinfo = (getaddrinfo_fn)dlsym(RTLD_NEXT, "getaddrinfo");
    trace_resolver_name(node, service);
    if (!real_getaddrinfo) return EAI_SYSTEM;
    if (is_won_bootstrap_host(node)) node = "127.0.0.1";
    return real_getaddrinfo(node, service, hints, results);
}

struct hostent *gethostbyname(const char *name) {
    typedef struct hostent *(*gethostbyname_fn)(const char *);
    static gethostbyname_fn real_gethostbyname;
    if (!real_gethostbyname)
        real_gethostbyname = (gethostbyname_fn)dlsym(RTLD_NEXT, "gethostbyname");
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
        if (!real_create) real_create = (pthread_create_t)dlsym(RTLD_NEXT, "pthread_create");
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
    if (!real_join) real_join = (pthread_join_t)dlsym(RTLD_NEXT, "pthread_join");
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
    if (!real_detach) real_detach = (pthread_detach_t)dlsym(RTLD_NEXT, "pthread_detach");
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

static volatile float gamma_requested = 1.0f;
static int gamma_width, gamma_height;

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
    unsigned program, texture;
    int texture_width, texture_height, exponent_location;
} gl_gamma;

static const char gamma_vertex_shader[] =
    "void main() { gl_TexCoord[0] = gl_MultiTexCoord0; gl_Position = gl_Vertex; }\n";
static const char gamma_fragment_shader[] =
    "uniform sampler2D frame; uniform float exponent;\n"
    "void main() {\n"
    " vec3 colour = texture2D(frame, gl_TexCoord[0].xy).rgb;\n"
    " gl_FragColor = vec4(pow(colour, vec3(exponent)), 1.0);\n"
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
            fprintf(stderr, "[t2_hook] Gamma unavailable: no %s\n", name); \
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
    GL_FN(Uniform1f, "glUniform1f");
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
            fprintf(stderr, "[t2_hook] Gamma unavailable: shader did not compile\n");
            return 0;
        }
        gl_gamma.AttachShader(program, shader);
    }
    int linked = 0;
    gl_gamma.LinkProgram(program);
    gl_gamma.GetProgramiv(program, GL_LINK_STATUS_, &linked);
    if (!linked) {
        fprintf(stderr, "[t2_hook] Gamma unavailable: shader did not link\n");
        return 0;
    }
    gl_gamma.program = program;
    gl_gamma.exponent_location = gl_gamma.GetUniformLocation(program, "exponent");
    int previous = 0;
    gl_gamma.GetIntegerv(GL_CURRENT_PROGRAM_, &previous);
    gl_gamma.UseProgram(program);
    gl_gamma.Uniform1i(gl_gamma.GetUniformLocation(program, "frame"), 0);
    gl_gamma.UseProgram((unsigned)previous);
    gl_gamma.state = 1;
    fprintf(stderr, "[t2_hook] Gamma correction drawn by a shader pass\n");
    return 1;
}

/* Called before each swap on the game thread with the game's context current. */
static void apply_gamma_pass(void) {
    float gamma = gamma_requested;
    if (gamma > 0.999f && gamma < 1.001f) return;
    if (gamma_width <= 0 || gamma_height <= 0 || !resolve_gl_gamma()) return;
    int previous_program = 0, previous_unit = 0;
    gl_gamma.GetIntegerv(GL_CURRENT_PROGRAM_, &previous_program);
    gl_gamma.GetIntegerv(GL_ACTIVE_TEXTURE_, &previous_unit);
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
    gl_gamma.Viewport(0, 0, gamma_width, gamma_height);
    if (!gl_gamma.texture) gl_gamma.GenTextures(1, &gl_gamma.texture);
    gl_gamma.Enable(GL_TEXTURE_2D_);
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
    gl_gamma.UseProgram(gl_gamma.program);
    gl_gamma.Uniform1f(gl_gamma.exponent_location, 1.0f / gamma);
    gl_gamma.Begin(GL_QUADS_);
    gl_gamma.TexCoord2f(0, 0); gl_gamma.Vertex2f(-1, -1);
    gl_gamma.TexCoord2f(1, 0); gl_gamma.Vertex2f(1, -1);
    gl_gamma.TexCoord2f(1, 1); gl_gamma.Vertex2f(1, 1);
    gl_gamma.TexCoord2f(0, 1); gl_gamma.Vertex2f(-1, 1);
    gl_gamma.End();
    gl_gamma.UseProgram((unsigned)previous_program);
    gl_gamma.MatrixMode(GL_TEXTURE_); gl_gamma.PopMatrix();
    gl_gamma.MatrixMode(GL_PROJECTION_); gl_gamma.PopMatrix();
    gl_gamma.MatrixMode(GL_MODELVIEW_); gl_gamma.PopMatrix();
    gl_gamma.PopClientAttrib();
    gl_gamma.PopAttrib();
    gl_gamma.ActiveTexture((unsigned)previous_unit);
}

int SDL_SetGamma(float red, float green, float blue) {
    static int (*real_set_gamma)(float, float, float);
    if (!hook_option_bool("video.gamma")) {
        if (!real_set_gamma) real_set_gamma = (int (*)(float, float, float))dlsym(RTLD_NEXT, "SDL_SetGamma");
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
    if (!real_get_proc) real_get_proc = (void *(*)(const char *))dlsym(RTLD_NEXT, "SDL_GL_GetProcAddress");
    if (!real_get_proc) return NULL;
    void *address = real_get_proc(proc);
    if (!proc || !hook_option_bool("video.disable_locked_arrays")) return address;
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

/* Run deferred TorqueScript changes after a rendered frame on the game thread. */
void SDL_GL_SwapBuffers(void) {
    if (!real_sdl_gl_swap_buffers)
        real_sdl_gl_swap_buffers = (sdl_gl_swap_buffers_t)dlsym(
            RTLD_NEXT, "SDL_GL_SwapBuffers");
    if (!real_sdl_gl_swap_buffers) return;

    if (game_main_thread_known && pthread_equal(pthread_self(), game_main_thread))
        apply_gamma_pass();
    real_sdl_gl_swap_buffers();
    cap_frame_rate();
    if (!sdl_gl_swap_hook_reported) {
        sdl_gl_swap_hook_reported = 1;
        game_main_thread = pthread_self();
        game_main_thread_known = 1;
        fprintf(stderr, "[t2_hook] SDL_GL_SwapBuffers main-loop hook active\n");
    }
    if (!registered)
        register_hook_commands();
    if (!registered || !Con__evaluate ||
        !game_main_thread_known ||
        !pthread_equal(pthread_self(), game_main_thread))
        return;

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
            shell_audio_setup_done = 1;
        }
    }
    if (shell_audio_setup_done && now != (time_t)-1)
        poll_discord_presence(now);
}

/* The dedicated server never swaps buffers. Platform::process (0x081CF400)
   ends each main-loop pass with usleep(1000), so the hook's main-thread work
   runs there instead. */
static void server_tick(void) {
    static int core_installed, tribesnext_installed;
    static time_t last_ready_check;
    if (!registered)
        register_hook_commands();
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
    if (!real_usleep) real_usleep = (int (*)(useconds_t))dlsym(RTLD_NEXT, "usleep");
    if (target->dedicated && game_main_thread_known && pthread_equal(pthread_self(), game_main_thread))
        server_tick();
    return real_usleep ? real_usleep(microseconds) : 0;
}

/* Observe SDL 1.2 mode changes without replacing its return surface or flags. */
void *SDL_SetVideoMode(int width, int height, int bpp, uint32_t flags) {
    if (!real_sdl_set_video_mode)
        real_sdl_set_video_mode = (sdl_set_video_mode_t)dlsym(
            RTLD_NEXT, "SDL_SetVideoMode");
    if (!real_sdl_set_video_mode) return NULL;
    if (!(flags & SDL_FULLSCREEN_FLAG) && hook_option_bool("video.borderless"))
        flags |= SDL_NOFRAME_FLAG;
    void *surface = real_sdl_set_video_mode(width, height, bpp, flags);
    if (surface && vsync_requested >= 0) vsync_reapply = 1;
    if (surface) {
        /* A new context needs a new texture; the old one went with its context. */
        gamma_width = width;
        gamma_height = height;
        gl_gamma.texture = 0;
        gl_gamma.texture_width = gl_gamma.texture_height = 0;
        if (gl_gamma.state > 0) gl_gamma.state = 0;
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

/* ── Big Number ops for RSA (kept minimal to avoid stack issues) ────── */
/* Full bn_ functions removed - using inline code in callback */

/* ── Console Callbacks ──────────────────────────────────────────────── */

static const char* hook_test_cb(void *obj, S32 argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    return "ok_from_c";
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

static int register_hook_commands(void) {
    if (registered) return 1;
    uint32_t console_ptr = *(volatile uint32_t *)target->string_table;
    if (console_ptr == 0) return 0;
    uint32_t table_size = *(volatile uint32_t *)(console_ptr + CONSOLE_TABLE_SIZE_OFFS);
    if (table_size == 0) return 0;

    Con__addMethodS = (con_addMethodS_t)target->con_add_method;
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
        real_sdl_list_modes = (sdl_list_modes_t)dlsym(RTLD_NEXT, "SDL_ListModes");
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
    if (hook_option_bool("browser.many_bots")) install_browser_bot_limit();
    if (hook_option_bool("ui.hud_get_value") || hook_option_bool("ui.vertical_fill") ||
        hook_option_bool("ui.bar_flip"))
        install_hud_bars(hook_option_bool("ui.hud_get_value"), hook_option_bool("ui.vertical_fill"),
                         hook_option_bool("ui.bar_flip"));
    if (hook_option_bool("ui.observer_iff")) install_observer_iff();
    if (hook_option_bool("ui.lan_join_anywhere")) install_lan_join_anywhere();
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
