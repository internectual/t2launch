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

typedef int S32;

typedef void (*con_addMethodS_t)(const char *nsName, const char *name,
    const char *(*cb)(void *obj, S32 argc, const char **argv),
    const char *usage, S32 minArgs, S32 maxArgs);
typedef const char *(*con_evaluate_t)(const char *code, unsigned char echo,
    const char *fileName, unsigned char noCalls);
typedef void (*sdl_gl_swap_buffers_t)(void);

static con_addMethodS_t Con__addMethodS = NULL;
static con_evaluate_t Con__evaluate = (con_evaluate_t)0x080E88F4;
static sdl_gl_swap_buffers_t real_sdl_gl_swap_buffers = NULL;
static pthread_t game_main_thread;
static volatile int registered = 0;
static volatile int game_main_thread_known = 0;
static volatile int profile_browser_wake_seen = 0;
static volatile int tribesnext_runtime_installed = 0;
static volatile int player_pane_override_installed = 0;
static char tribesnext_certificate[20001];
static char tribesnext_modulus[2049];
static char tribesnext_exponent[2049];
static char tribesnext_private_exponent[4097];
static char tribesnext_username[128];
static char tribesnext_password[512];
static int won_redirected = 0;
static char won_endpoint[28] = "127.0.0.1:15104";
static time_t hook_started_at = 0;
static time_t last_player_pane_override = 0;
static int sdl_gl_swap_hook_reported = 0;

static const char tribesnext_runtime_script[] =
    "function t2launch_tnUnlockPoll() {"
    " if ($T2Launch::TNReady && trim($LoginName) !$= \"\" && stricmp($T2Launch::TNUsername, $LoginName) != 0) {"
    "  hook_tn_clear(); $T2Launch::TNReady = 0; $T2Launch::TNLastAttempt = \"\";"
    " }"
    " if (!$T2Launch::TNReady && trim($LoginName) !$= \"\" && $LoginPassword !$= \"\") {"
    "  %fingerprint = hook_sha1($LoginName @ \"\\t\" @ $LoginPassword);"
    "  if (%fingerprint !$= $T2Launch::TNLastAttempt) {"
    "   $T2Launch::TNLastAttempt = %fingerprint;"
    "   schedule(350, 0, \"t2launch_tnTryUnlock\", %fingerprint);"
    "  }"
    " }"
    " schedule(100, 0, \"t2launch_tnUnlockPoll\");"
    "}"
    "function t2launch_tnTryUnlock(%fingerprint) {"
    " if ($T2Launch::TNReady || %fingerprint !$= hook_sha1($LoginName @ \"\\t\" @ $LoginPassword)) return;"
    " if (hook_tn_unlock($LoginName, $LoginPassword) $= \"ok\") {"
    "  $T2Launch::TNReady = 1; $T2Launch::TNUsername = $LoginName;"
    " }"
    "}"
    "function t2launch_tnReject(%message) {"
    " error(%message); schedule(1000, 0, \"disconnect\");"
    "}"
    "function clientCmdt2csri_pokeClient(%version) {"
    " hook_tn_trace(\"poke\");"
    " if (!$T2Launch::TNReady) { t2launch_tnReject(\"T2CSRI: use your TribesNext account password in the login screen to unlock the stored key\"); return; }"
    " $T2Launch::TNEncryptedChallenge = \"\";"
    " %certLength = hook_tn_certificate_length();"
    " if (%certLength <= 0 || %certLength > 20000) { t2launch_tnReject(\"T2CSRI: no account certificate is loaded\"); return; }"
    " echo(\"T2CSRI: sending existing account certificate\");"
    " for (%i = 0; %i < %certLength; %i += 200) {"
    "  %certChunk = hook_tn_certificate_chunk(%i);"
    "  if (strLen(%certChunk) == 0) { t2launch_tnReject(\"T2CSRI: certificate chunk was empty\"); return; }"
    "  commandToServer('t2csri_sendCertChunk', %certChunk);"
    " } hook_tn_trace(\"certificate_sent\");"
    " %address = $T2Launch::TNJoinAddress;"
    " if (trim(%address) $= \"\") %address = ServerConnection.getAddress();"
    " %firstColon = strstr(%address, \":\");"
    " if (%firstColon < 0) { t2launch_tnReject(\"T2CSRI: invalid game-server address\"); return; }"
    " %afterFirst = getSubStr(%address, %firstColon + 1, strLen(%address));"
    " %secondColon = strstr(%afterFirst, \":\");"
    " if (%secondColon >= 0) %serverIP = getSubStr(%afterFirst, 0, %secondColon);"
    " else %serverIP = getSubStr(%address, 0, %firstColon);"
    " echo(\"T2CSRI: challenge target IP \" @ %serverIP);"
    " %serverIPHex = hook_tn_ipv4_hex(%serverIP);"
    " if (strLen(%serverIPHex) != 8) { t2launch_tnReject(\"T2CSRI: could not encode game-server IPv4 address\"); return; }"
    " hook_tn_ip_trace(%serverIP, %serverIPHex);"
    " $T2Launch::TNClientChallenge = hook_rand64() @ %serverIPHex;"
    " schedule(1000, 0, \"t2launch_tnSendChallenge\");"
    "}"
    "function t2launch_tnSendChallenge() {"
    " if ($T2Launch::TNClientChallenge !$= \"\") {"
    "  commandToServer('t2csri_sendChallenge', $T2Launch::TNClientChallenge);"
    "  hook_tn_trace(\"client_challenge_sent\");"
    "  echo(\"T2CSRI: client challenge sent\");"
    " }"
    "}"
    "function clientCmdt2csri_getChallengeChunk(%chunk) {"
    " if (strLen($T2Launch::TNEncryptedChallenge) + strLen(%chunk) > 8192) { t2launch_tnReject(\"T2CSRI: server challenge exceeded the size limit\"); return; }"
    " if ($T2Launch::TNEncryptedChallenge $= \"\") echo(\"T2CSRI: received encrypted server challenge\");"
    " hook_tn_trace(\"challenge_chunk\");"
    " $T2Launch::TNEncryptedChallenge = $T2Launch::TNEncryptedChallenge @ strLwr(%chunk);"
    "}"
    "function clientCmdt2csri_decryptChallenge() {"
    " hook_tn_trace(\"decrypt_challenge\");"
    " %decrypted = hook_tn_decrypt($T2Launch::TNEncryptedChallenge);"
    " %clientLen = strLen($T2Launch::TNClientChallenge);"
    " if (strLwr(getSubStr(%decrypted, 0, %clientLen)) !$= strLwr($T2Launch::TNClientChallenge)) { t2launch_tnReject(\"T2CSRI: server challenge did not echo client nonce\"); return; }"
    " %serverChallenge = getSubStr(%decrypted, %clientLen, strLen(%decrypted));"
    " if (trim(%serverChallenge) $= \"\") { t2launch_tnReject(\"T2CSRI: server challenge was empty\"); return; }"
    " schedule(getRandom(128, 512), 0, \"t2launch_tnRespond\", %serverChallenge);"
    " $T2Launch::TNEncryptedChallenge = \"\";"
    "}"
    "function t2launch_tnRespond(%challenge) {"
    " commandToServer('t2csri_challengeResponse', %challenge);"
    " hook_tn_trace(\"response_sent\");"
    "}"
    "t2launch_tnUnlockPoll();";

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
    " hook_profile_browser_wake();"
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
    "  %localName = $pref::LastLoginName;"
    "  if (trim(%localName) $= \"\") %localName = getField(getRecord(%info, 0), 0);"
    "  %isMe = (stricmp(TProfileHdr.playerName, %localName) == 0) ||"
    "   (stricmp(getField(getRecord(%info, 0), 0), TWBTitle.name) == 0);"
    "  %authID = getField(getRecord(%info, 0), 3);"
    "  if (trim(%authID) !$= \"\" && %authID == TProfileHdr.playerID) %isMe = 1;"
    "  TProfileHdr.isMe = %isMe;"
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
    "}"
    "package T2LaunchJoinTrace {"
    " function JoinGame(%address) {"
    "  $T2Launch::TNJoinAddress = %address;"
    "  hook_tn_join_trace(%address, $T2Launch::TNReady);"
    "  Parent::JoinGame(%address);"
    " }"
    "}; activatePackage(T2LaunchJoinTrace);";

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

