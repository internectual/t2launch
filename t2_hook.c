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

typedef int S32;

typedef void (*con_addMethodS_t)(const char *nsName, const char *name,
    const char *(*cb)(void *obj, S32 argc, const char **argv),
    const char *usage, S32 minArgs, S32 maxArgs);
typedef const char *(*con_evaluate_t)(const char *code, unsigned char echo,
    const char *fileName, unsigned char noCalls);
typedef int (*sdl_poll_event_t)(void *event);
typedef void (*sdl_gl_swap_buffers_t)(void);

static con_addMethodS_t Con__addMethodS = NULL;
static con_evaluate_t Con__evaluate = (con_evaluate_t)0x080E88F4;
static sdl_poll_event_t real_sdl_poll_event = NULL;
static sdl_gl_swap_buffers_t real_sdl_gl_swap_buffers = NULL;
static pthread_t game_main_thread;
static volatile int registered = 0;
static volatile int game_main_thread_known = 0;
static int won_redirected = 0;
static char won_endpoint[28] = "127.0.0.1:15104";
static time_t hook_started_at = 0;
static time_t last_player_pane_override = 0;
static int sdl_poll_hook_reported = 0;
static int sdl_gl_swap_hook_reported = 0;

#define CONSOLE_GLOBAL 0x8493dec
#define CONSOLE_TABLE_SIZE_OFFS 4

/* PlayerPane::ButtonClick(2) trusts memberships embedded in AuthInfo, so the
 * local account's branch never sends query 13 and leaves the wait cursor active.
 * Preserve the other tabs but always fetch the tribe list through DBProxy. */
