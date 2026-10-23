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
#include <errno.h>
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

static con_addMethodS_t Con__addMethodS = NULL;
static con_evaluate_t Con__evaluate = (con_evaluate_t)0x080E88F4;
static sdl_gl_swap_buffers_t real_sdl_gl_swap_buffers = NULL;
static sdl_set_video_mode_t real_sdl_set_video_mode = NULL;
static sdl_list_modes_t real_sdl_list_modes = NULL;
static pthread_t game_main_thread;
static volatile int registered = 0;
static volatile int game_main_thread_known = 0;
static int register_hook_commands(void);
static volatile int profile_browser_wake_seen = 0;
static volatile int core_runtime_installed = 0;
static volatile int tribesnext_runtime_installed = 0;
static volatile int shell_audio_setup_done = 0;
static volatile int player_pane_override_installed = 0;
static volatile int filter_max_bots_override_installed = 0;
static volatile int net_options_override_installed = 0;
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

static int open_web_link(const char *url) {
    /* xdg-open and the browser are 64-bit; leave the game's loader settings behind. */
    size_t count = 0;
    while (environ[count]) count++;
    char **env = malloc((count + 1) * sizeof(*env));
    if (!env) return -1;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(environ[i], "LD_PRELOAD=", 11) != 0 && strncmp(environ[i], "LD_AUDIT=", 9) != 0 &&
            strncmp(environ[i], "LD_LIBRARY_PATH=", 16) != 0)
            env[kept++] = environ[i];
    env[kept] = NULL;
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

#define CONSOLE_GLOBAL 0x8493dec
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
    static const uintptr_t addresses[] = {0x084846B1, 0x084846CD, 0x084846E9};
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
#define REAL_MILLISECONDS_ADDRESS 0x081CFA44
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
    uintptr_t address = REAL_MILLISECONDS_ADDRESS;
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

/* The client clamps $pref::Net::PacketRateToClient (0x08495CEC) to 32 and
   $pref::Net::PacketSize (0x08495CF0) to 450 in three places: the
   NetConnection constructor and two rate setters. The clamped values become
   the rate and size the client asks the server for; a stock server still
   sends at most its own rate and size, while QoL-patched TribesNext servers
   accept more. The rate compare takes a sign-extended imm8, so 127 is the
   largest possible limit. */
#define NET_PACKET_RATE_LIMIT 64
#define NET_PACKET_SIZE_LIMIT 1000
static const uintptr_t net_rate_compare_sites[] = {0x081F8EAE, 0x081FB382, 0x081FB683};
static const uintptr_t net_rate_store_sites[] = {0x081F8EB7, 0x081FB38B, 0x081FB68C};
static const uintptr_t net_size_compare_sites[] = {0x081F8ED4, 0x081FB3A8, 0x081FB6A9};
static const uintptr_t net_size_store_sites[] = {0x081F8EE0, 0x081FB3B4, 0x081FB6B5};
static const unsigned char net_rate_compare_original[] = {0x83, 0x3d, 0xec, 0x5c, 0x49, 0x08, 0x20};
static const unsigned char net_rate_store_original[] =
    {0xc7, 0x05, 0xec, 0x5c, 0x49, 0x08, 0x20, 0x00, 0x00, 0x00};
static const unsigned char net_size_compare_original[] =
    {0x81, 0x3d, 0xf0, 0x5c, 0x49, 0x08, 0xc2, 0x01, 0x00, 0x00};
static const unsigned char net_size_store_original[] =
    {0xc7, 0x05, 0xf0, 0x5c, 0x49, 0x08, 0xc2, 0x01, 0x00, 0x00};