/* Run deferred TorqueScript changes after a rendered frame on the game thread. */
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

    if (!tribesnext_runtime_installed) {
        Con__evaluate(tribesnext_runtime_script, 0, NULL, 0);
        tribesnext_runtime_installed = 1;
        fprintf(stderr, "[t2_hook] Installed runtime TribesNext authentication handlers\n");
    }

    time_t now = time(NULL);
    if (now != (time_t)-1 && hook_started_at && now - hook_started_at >= 2 &&
        !profile_browser_wake_seen && !player_pane_override_installed &&
        (!last_player_pane_override || now - last_player_pane_override >= 1)) {
        last_player_pane_override = now;
        const char *gui_ready = Con__evaluate("isObject(TWBTabView);", 0, NULL, 0);
        if (gui_ready && atoi(gui_ready) > 0) {
            Con__evaluate(player_pane_button_click_override, 0, NULL, 0);
            player_pane_override_installed = 1;
            fprintf(stderr, "[t2_hook] Installed runtime PlayerPane TRIBES handler\n");
        }
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
    if (access("/home/loki/game/rsa_helper", X_OK) != 0) return 0;
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
        char *args[] = {"/home/loki/game/rsa_helper", "--stdin", NULL};
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

    file = fopen("/home/loki/game/base/public.store", "r");
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

    file = fopen("/home/loki/game/base/private.store", "r");
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
    FILE *file;
    if (!tribesnext_username[0] || !tribesnext_modulus[0] || !tribesnext_exponent[0])
        return 0;
    file = fopen("/home/loki/game/base/public.store", "r");
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
                    "hook_tn_trace <known_stage> - report T2CSRI join stage", 2, 2);
                Con__addMethodS(NULL, "hook_tn_join_trace", hook_tn_join_trace_cb,
                    "hook_tn_join_trace <address> <ready> - safe join diagnostics", 3, 3);
                Con__addMethodS(NULL, "hook_tn_ip_trace", hook_tn_ip_trace_cb,
                    "hook_tn_ip_trace <ip> <hex> - safe challenge address diagnostics", 3, 3);
                Con__addMethodS(NULL, "hook_tn_ipv4_hex", hook_tn_ipv4_hex_cb,
                    "hook_tn_ipv4_hex <dotted_ipv4> - encode IPv4 in T2CSRI hex order", 2, 2);
                fprintf(stderr, "[t2_hook] Registered 14 commands\n");
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