static const char player_pane_button_click_override[] =
    "function PlayerPane::ButtonClick(%this, %senderid) {"
    " canvas.SetCursor(ArrowWaitCursor);"
    " %this.key = LaunchGui.key++; %this.tabstate = \"WARRIOR\";"
    " if (isEventPending(TribeAndWarriorBrowserGui.eid)) cancel(TribeAndWarriorBrowserGui.eid);"
    " switch (%senderid) {"
    " case 0:"
    "  W_Text.setValue(\"\"); %this.state = \"getWarriorProfile\"; %this.qrystatus = 0;"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());"
    "  if (TWBTitle.OldText $= TWBTitle.name || W_MemberList.rowCount() == 0)"
    "   %this.NeedRefresh = 0; else %this.NeedRefresh = 1;"
    "  TWBTitle.OldText = TWBTitle.name;"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQuery,"
    "   23, %playerName, %this, %this.key);"
    " case 1:"
    "  W_Text.setValue(\"\"); %this.state = \"getWarriorHistory\"; %this.qrystatus = 1;"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQueryArray,"
    "   12, 0, %playerName, %this, %this.key, true);"
    " case 2:"
    "  W_MemberList.Clear(); W_MemberList.ClearColumns(); W_MemberList.clearList();"
    "  W_MemberList.CID = 0; %this.wtid = 0; %this.state = \"getWarriorTribeList\";"
    "  W_MemberList.addColumn(0, \"TRIBE\", 94, 0, 330);"
    "  W_MemberList.addColumn(1, \"TITLE\", 80, 0, 300);"
    "  W_MemberList.addColumn(2, \"RNK\", 38, 0, 50, \"numeric center\");"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQueryArray,"
    "   13, 0, %playerName, %this, %this.key, true);"
    " case 3:"
    "  W_MemberList.Clear(); W_MemberList.ClearColumns(); W_MemberList.clearList();"
    "  W_MemberList.CID = 1; W_MemberList.addColumn(0, \"BUDDY\", 100, 0, 250);"
    "  W_MemberList.addColumn(1, \"SINCE\", 112, 0, 250);"
    "  %this.key = LaunchGui.key++; %this.state = \"getWarriorBuddyList\";"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQueryArray,"
    "   5, 0, %playerName, %this, %this.key, true);"
    " case 4:"
    "  W_Text.setValue(\"\"); %this.state = \"getVisitorOptions\";"
    "  %playerName = TWBTabView.getTabText(TWBTabView.GetSelectedID());"
    "  TWBTitle.OldText = TWBTitle.name;"
    "  TribeAndWarriorBrowserGui.eid = schedule(500, 0, DatabaseQuery,"
    "   23, %playerName, %this, %this.key, true);"
    " case 5:"
    "  if (trim(W_Text.getText()) !$= \"\") {"
    "   WarriorPropertiesDlg.pendingChanges = \"\"; Canvas.PushDialog(WarriorPropertiesDlg);"
    "  } else {"
    "   W_Profile.setValue(1);"
    "   messageBoxOK(\"ERROR\", \"Your Profile was not loaded properly, Please wait a moment and try again\");"
    "  }"
    " }"
    "}"
    "function PlayerPane::onDatabaseRow(%this, %row, %isLastRow, %key) {"
    " if (%this.key != %key) return;"
    " switch$(%this.state) {"
    " case \"warriorHistory\":"
    "  W_Text.setText(W_Text.getText() @ %row @ \"\\n\");"
    "  if (%isLastRow) { %this.state = \"done\";"
    "   if (%this.needRefresh) { %this.needRefresh = 0; W_tribes.setValue(1); }"
    "  }"
    " case \"warriorTribeList\":"
    "  %wid = getField(%row, 2); %name = getField(%row, 0);"
    "  %title = getField(%row, 5); %adminLevel = getField(%row, 3);"
    "  %editkick = getField(%row, 4);"
    "  if (%title $= \"\") %title = \"Not Shown\";"
    "  W_MemberList.AddMember(%this.wtid, %wid, %name, %adminLevel, %editkick, %row);"
    "  W_MemberList.AddRow(%wid, %name TAB %title TAB %adminLevel TAB %this.wtid);"
    "  %this.wtid++; Canvas.repaint();"
    "  if (%isLastRow) { %this.state = \"done\"; Canvas.setCursor(DefaultCursor); }"
    " case \"warriorBuddyList\":"
    "  W_MemberList.AddInvite(%this.blid, getField(%row, 3), getFields(%row, 0, 3),"
    "   getFields(%row, 0, 3), 4, %row);"
    "  W_MemberList.AddRow(getField(%row, 3), getField(%row, 0) TAB"
    "   getField(%row, 4) TAB %this.blid); %this.blid++;"
    "  if (%isLastRow) W_MemberList.getOnlineStatus();"
    " }"
    "}"
    "function TribeAndWarriorBrowserGui::onWake(%this) {"
    " MemberList.ClearColumns(); W_MemberList.ClearColumns();"
    " MemberList.Clear(); W_MemberList.clear(); Canvas.pushDialog(LaunchToolbarDlg);"
    " if (TWBTabView.tabCount() == 0) {"
    "  %info = WONGetAuthInfo(); %warrior = getField(%info, 0);"
    "  if (trim(%warrior) $= \"\") %warrior = $pref::LastLoginName;"
    "  if (trim(%warrior) $= \"\") %warrior = $LoginName;"
    "  TWBTabView.view(%warrior, \"Warrior\"); w_profile.setValue(1);"
    "  %tribeCount = getField(getRecord(%info, 1), 0);"
    "  for (%i = 0; %i < %tribeCount; %i++) {"
    "   %tribe = getField(getRecord(%info, %i + 2), 0);"
    "   if (trim(%tribe) !$= \"\") TWBTabView.addTab(%i + 1, %tribe, 1);"
    "  }"
    " } else if (PlayerPane.visible) PlayerPane.onWake();"
    " else TribePane.onWake();"
    " %selectedTab = TWBTabView.getSelectedID();"
    " if (TWBTabView.getTabText(%selectedTab) $= \"\") {"
    "  %warrior = $pref::LastLoginName;"
    "  if (trim(%warrior) $= \"\") %warrior = $LoginName;"
    "  if (trim(%warrior) !$= \"\") TWBTabView.setTabText(%selectedTab, %warrior);"
    " }"
    " %warrior = $pref::LastLoginName;"
    " if (trim(%warrior) $= \"\") %warrior = getField(WONGetAuthInfo(), 0);"
    " if (trim(%warrior) $= \"\") %warrior = $LoginName;"
    " if (!isObject(T2LaunchTribeCache)) {"
    "  new ScriptObject(T2LaunchTribeCache) { class = \"T2LaunchTribeCache\"; };"
    " }"
    " if (trim(%warrior) !$= \"\") {"
    "  T2LaunchTribeCache.tribeCount = 0;"
    "  T2LaunchTribeCache.key = LaunchGui.key++;"
    "  schedule(500, 0, DatabaseQueryArray, 13, 0, %warrior,"
    "   T2LaunchTribeCache, T2LaunchTribeCache.key, true);"
    " }"
    "}"
    "function T2LaunchTribeCache::onDatabaseQueryResult(%this, %status, %result, %key) {"
    " if (%this.key != %key) return;"
    " %this.loading = 0;"
    " if (getField(%status, 0) != 0) %this.failed = 1;"
    " Canvas.repaint();"
    "}"
    "function T2LaunchTribeCache::onDatabaseRow(%this, %row, %isLastRow, %key) {"
    " if (%this.key != %key) return;"
    " %name = getField(%row, 0);"
    " if (trim(%name) !$= \"\") {"
    "  %this.tribeName[%this.tribeCount] = %name;"
    "  %this.tribeTag[%this.tribeCount] = getField(%row, 1);"
    "  %this.tribeId[%this.tribeCount] = getField(%row, 2);"
    "  %this.tribeAccess[%this.tribeCount] = getField(%row, 3);"
    "  %this.tribeTitle[%this.tribeCount] = getField(%row, 5);"
    "  %this.tribeCount++; %exists = false;"
    "  for (%i = 0; %i < TWBTabView.tabCount(); %i++) {"
    "   if (stricmp(TWBTabView.getTabText(%i), %name) == 0) %exists = true;"
    "  }"
    "  if (!%exists) TWBTabView.addTab(TWBTabView.tabCount(), %name, 1);"
    " }"
    " if (%isLastRow) { %this.loading = 0; Canvas.repaint(); }"
    "}"
    "function GetTribeMember(%tribeName) {"
    " %info = WONGetAuthInfo(); %count = getField(getRecord(%info, 1), 0);"
    " for (%i = 0; %i < %count; %i++) {"
    "  if (stricmp(getField(getRecord(%info, %i + 2), 0), %tribeName) == 0) return 1;"
    " }"
    " if (isObject(T2LaunchTribeCache)) {"
    "  for (%i = 0; %i < T2LaunchTribeCache.tribeCount; %i++) {"
    "   if (stricmp(T2LaunchTribeCache.tribeName[%i], %tribeName) == 0) return 1;"
    "  }"
    " }"
    " return 0;"
    "}"
    "function GetProfileHdr(%type, %line) {"
    " %info = WONGetAuthInfo(); $GuidTribes = getRecords(%info, 1);"
    " if (%type == 0) {"
    "  TProfileHdr.tribeID = getField(%line, 0); TProfileHdr.tribeName = getField(%line, 1);"
    "  TProfileHdr.tribeTag = getField(%line, 2); TProfileHdr.appending = getField(%line, 3);"
    "  TProfileHdr.recruiting = getField(%line, 4); TProfileHdr.tribegfx = getField(%line, 5);"
    "  TProfileHdr.twa = 0; TProfileHdr.Desc = \"\";"
    "  TL_Profile.setVisible(1); TL_Roster.setVisible(1); TL_News.setVisible(1);"
    "  for (%i = 0; %i < getField(getRecord($GuidTribes, 0), 0); %i++) {"
    "   %authRow = getRecord($GuidTribes, %i + 1);"
    "   if (getField(%authRow, 3) == 1401) TProfileHdr.twa = 4;"
    "   else if (TProfileHdr.tribeID == getField(%authRow, 3) && TProfileHdr.twa == 0)"
    "    TProfileHdr.twa = getField(%authRow, 4);"
    "  }"
    "  if (isObject(T2LaunchTribeCache)) {"
    "   for (%i = 0; %i < T2LaunchTribeCache.tribeCount; %i++) {"
    "    if (stricmp(T2LaunchTribeCache.tribeName[%i], TProfileHdr.tribeName) == 0)"
    "     TProfileHdr.twa = T2LaunchTribeCache.tribeAccess[%i];"
    "   }"
    "  }"
    "  if (TProfileHdr.twa > 1) { TW_Admin.setVisible(1); TL_Invites.setVisible(1); }"
    "  else { TW_Admin.setVisible(0); TL_Invites.setVisible(0); }"
    " } else {"
    "  TProfileHdr.playerName = getField(%line, 0); TProfileHdr.playerTag = getField(%line, 1);"
    "  TProfileHdr.appending = getField(%line, 2); TProfileHdr.playerID = getField(%line, 3);"
    "  TProfileHdr.registered = getField(%line, 4); TProfileHdr.onLine = getField(%line, 5);"
    "  TProfileHdr.playerURL = getField(%line, 6); TProfileHdr.playerGFX = getField(%line, 7);"
    "  TProfileHdr.twa = 0; TProfileHdr.Desc = \"\";"
    "  if (getField(getRecord(%info, 0), 3) == getField(%line, 3)) TProfileHdr.twa = 1;"
    "  for (%i = 0; %i < getField(getRecord($GuidTribes, 0), 0); %i++) {"
    "   %authRow = getRecord($GuidTribes, %i + 1);"
    "   if (getField(%authRow, 3) == 1401 && getField(%authRow, 4) >= 2) TProfileHdr.twa = 1;"
    "  }"
    "  W_Profile.setVisible(1); W_History.setVisible(1); W_Tribes.setVisible(1);"
    "  %isMe = getField(getRecord(%info, 0), 0) $= TWBTitle.name; TProfileHdr.isMe = %isMe;"
    "  TProfileHdr.twa = TProfileHdr.isMe;"
    "  if (TProfileHdr.twa) {"
    "   W_BuddyList.setText(\"BUDDYLIST\"); W_BuddyList.setVisible(1);"
    "   W_BuddyList.command = \"PlayerPane.ButtonClick(3);\"; W_BuddyList.groupNum = 5; W_Admin.setVisible(1);"
    "  } else {"
    "   W_BuddyList.setText(\"OPTIONS\"); W_BuddyList.setVisible(1);"
    "   W_BuddyList.command = \"PlayerPane.ButtonClick(4);\"; W_BuddyList.groupNum = 4; W_Admin.setVisible(0);"
    "  }"
    " }"
    "}"
    "function CreateTribeDlg::onWake(%this) {"
    " %frame = %this.getObject(0);"
    " if (isObject(%frame)) {"
    "  if (%frame.getCount() > 3) {"
    "   %nameEdit = %frame.getObject(3);"
    "   if (isObject(%nameEdit)) %nameEdit.IRCName = false;"
    "  }"
    " }"
    " if (isObject(CT_TagText)) CT_TagText.IRCName = false;"
    " updateTribeTagPreview();"
    "}"
    "function CreateTribeDlg::onDatabaseQueryResult(%this, %status, %resultString, %key) {"
    " if (%this.key != %key) return;"
    " if (getField(%status, 0) == 0) {"
    "  if (%this.state $= \"createTribe\") {"
    "   if (isObject(T2LaunchTribeCache)) {"
    "    %name = $CreateTribeName; %found = false;"
    "    for (%i = 0; %i < T2LaunchTribeCache.tribeCount; %i++) {"
    "     if (stricmp(T2LaunchTribeCache.tribeName[%i], %name) == 0) %found = true;"
    "    }"
    "    if (!%found) {"
    "     %index = T2LaunchTribeCache.tribeCount;"
    "     T2LaunchTribeCache.tribeName[%index] = %name;"
    "     T2LaunchTribeCache.tribeTag[%index] = $CreateTribeTag;"
    "     T2LaunchTribeCache.tribeAccess[%index] = 4;"
    "     T2LaunchTribeCache.tribeTitle[%index] = \"Founder\";"
    "     T2LaunchTribeCache.tribeCount++;"
    "    }"
    "   }"
    "   TWBTabView.view($CreateTribeName, \"Tribe\"); Canvas.popDialog(CreateTribeDlg);"
    "   $CreateTribeName = \"\"; $CreateTribeTag = \"\";"
    "   if (isObject(CreateTribeDlg)) CreateTribeDlg.delete(); WonUpdateCertificate();"
    "  }"
    " } else if (getSubStr(getField(%status, 1), 0, 9) $= \"ORA-04061\") {"
    "  %this.state = \"error\";"
    "  MessageBoxOK(\"ERROR\", \"There was an error processing your request, please wait a few moments and try again.\");"
    " } else { %this.state = \"error\"; MessageBoxOK(\"WARNING\", getField(%status, 1)); }"
    " Canvas.setCursor(DefaultCursor);"
    "}";

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