static int install_net_packet_limits(void) {
    static const unsigned char rate8[] = {NET_PACKET_RATE_LIMIT};
    static const unsigned char rate32[] = {NET_PACKET_RATE_LIMIT, 0, 0, 0};
    static const unsigned char size32[] = {NET_PACKET_SIZE_LIMIT & 0xff, NET_PACKET_SIZE_LIMIT >> 8, 0, 0};
    struct code_patch patches[12];
    size_t count = 0;
    for (size_t i = 0; i < 3; i++) {
        patches[count++] = (struct code_patch){net_rate_compare_sites[i], net_rate_compare_original,
            sizeof(net_rate_compare_original), 6, rate8, sizeof(rate8)};
        patches[count++] = (struct code_patch){net_rate_store_sites[i], net_rate_store_original,
            sizeof(net_rate_store_original), 6, rate32, sizeof(rate32)};
        patches[count++] = (struct code_patch){net_size_compare_sites[i], net_size_compare_original,
            sizeof(net_size_compare_original), 6, size32, sizeof(size32)};
        patches[count++] = (struct code_patch){net_size_store_sites[i], net_size_store_original,
            sizeof(net_size_store_original), 6, size32, sizeof(size32)};
    }
    if (!apply_code_patches("Network packet limit", patches, count)) return 0;
    fprintf(stderr, "[t2_hook] Network: client packet limits %d/s and %d bytes instead of 32/s and 450 "
            "(memory only)\n", NET_PACKET_RATE_LIMIT, NET_PACKET_SIZE_LIMIT);
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
    /* Key slot consumed by the WON global constructor at 0x08374BED. */
    if (count != sizeof(encoded) || extra != EOF || encoded[0] != 0x30 ||
        encoded[1] != 0x82 || !readable_range(0x0849A860, sizeof(encoded), &protection) ||
        memcmp((void *)0x0849A860, "\x30\x82\x03\x2c", 4) != 0) {
        fprintf(stderr, "[t2_hook] WON verifier refused: key/slot format mismatch\n");
        return 0;
    }
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return 0;
    uintptr_t page = 0x0849A860 & ~((uintptr_t)size - 1);
    if (0x0849A860 + sizeof(encoded) > page + (uintptr_t)size ||
        mprotect((void *)page, (size_t)size, protection | PROT_WRITE) != 0)
        return 0;
    memcpy((void *)0x0849A860, encoded, sizeof(encoded));
    if (mprotect((void *)page, (size_t)size, protection) != 0) return 0;
    fprintf(stderr, "[t2_hook] WON: installed local verifier key (signature checks remain active)\n");
    return 1;
}

static int is_game_process(void) {
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) return 0;
    executable[length] = 0;
    const char *name = strrchr(executable, '/');
    return name && strcmp(name + 1, "tribes2.dynamic") == 0;
}

static int is_supported_game(void) {
    int protection;
    /* Guard the console addresses as well as the executable basename. */
    return readable_range(0x080E85C0, 3, &protection) &&
        (protection & PROT_EXEC) && memcmp((void *)0x080E85C0, "\x55\x89\xe5", 3) == 0 &&
        readable_range(0x080E88F4, 3, &protection) &&
        (protection & PROT_EXEC) && memcmp((void *)0x080E88F4, "\x55\x89\xe5", 3) == 0 &&
        readable_range(CONSOLE_GLOBAL, sizeof(uint32_t), &protection);
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

static int register_hook_commands(void) {
    if (registered) return 1;
    uint32_t console_ptr = *(volatile uint32_t *)CONSOLE_GLOBAL;
    if (console_ptr == 0) return 0;
    uint32_t table_size = *(volatile uint32_t *)(console_ptr + CONSOLE_TABLE_SIZE_OFFS);
    if (table_size == 0) return 0;

    if (!hook_option_bool("hook.console_commands")) {
        registered = 1;  /* the console is ready; only the commands are skipped */
        fprintf(stderr, "[t2_hook] Console commands disabled by hook.console_commands\n");
        return 1;
    }
    Con__addMethodS = (con_addMethodS_t)0x80E85C0;
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
    registered = 1;
    fprintf(stderr, "[t2_hook] Registered 15 commands on the game thread (table_size=%d)\n", table_size);
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
        fprintf(stderr, "[t2_hook] Unsupported client: console address check failed\n");
        if (won && strcmp(won, "1") == 0) _exit(1);
        return;
    }
    if (won && strcmp(won, "1") == 0) {
        /* The client copies the embedded verifier during its own global
           initialization, and console_start.cs calls WONInit(), which reads
           the bootstrap strings, before createCanvas(). Patch both now. */
        if (!install_won_verifier() || !redirect_won_bootstrap()) _exit(1);
    }
    pthread_once(&hook_options_once, load_hook_options);
    /* Before main() calibrates or reads the timer, so game time never jumps. */
    if (hook_option_bool("runtime.monotonic_clock")) install_monotonic_clock();
    if (hook_option_bool("browser.many_bots")) install_browser_bot_limit();
    if (hook_option_bool("net.packet_limits")) install_net_packet_limits();
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