static int redirect_won_bootstrap(void) {
    static const uintptr_t addresses[] = {0x084846B1, 0x084846CD, 0x084846E9};
    static const char *original[] = {
        "tribes2.m1.sierra.com:15104", "tribes2.m2.sierra.com:15104",
        "tribes2.m3.sierra.com:15104"
    };
    const char *port_text = getenv("T2LAUNCH_WON_PORT");
    if (port_text && *port_text) {
        char *end;
        unsigned long port = strtoul(port_text, &end, 10);
        if (*end || port == 0 || port > 65535) return 0;
        snprintf(won_endpoint, sizeof(won_endpoint), "127.0.0.1:%lu", port);
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
        memcpy((void *)addresses[i], won_endpoint, strlen(won_endpoint) + 1);
    }
    if (mprotect((void *)page, (size_t)page_size, protection) != 0) {
        fprintf(stderr, "[t2_hook] WON redirect: restoring page permissions failed: %s\n", strerror(errno));
        return 0;
    }
    fprintf(stderr, "[t2_hook] WON bootstrap: redirected 3 endpoints to %s (memory only)\n", won_endpoint);
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

/* SDL_PollEvent is interposed as a diagnostic that the hook is first in the
 * executable's dynamic lookup order. */
int SDL_PollEvent(void *event) {
    if (!real_sdl_poll_event)
        real_sdl_poll_event = (sdl_poll_event_t)dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!real_sdl_poll_event) return 0;
    if (!sdl_poll_hook_reported) {
        sdl_poll_hook_reported = 1;
        fprintf(stderr, "[t2_hook] SDL_PollEvent main-loop hook active\n");
    }

    return real_sdl_poll_event(event);
}

/* Run deferred TorqueScript changes after the game's rendered frame, on the
 * SDL/OpenGL thread. The event-poll hook may only run during startup on some
 * builds, whereas SDL_GL_SwapBuffers is called continuously by the main loop. */
void SDL_GL_SwapBuffers(void) {
    if (!real_sdl_gl_swap_buffers)
        real_sdl_gl_swap_buffers = (sdl_gl_swap_buffers_t)dlsym(
            RTLD_NEXT, "SDL_GL_SwapBuffers");
    if (!real_sdl_gl_swap_buffers) return;

    real_sdl_gl_swap_buffers();
    if (!sdl_gl_swap_hook_reported) {
        sdl_gl_swap_hook_reported = 1;
        game_main_thread = pthread_self();
        game_main_thread_known = 1;
        fprintf(stderr, "[t2_hook] SDL_GL_SwapBuffers main-loop hook active\n");
    }
    if (!registered || !Con__evaluate ||
        !game_main_thread_known ||
        !pthread_equal(pthread_self(), game_main_thread))
        return;

    time_t now = time(NULL);
    if (now != (time_t)-1 && hook_started_at && now - hook_started_at >= 10 &&
        (!last_player_pane_override || now - last_player_pane_override >= 15)) {
        Con__evaluate(player_pane_button_click_override, 0, NULL, 0);
        if (!last_player_pane_override)
            fprintf(stderr, "[t2_hook] Installed runtime PlayerPane TRIBES handler\n");
        last_player_pane_override = now;
    }
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

static const char* hook_sha1_cb(void *obj, S32 argc, const char **argv) {
    static char hex[41];
    (void)obj; (void)argc;
    if (argc < 2) return "usage: hook_sha1 <string>";
    sha1((const uint8_t *)argv[1], strlen(argv[1]), hex);
    return hex;
}

static const char* hook_rsa_mod_exp_cb(void *obj, S32 argc, const char **argv) {
    static char result[2048];
    (void)obj;
    if (argc < 4) return "usage";
    fprintf(stderr, "[t2_hook] rsa_mod_exp b=%.20s... e=%.20s... m=%.20s...\n",
        argv[1], argv[2], argv[3]);
    /* Use precompiled GMP helper for fast modexp via fork+exec */
    int pipefd[2];
    if (pipe(pipefd) != 0) { strcpy(result, "ERR:pipe"); return result; }
    pid_t cpid = fork();
    if (cpid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        char *args[] = {"/home/loki/game/rsa_helper", (char*)argv[1], (char*)argv[2], (char*)argv[3], NULL};
        execv("/home/loki/game/rsa_helper", args);
        _exit(1);
    } else if (cpid > 0) {
        close(pipefd[1]);
        ssize_t n = read(pipefd[0], result, sizeof(result)-1);
        close(pipefd[0]);
        waitpid(cpid, NULL, 0);
        if (n <= 0) { strcpy(result, "ERR:no_output"); return result; }
        result[n] = 0;
    } else {
        strcpy(result, "ERR:fork");
        close(pipefd[0]); close(pipefd[1]);
        return result;
    }
    /* strip newline */
    result[strcspn(result, "\r\n")] = 0;
    /* lowercase */
    for (int i = 0; result[i]; i++)
        if (result[i] >= 'A' && result[i] <= 'F')
            result[i] += 32;
    fprintf(stderr, "[t2_hook] rsa_mod_exp result=%.40s...\n", result);
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
    sprintf(buf, "%016llx", (unsigned long long)val);
    return buf;
}

/* ── RC4 implementation in C (correct, no TS──────── */

static void rc4_ksa(uint8_t *S, const uint8_t *key, int keylen) {
    for (int i = 0; i < 256; i++) S[i] = i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % keylen]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }
}

static void rc4_drop(uint8_t *S, int n) {
    int i = 0, j = 0;
    for (int k = 0; k < n; k++) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }
}

static void rc4_crypt(uint8_t *S, const uint8_t *in, uint8_t *out, int len) {
    int i = 0, j = 0;
    for (int k = 0; k < len; k++) {
        i = (i + 1) & 0xFF;
        j = (j + S[i]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
        out[k] = in[k] ^ S[(S[i] + S[j]) & 0xFF];
    }
}

static int hex2nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int max_out) {
    int len = 0;
    for (int i = 0; hex[i] && hex[i+1] && len < max_out; i += 2) {
        int hi = hex2nibble(hex[i]);
        int lo = hex2nibble(hex[i+1]);
        if (hi < 0 || lo < 0) return -1;
        out[len++] = (hi << 4) | lo;
    }
    return len;
}

static void bytes_to_hex(const uint8_t *data, int len, char *out) {
    for (int i = 0; i < len; i++)
        sprintf(out + i*2, "%02x", data[i]);
    out[len*2] = '\0';
}

static const char* hook_login_cb(void *obj, S32 argc, const char **argv) {
    static char result[2048];
    (void)obj; (void)argc;

    const char *username = "LouCypher";
    if (argc >= 2) username = argv[1];

    /* ── Read password from file ── */
    char password[256] = {0};
    FILE *fp = fopen("/home/loki/.loki/tribes2/password", "r");
    if (!fp) { snprintf(result, sizeof(result), "ERR:no_pw_file"); return result; }
    if (!fgets(password, sizeof(password), fp)) { fclose(fp); snprintf(result, sizeof(result), "ERR:empty_pw"); return result; }
    fclose(fp);
    password[strcspn(password, "\r\n")] = 0;
    fprintf(stderr, "[t2_hook] login: pw=%s (len=%zu)\n", password, strlen(password));

    /* ── Read private.store ── */
    char store_path[512];
    snprintf(store_path, sizeof(store_path), "/home/loki/game/base/private.store");
    FILE *fs = fopen(store_path, "r");
    if (!fs) { snprintf(result, sizeof(result), "ERR:no_store"); return result; }
    char line[2048], enc_data[2048] = {0};
    while (fgets(line, sizeof(line), fs)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (strcmp(line, username) == 0) {
            strncpy(enc_data, tab + 1, sizeof(enc_data) - 1);
            break;
        }
    }
    fclose(fs);
    if (!enc_data[0]) { snprintf(result, sizeof(result), "ERR:no_user"); return result; }

    /* ── Parse nonce:ciphertext ── */
    char *colon = strchr(enc_data, ':');
    if (!colon) { snprintf(result, sizeof(result), "ERR:bad_fmt"); return result; }
    *colon = 0;
    char *nonce_hex = enc_data;
    char *ct_hex = colon + 1;

    /* ── Compute RC4 key = SHA1(password + nonce) ── */
    char pw_nonce[512];
    snprintf(pw_nonce, sizeof(pw_nonce), "%s%s", password, nonce_hex);
    char key_hex[41];
    sha1((const uint8_t *)pw_nonce, strlen(pw_nonce), key_hex);

    uint8_t key_bytes[20];
    for (int i = 0; i < 20; i++) {
        unsigned int b;
        sscanf(key_hex + i*2, "%02x", &b);
        key_bytes[i] = (uint8_t)b;
    }

    /* ── Decode ciphertext hex ── */
    int ct_len = strlen(ct_hex) / 2;
    uint8_t ct[1024], pt[1024];
    if (ct_len > sizeof(ct)) { snprintf(result, sizeof(result), "ERR:ct_too_long"); return result; }
    for (int i = 0; i < ct_len; i++) {
        unsigned int b;
        sscanf(ct_hex + i*2, "%02x", &b);
        ct[i] = (uint8_t)b;
    }

    /* ── RC4 KSA ── */
    uint8_t S[256];
    for (int i = 0; i < 256; i++) S[i] = i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key_bytes[i % 20]) & 0xFF;
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }

    /* ── RC4 drop 2048 ── */
    int ii = 0, jj = 0;
    for (int k = 0; k < 2048; k++) {
        ii = (ii + 1) & 0xFF;
        jj = (jj + S[ii]) & 0xFF;
        uint8_t tmp = S[ii]; S[ii] = S[jj]; S[jj] = tmp;
    }

    /* ── RC4 PRGA + decrypt ── */
    for (int k = 0; k < ct_len; k++) {
        ii = (ii + 1) & 0xFF;
        jj = (jj + S[ii]) & 0xFF;
        uint8_t tmp = S[ii]; S[ii] = S[jj]; S[jj] = tmp;
        pt[k] = ct[k] ^ S[(S[ii] + S[jj]) & 0xFF];
    }

    /* ── Convert plaintext bytes to hex string ── */
    for (int i = 0; i < ct_len; i++)
        sprintf(result + i*2, "%02x", pt[i]);
    result[ct_len*2] = 0;
    fprintf(stderr, "[t2_hook] login: decrypted %d bytes = %.40s...\n", ct_len, result);
    return result;
}

/* ── Registration thread ─────────────────────────────────────────────── */

static void* poll_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "[t2_hook] Poll thread started\n");

    for (int i = 0; i < 100; i++) {
        usleep(500000);  /* 500ms */

        uint32_t console_ptr = *(volatile uint32_t *)CONSOLE_GLOBAL;
        if (console_ptr != 0) {
            uint32_t table_size = *(volatile uint32_t *)(console_ptr + CONSOLE_TABLE_SIZE_OFFS);
            if (table_size > 0 && !registered) {
                registered = 1;
                Con__addMethodS = (con_addMethodS_t)0x80E85C0;
                fprintf(stderr, "[t2_hook] Console ready (table_size=%d) at ~%ds\n", table_size, i/2);
                Con__addMethodS(NULL, "hook_test", hook_test_cb, "hook_test - test command", 1, 1);
                Con__addMethodS(NULL, "hook_sha1", hook_sha1_cb, "hook_sha1 <string> - SHA1 hash", 2, 2);
                Con__addMethodS(NULL, "hook_rsa_mod_exp", hook_rsa_mod_exp_cb, "hook_rsa_mod_exp <base> <exp> <mod>", 4, 4);
                Con__addMethodS(NULL, "hook_rand64", hook_rand64_cb, "hook_rand64 - random 64-bit hex", 1, 1);
                Con__addMethodS(NULL, "hook_login", hook_login_cb, "hook_login [username] - C-based RC4 decrypt", 1, 2);
                fprintf(stderr, "[t2_hook] Registered 5 commands\n");
                break;
            }
        }
    }
    if (!registered) {
        fprintf(stderr, "[t2_hook] TIMEOUT\n");
    }
    return NULL;
}

#ifndef T2_HOOK_TEST
static void __attribute__((constructor)) init(void) {
    if (!is_game_process()) return;
    game_main_thread = pthread_self();
    hook_started_at = time(NULL);
    const char *won = getenv("T2LAUNCH_WONEMU");
    if (!is_supported_game()) {
        fprintf(stderr, "[t2_hook] Unsupported client: console address check failed\n");
        if (won && strcmp(won, "1") == 0) _exit(1);
        return;
    }
    if (won && strcmp(won, "1") == 0) {
        if (!install_won_verifier()) _exit(1);
        won_redirected = redirect_won_bootstrap();
        if (!won_redirected) _exit(1);
    }
    const char *disable_commands = getenv("T2LAUNCH_DISABLE_HOOK_COMMANDS");
    if (disable_commands && strcmp(disable_commands, "1") == 0) {
        fprintf(stderr, "[t2_hook] Console helper registration disabled by environment\n");
        return;
    }
    fprintf(stderr, "[t2_hook] Constructor: starting poll thread\n");
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    pthread_create(&tid, &attr, poll_thread, NULL);
    pthread_attr_destroy(&attr);
}
#endif
