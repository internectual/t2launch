# t2launch

Play Tribes 2 online on Linux with the native Loki client, without Wine.

Tribes 2 originally logged in through Sierra's WON services, which shut down
long ago. The TribesNext community patch replaced them with its own RSA
challenge-response login, but shipped only as a Windows executable patcher, so
Linux players ran the whole game under Wine. T2Launch runs the Loki Linux client
(release #25034, 2001) on a current 32-bit userland and adds what it needs at
runtime: an `LD_PRELOAD` hook that implements the TribesNext client handshake in
C, and a local WON emulator for login, the server list, and community features.
No game file, script, or archive is modified on disk.

## Features

- TribesNext login and joining TribesNext servers, using your existing
  `public.store` / `private.store` account files
- Account creation and Edit Account from the game's own dialogs, for
  TribesNext or the emulator's own accounts, with a CD key generator
- TribesNext accounts follow you: logging in on a new machine fetches the
  account from TribesNext
- A local WON emulator: login, server list (TribesNext plus LAN/other Torque
  masters), buddies, T-Mail, tribes, player profiles, news, and chat
- Optional remote mode, so several machines can share one emulator
- A system-tray desktop launcher with per-profile options, for the game or
  the dedicated server (`tribes2d.dynamic`)
- A dedicated server that TribesNext players can join, verified with
  TribesNext's T2CSRI protocol, with an optional restart after a crash
- Fixes for running a 2001 binary on current libraries: stack alignment, thread
  joins, GUI redraw, volume sliders, and monitor/mode selection

## Install

You need your own Tribes 2 Linux game files (Loki release #25034) and, for
online play, a TribesNext account. The launcher looks for the game in
`~/t2-linux/` by default:

```
~/t2-linux/
├── tribes2.dynamic            # Loki Linux binary (#25034)
├── base/                      # Game data, scripts, fonts, and account stores
└── *.vl2                      # Script archives
```

### Packages

The game is 32-bit, so the host needs multilib support. On Arch, enable the
`[multilib]` repository in `/etc/pacman.conf` if it is not already (CachyOS
enables it by default):

```ini
[multilib]
Include = /etc/pacman.d/mirrorlist
```

Then install:

```bash
sudo pacman -Syu --needed \
    lib32-glibc lib32-gcc-libs lib32-mesa lib32-glu lib32-libx11 lib32-libxext \
    lib32-libxrandr lib32-freetype2 lib32-libpulse lib32-gmp \
    gcc python xdotool xorg-xrandr
```

| Package | Used for |
|---------|----------|
| `lib32-glibc`, `lib32-gcc-libs` | 32-bit loader and C runtime; `gcc -m32` builds |
| `lib32-mesa`, `lib32-glu` | OpenGL |
| `lib32-libx11`, `lib32-libxext`, `lib32-libxrandr` | X11 windowing (the bundled SDL links XRandR) |
| `lib32-freetype2` | TrueType text rendering |
| `lib32-libpulse` | `padsp`, which gives Loki OpenAL its OSS `/dev/dsp` |
| `lib32-gmp` | the RSA helper used for TribesNext authentication |
| `gcc` | compiling the hook, RSA helper, and stack-realignment shim |
| `python` | the WON emulator and the desktop launcher |
| `xdotool`, `xorg-xrandr` | keeping the window on the selected monitor and mode |

`strace` is optional (for `T2LAUNCH_TRACE_CONNECTS=1`).

Debian/Ubuntu and Fedora are untested but supported. The game starts through
`/lib/ld-linux.so.2`, which each distribution's 32-bit C library provides, and
the launcher finds the 32-bit `libpulsedsp.so` under `/usr/lib32/pulseaudio`
(Arch), `/usr/lib/i386-linux-gnu/pulseaudio` (Debian/Ubuntu), or
`/usr/lib/pulseaudio` (Fedora), skipping 64-bit copies. The rough equivalents
of the Arch list are:

```bash
# Debian/Ubuntu
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install libc6:i386 libgl1:i386 libglu1-mesa:i386 libx11-6:i386 libxext6:i386 \
    libxrandr2:i386 libfreetype6:i386 pulseaudio-utils:i386 libgmp-dev:i386 \
    gcc-multilib python3-venv xdotool x11-xserver-utils

# Fedora
sudo dnf install glibc-devel.i686 libgcc.i686 mesa-libGL.i686 mesa-libGLU.i686 \
    libX11.i686 libXext.i686 libXrandr.i686 freetype.i686 pulseaudio-utils.i686 \
    gmp-devel.i686 gcc python3 xdotool xrandr
```

On Debian/Ubuntu, `pulseaudio-utils:i386` may replace the 64-bit
`pulseaudio-utils`; without it the game runs silently.

### Desktop launcher

```bash
./install.sh
~/.local/bin/t2launch
```

`install.sh` installs the launcher into a private Python environment with
PySide6. It checks for each package above (32-bit libraries through the
`ldconfig` cache, the compiler with a trial `gcc -m32` build) and prints one
install command for whatever is missing, for the detected distribution; it
still installs the launcher. The launcher also names any missing 32-bit library
before it starts the game.

## Usage

### Desktop launcher

T2Launch runs in the system tray. The tray menu lists profiles alphabetically;
click one to launch it, and hover the icon to see which are running. Open
**Settings** to choose the game directory, create and edit profiles, choose the
fullscreen display, toggle the hook, configure autolaunch, or **Rebuild** the
support binaries.

Settings are stored in `~/.config/t2launch/settings.json` with private
permissions. Launcher and crash-stage diagnostics are written, with login
arguments redacted, to `~/.local/state/t2launch/launch-diagnostics.log`.

### Profile options

Each profile's editor has three tabs. **Game** holds the command-line flags
(the profile supplies `-online` or `-nologin`). **WON emulator** and **Hook**
are generated from `t2options.py`, which declares every option once: its
default, its type, what it depends on, and how it is applied. An option whose
requirements are unmet is greyed out with a "Requires: …" note (for example,
the TribesNext handlers need WON emulation and the hook's console commands);
its saved choice comes back when the dependency does. **Show advanced options**
reveals the debugging and compatibility settings.

At launch, options become environment variables for `runtime/run-game`, and the
hook's options are written to `~/.local/state/t2launch/hook/<profile>.conf`
(one `key=value` per line, mode 0600), passed as `T2LAUNCH_HOOK_CONFIG`. The
hook reads a value from that file, then from the option's legacy environment
variable (such as `T2LAUNCH_FULL_REDRAW`), then its default, so command-line
launches keep working. Unknown keys are kept, and in-game scripts can read any
option with `hook_option("key")`.

### Dedicated server

A profile's **Game executable** (top of the Game tab) picks the game client
(`tribes2.dynamic`) or the dedicated server (`tribes2d.dynamic`); **Add server
profile** makes one for Katabatic CTF with local WON emulation. A server
profile hides the client-only options (video, gamescope, audio, the server
browser, Discord, the depot, the TribesNext login) and adds a **Dedicated
server** section:

- **Server console**: `terminal` opens the server in a terminal window
  (`$TERMINAL`, else x-terminal-emulator, konsole, gnome-terminal,
  xfce4-terminal, alacritty, kitty, foot or xterm) so you can type console
  commands; it stays open after the server stops. `headless` runs it in the
  background, with its output in the launcher log.
- **Restart the server if it crashes**: start it again 5 seconds after a crash
  (a nonzero exit or a signal). A clean `quit();`, Ctrl+C, or a fifth crash
  within two minutes stops it.
- **Let TribesNext players join** (on by default): see below.
- **Public IPv4 address** (advanced): the Internet address players connect to.

`-dedicated` is added when the profile doesn't have it. The server uses the
same `~/.loki/tribes2` as the game: it reads `prefs/ServerPrefs.cs`, and on exit
writes back `prefs/ClientPrefs.cs` with the values it loaded, so whichever of
a running game and server exits last writes that file.

With WON emulation on, the server logs in as `Tribes2Server` (the fixed
identity every dedicated server sends) and wonemu gives it a server
certificate. Players with a WON certificate from wonemu authenticate through
WON as before. TribesNext players have none, so the hook lets them connect and
runs the server half of TribesNext's T2CSRI 1.1 protocol in memory:

1. It pokes the client and receives its account certificate in chunks.
2. It checks the certificate's signature against the TribesNext root key in
   native code.
3. It encrypts the client's challenge plus its own to the account key.
4. Once the client returns the server half, it continues the stock connect
   with the account's name and GUID as the player's AuthInfo, so admin lists,
   bans and smurf checks work by GUID.

A player who doesn't finish within 15 seconds is dropped with TribesNext's own
message. TribesNext clients name the address they connected to, and the server
refuses a login naming any address other than its public one, its bind address
or the player's own LAN, as TribesNext servers do. The public address is
**Public IPv4 address**, or, when that's empty, the one run-game looks up from
TribesNext (`www.tribesnext.com/whatismyip.php`) at launch. If the lookup fails,
only LAN players and players using the bind address can join until you set it.
Clan tags from TribesNext community certificates are not shown.

wonemu's own accounts get GUIDs from 1,000,000,000 up, a range TribesNext
account GUIDs (in the low millions) never reach, so a wonemu account can't
share a TribesNext player's GUID, and admin and ban lists can't mix them up.
Each account keeps its GUID in the community database, and a new account whose
first choice is taken gets the next free one. A wonemu login always uses its own
account's GUID, even when this machine also holds a TribesNext account with the
same name. Accounts made before this change are moved into the range the first
time wonemu starts, along with their tribes, buddies, mail, forum posts and
news; GUIDs written into a server's `$Host::AdminList` or ban list for those
accounts must be added again.

A headless server writes its log line by line, so the output in the launcher
log is current and survives a crash.

#### Test results

Tested on 2026-10-25 with the Loki #25034 dedicated server on Katabatic CTF and
local WON emulation:

| Test | Result |
|------|--------|
| Hook in `tribes2d.dynamic` | Pass: WON verifier and redirect, monotonic clock, 64/s and 1000-byte packets, server patches, console commands, T2CSRI handlers |
| Server login to wonemu (`Tribes2Server`) | Pass: the server gets a certificate, loads the mission and answers browser queries |
| Join with a WON certificate (Linux client, native authentication off) | Pass: accepted through WON as `LouCypher`, GUID 2004299, not a smurf, no T2CSRI exchange |
| Join as a TribesNext player (Linux client with **TribesNext native authentication** on, which sends no WON certificate) | Pass: the full T2CSRI exchange (certificate, challenge, response), then in game |
| T2CSRI checks from the server console | Pass: a real certificate from `public.store` verifies; tampered and forged certificates and a wrong challenge response are refused |
| Restart after a crash (`SIGSEGV`) | Pass: restarted under a new process after 5 s; `SIGTERM` stopped the loop and the server |

Not yet tested: a Windows TribesNext or QoL client, players connecting from
outside the LAN, and the terminal console opened from the launcher.

Two server-console quirks turned up while testing:

- The Loki server console keeps the last 32 lines in
  `~/.loki/tribes2/history.txt`. When its input is not a terminal (a pipe or a
  FIFO), it runs each new line together with the last saved entry, so a saved
  `quit();` stops the next server on its first input.
- A local variable (`%name`) used outside a function crashes the server; wrap
  console checks in a function.

### Command line

```bash
./runtime/run-game tribes2 ~/t2-linux -online
T2LAUNCH_EXECUTABLE=dedicated T2LAUNCH_WON_MODE=local ./runtime/run-game tribes2 ~/t2-linux -mission Katabatic CTF
```

The first launch compiles `t2_hook.so`, `rsa_helper`, and `t2_realign.so` into
`games/tribes2/` with `gcc -m32` (`./runtime/build`), and rebuilds them when
their sources change. The launcher checks that every library the game links
resolves before starting it. The game runs on the host's 32-bit libraries plus
the bundled Loki-era libraries in `runtime/compat-libs/`.

| Variable | Effect |
|----------|--------|
| `T2LAUNCH_WON_MODE` | `off` (default), `local`, or `remote`; see [WON emulator](#won-emulator) |
| `T2LAUNCH_EXECUTABLE` | `client` (default) or `dedicated`; see [Dedicated server](#dedicated-server) |
| `T2LAUNCH_SERVER_RESTART=1` | Restart the dedicated server after a crash |
| `T2LAUNCH_SOFTWARE_GL=1` | Use software GL instead of the hardware driver |
| `T2LAUNCH_FULL_REDRAW=0` | Turn off the full-canvas redraw (see [Runtime compatibility](#runtime-compatibility)) |
| `T2LAUNCH_OPENAL_VOLUME_FIX=0` | Turn off the volume-slider fix |
| `T2LAUNCH_PULSE_SINK` | Send game audio to this sink (from `pactl list short sinks`) instead of the default output |
| `FULLSCREEN=true` | Start fullscreen (same as `-f`/`--fullscreen`) |
| `T2LAUNCH_TRACE_CONNECTS=1` | Record resolver names and, with `strace`, `connect()` calls under `~/.local/state/t2launch/diagnostics` |
| `T2LAUNCH_REALIGN_TRACE_AL=1` | Log each OpenAL gain call |

### Display and audio

Tribes 2 runs windowed at its selected resolution with hardware OpenGL unless
fullscreen is requested. An X11 watcher (`runtime/window_resize.sh`) keeps the
window on the selected monitor and scales fullscreen output while preserving
the game's aspect ratio; in windowed mode it follows the saved game resolution
when SDL 1.2 leaves the window at its previous size.

SDL 1.2's windowed `SDL_ListModes` may report "any size", and Tribes 2 then
falls back to a short built-in 4:3 list. The launcher enables SDL XRandR
enumeration, and the hook substitutes the host display's real mode list for
that case only.

The bundled Loki OpenAL uses OSS `/dev/dsp`; `padsp` routes it through
PulseAudio/PipeWire.

The game renders text from the font caches in `base.vl2` and, for other sizes,
from the TrueType files in your install's `base/fonts/` (the Loki release ships
Arial and Verdana there). It aborts if Arial is missing, so keep that folder
intact; no font files come from this repository.

## Accounts

### TribesNext login

Log in with your TribesNext name and password. The hook watches the login
fields in memory, verifies the password against the encrypted key in
`base/private.store`, and uses the recovered private exponent to answer the
server's challenge. Passwords and private exponents are never written to disk
or included in diagnostics; the temporary C password buffer is cleared after
verification, and challenge input reaches `rsa_helper` over a pipe, not its
command line.

The game sends the login name in capitals, whatever you type. With WON
emulation the emulator answers with the account's own name (a TribesNext
account's certificate name, or a Native account's name as created), so players
appear as registered rather than in capitals. A wrong password or unknown name
gets the game's own "Please check your login name and password" message.

The published TribesNext source does not include the production
certificate-signing key, so T2Launch sends the official AuthServer certificate
unchanged and leaves its issuer signature to the game server.

### Creating an account in the game

With WON emulation on, the stock **Create New Account** button works. The new
account goes to the first checked Auth backend (see [Backends](#backends)):

- **TribesNext** (the default): registered with the official TribesNext
  account server and installed in `base/public.store` and
  `base/private.store`, exactly as `tribesnext.py create-account` does below.
  Only possible on the machine running the emulator; another machine gets a
  Native account instead.
- **Native**: an account in the emulator's own database.

The game uppercases the login name before sending it, so the account takes its
capitalization from the warrior name when the two are the same name. TribesNext
accounts have a single name; a different warrior name is ignored for them.

The dialog asks for a CD key and checks it itself, offline, before sending
anything. Use your own key or generate one:

```bash
python3 wonemu.py --generate-cd-key
```

Nothing verifies keys any more, and the emulator discards the key after
checking its format; account keys always come from `/dev/urandom`. Turn the
dialog off with the **Allow account creation** profile option
(`T2LAUNCH_WON_ACCOUNT_CREATION=0`).

### Logging in on another machine

Logging in with a TribesNext name that is not installed on this machine fetches
it from TribesNext (`RECOVER`, using the name and password) and installs it, so
an existing account works on a fresh install. A failed lookup is not retried for
five minutes with the same name and password. Routine logins of an installed
account do not contact TribesNext; to refresh its stores anyway, start the
emulator with `--refresh-tribesnext` or set `T2LAUNCH_REFRESH_TRIBESNEXT=1`.

### Edit Account and forgotten passwords

**Edit Account** shows and updates the account's email and "send me
information" setting, which are kept on this machine, and changes Native
passwords. TribesNext has no way to change a password: the stock dialog reports
an error if the password is changed, and saves nothing.

The emulator cannot send email and TribesNext has no password reset, so the
"email me my login info" requests always fail; a login name that does not exist
gets the stock "No such login name" message.

### Creating an account from the shell

## WON emulator

`wonemu.py` stands in for Sierra's WON services: the directory, login,
community database (DBProxy), IRC, and master server.

### Local mode

Set **WON emulation** to `local` on a profile's WON emulator tab and leave
**Load t2_hook.so** enabled (on the command line, `T2LAUNCH_WON_MODE=local`).
The launcher starts or reuses the emulator and checks it is ready before
starting the game. Readiness includes an implementation fingerprint and the
service's addresses, ports, and master sources, so a running emulator is
restarted when its code or configuration changes, and an unrelated process on
the port is never reused. It keeps running across game instances.

By default it listens on `127.0.0.1`. **Listen on** and **Port** move the
directory; the other four ports always follow it:

| Port | Handler |
|------|---------|
| TCP 15104 | WON directory (GetDir) and service discovery |
| TCP 15105 | Signed public-key block and Auth1 login |
| TCP 15106 | DBProxy community database |
| TCP 15107 | IRC certificate/challenge login and chat |
| UDP 15108 | Torque master protocol and TribesNext list proxy |

The hook points the game at the emulator by replacing the three Sierra
bootstrap strings in the running client's memory when it loads, before
`console_start.cs` calls `WONInit()`.
They come from the profile's **Game connects to** field
(`T2LAUNCH_WON_ENDPOINTS`): one `host:port`, optionally followed by up to two
comma-separated fallbacks, each at most 27 characters (the size of the original
strings). Left empty in local mode, it is the listen address and port. The hook
checks the original strings first and restores page permissions afterward; on
an unsupported executable it fails instead of patching.

### Remote mode

Set **WON emulation** to `remote` to use an emulator on another machine without
starting one locally.

1. On the server machine, set **Listen on** to a LAN address or `0.0.0.0` and
   launch once so the emulator starts. It prints a warning, since the login,
   community, and master services become reachable on the network.
2. On each client, enter the server's `host:port` in **Game connects to**.
3. Copy the server's `~/.local/state/t2launch/wonemu/verifier.der` to each
   client and select it as **Remote verifier file**; the game checks the
   emulator's signatures against that key.

Remote clients log in with the server's own T2Launch accounts. TribesNext
accounts work only in local mode, because checking a TribesNext password needs
that account's key files on the machine doing the check; the emulator never
uses its installed TribesNext keys for a login from another machine. It answers
status and stop requests only from its own machine (including its own LAN
address), so a LAN-reachable instance cannot be stopped remotely.

### Backends

In local mode the WON emulator tab lists the emulator's backends in priority
order (↑/↓ to reorder), with a column per service, each with a checkbox and,
where the service needs one, an address:

| Backend | Auth | Master (server list) | IRC |
|---|---|---|---|
| TribesNext | the account's installed key files (this machine only) | HTTP list, `http://master.tribesnext.com` | community data through the robot API; with a chat server address, chat relayed to that IRC network |
| Native | the emulator's own accounts | servers that heartbeat to this emulator | the emulator's community store and its own chat |
| Torque master (any number) | — | another Torque-protocol master, `host:port` | — |

TribesNext is first by default, with every service checked, then Native. Tribes
2 sees one login, one server list, and one community, whichever backends supply
them:

- **Auth** tries each checked backend in order; the first that accepts the
  password logs the player in. A wrong password or unknown account falls
  through to the next.
- **Master** merges the server lists of every checked backend, in order. Local
  game servers' heartbeats are forwarded to TribesNext when it is checked.
- **IRC** serves a player's own data (buddies, T-Mail, history, memberships)
  from the backend that logged them in, or the next checked backend that can:
  TribesNext community data needs a TribesNext login; Native serves anyone.
  Player and tribe searches merge every checked backend, and a name TribesNext
  does not know opens its native profile. Chat goes to the first checked
  backend that offers it.

With a chat server entered in TribesNext's IRC cell, the emulator connects each
player to that IRC network under their account name (as TribesNext's own client
fix does), relays chat both ways, and keeps DBProxy and the WON handshake local.
If the chat server cannot be reached or drops, the player gets a notice and chat
falls back to the emulator's own. The address is empty by default: the server
TribesNext's published client fix used, `irc.arloria.net:6667`, no longer
resolves, so enter the current one to turn the relay on.

On the command line, `--backends` (or `T2LAUNCH_WON_BACKENDS`) takes the list as
JSON:

```json
[{"kind":"tribesnext","auth":true,"master":true,"irc":"chat.example.net:6667"},
 {"kind":"native","auth":true,"master":true,"irc":true}]
```

`"irc": true` serves TribesNext community data without relaying chat. The older
`--master-sources kind=address,...` and `--master-backend local|tribesnext|hybrid`
options set only the master column; `--tribesnext-master-url` points at another
compatible HTTP list.

### Local accounts

Native accounts are stored in `community.sqlite3` as salted PBKDF2 password
hashes, independent of Sierra/WON signing keys and `private.store`. Create one
with the game's **Create New Account** dialog when Native is the first checked
Auth backend (see [Accounts](#accounts)), or reserve a name and enter it with a
password in the game's online login screen; the first login sets the password:

```bash
python3 wonemu.py --reserve-local-account NAME
```

Accounts in `private.store` are still accepted as a legacy fallback (the store
beside the game binary, or `--private-store`).

### Community features

For TribesNext logins, community features use the TribesNext robot API:

- player and clan search, profiles, and rosters; profile memberships; player
  history (`uhist`)
- buddy and block lists (`robot_mail.php` `viewlist`/`addlist`/`dellist`)
- T-Mail (`box`/`read`/`send`/`delete`). The stock client caches whole
  messages, so each inbox poll reads the bodies of up to 25 new messages, which
  marks them read on TribesNext. Clan invitations arrive as `invite` T-Mail with
  a JSON body and are shown with the stock accept/reject links, which answer
  through `uaccept`/`ureject`. TribesNext drops mail sent to yourself.
- TribesNext has no player picture, so a chosen picture is kept locally by
  certificate GUID (visible only on this machine).

TribesNext sessions do not fall back to local records when a robot lookup fails,
and local-only operations are rejected for them. For native logins, the
emulator's SQLite community store provides:

- player search, profiles, descriptions, pictures, and profile history
- tribes: search, profiles, rosters, creation, settings, membership roles,
  primary tribe, and invitations
- buddy lists, online presence, and T-Mail (send, inbox, read, delete, deleted
  mail, sender block lists)
- news articles (post, edit, and delete by the author), the MOTD, and the
  classic web-link rows
- forums: a General board and tribe forums, with posts editable by their author
  and topics manageable by tribe moderators. The stock #25034 shell has no
  Forums tab, so these are reached only through existing forum links.

The MOTD on the News tab can be edited by the native accounts listed in the
WON tab's **MOTD editors** option (advanced). Those logins carry the stock
client's news-editor membership (tribe id 1402), so the stock Edit button
appears; editors can also edit or delete any news article. Local tribes never
get ids 1401 or 1402, which the stock client treats as staff.

The stock client sends no other DBProxy queries. A community query wonemu
doesn't know gets an "Unsupported DBProxy query N" reply, logged as
`irc_dbproxy_query` with its number and argument length; an unknown message
on the account service is logged as `unhandled` with its type and SHA-256.

### Running it by hand

```bash
python3 wonemu.py          # start in the foreground
python3 wonemu.py --stop   # stop the running instance
```

State and logs are in `${XDG_STATE_HOME:-~/.local/state}/t2launch/wonemu/`:

| File | Contents |
|------|----------|
| `keys.json` | the generated private root and auth keys |
| `verifier.der` | the public verifier that remote clients need |
| `community.sqlite3` | accounts and community data (`--community-db` moves it) |
| `traffic.jsonl` | decoded directory queries and message metadata; frames appear only as a size and SHA-256 |
| `wonemu.log` | stdout/stderr when started by the launcher |

No frame is logged as bytes, encrypted or not: an encrypted login could be
decrypted with `keys.json`. Credentials, emails, and CD keys never reach the
logs.

Other options: `--auth-port`, `--dbproxy-port`, and so on move individual
listeners; `--auth-key-profile native` uses the client's embedded 2048-bit
group instead of the default compact `compat` group; `--empty-directory` serves
successful directory queries with no entities.

The directory is contacted only when the game's native WON login runs;
`-nologin` skips it. To start discovery by hand from the game console:

```cs
WONInit();
WONStartLogin("wonemu_probe", "probe_not_a_real_password", false);
echo(WONLoginResult());
```

## How it works

### The hook

`t2_hook.so` is loaded with `LD_PRELOAD` on the game process only, ahead of the
bundled SDL, with `t2_realign.so` as an `LD_AUDIT` module:

```
LD_AUDIT=games/tribes2/t2_realign.so
LD_PRELOAD=games/tribes2/t2_hook.so:/usr/lib32/pulseaudio/libpulsedsp.so:runtime/compat-libs/libSDL-1.2.so.0
```

Its constructor installs the emulator's verifier when WON emulation is on.
Everything that touches TorqueScript waits for the render loop (in the
dedicated server, its main-loop `usleep`): the hook interposes
`SDL_GL_SwapBuffers`, and on the game's main thread, once the
console table exists, it registers its console commands and installs the
runtime T2CSRI (TribesNext client) handlers. The hook toggle removes only
`t2_hook.so`. The hook reads `base/*.store` from `T2LAUNCH_GAME_DIR` and runs
the RSA helper from `T2LAUNCH_RSA_HELPER`, so nothing is copied into the game
folder.

It also fixes the #25034 Community Browser in memory: the local player's broken
TRIBES-tab callback is replaced with a `DatabaseQueryArray(13, ...)` request
that prefetches tribe memberships, adds them to the browser tabs and membership
checks, and repaints the member list when rows arrive. An empty AuthInfo
display name falls back to the saved login name, and IRC nickname filtering is
disabled on tribe creation fields.

### Runtime compatibility

The project first ran the game in a 32-bit Ubuntu 16.04 Docker image. That image
supplied nothing a current multilib host cannot, but four behaviors of the game
need handling on current 32-bit libraries:

- **Stack alignment.** Tribes 2 and its Loki libraries keep the 4-byte stack
  alignment of 2001 compilers; current 32-bit libraries assume 16 bytes and use
  SSE stores such as `movaps` on stack slots. The first crash is in libX11's
  `_XSend`, reached from the bundled SDL's `XSync`. When an old object binds a
  function in a modern system library (including through `dlsym` and
  `glXGetProcAddress`), `t2_realign.so` returns a thunk that realigns the stack
  and copies sixteen argument words. glibc's core libraries are excluded so
  `setjmp` and friends never run in an extra frame. The hook and helpers are
  built with `-mstackrealign`.
- **Joining detached threads.** At shutdown and while loading missions the
  client joins threads it created detached. LinuxThreads returned `EINVAL`;
  NPTL reads the freed thread descriptor. The hook interposes `pthread_create`,
  `pthread_detach`, and `pthread_join`, and refuses joins on threads it did not
  record as joinable.
- **Back-buffer contents.** Torque's GUI redraws only dirty regions and assumes
  the back buffer still holds the previous frame. Current drivers do not
  preserve it, which shows as flicker and ghost cursors. With
  `T2LAUNCH_FULL_REDRAW=1` (the default) the hook calls `Canvas.reset()` after
  every swap, so each frame is drawn in full.
- **Volume controls.** Torque plays interface sounds, the shell hum, and music
  as `AL_SOURCE_RELATIVE` sources and sets their gain with Loki's linear
  `AL_GAIN_LINEAR_LOKI`. The bundled `libopenal.so.0` applies source gain in its
  distance-attenuation filter, which returns early for relative sources, so
  every volume slider was ignored. `t2_realign.so` tracks relative sources and
  also sets `AL_MAX_GAIN`, which Loki's min/max filter still applies, to the
  value that yields the requested linear gain (Loki maps it non-linearly; the
  table was measured from its output).

`MESA_EXTENSION_MAX_YEAR=2000` and `__GL_ExtensionStringVersion=17700` keep the
GL extension string short enough for the client's fixed buffer.

### Game clock

The Linux client's `Platform::getRealMilliseconds` (`0x081CFA44`) divides
`rdtsc` ticks by a rate it measures against `gettimeofday` for 100 ms at
startup, the same timestamp-counter clock behind the Windows multi-core
speed-up reports. CPUs that report `constant_tsc` and `nonstop_tsc`, with the
kernel's clocksource set to `tsc`, keep the counter in step across cores. Older
multi-core CPUs, some virtual machines, and hosts whose kernel marks the TSC
unstable do not. With the **Game clock from the system monotonic clock** hook
option (`runtime.monotonic_clock`, on by default) the hook checks the
function's prologue and replaces it in memory with a jump to a
`CLOCK_MONOTONIC` clock before the game starts. The log names the kernel
clocksource. No CPU affinity is needed.

### Other reported Tribes 2 issues

- **Network rates.** The engine limits packet rate to 32 per second each way
  and packet size to 450 bytes. The stock defaults are the 56K preset
  (16/20/240); the stock Options → Network **T1/LAN** preset already sets the
  maximum (32/32/450). Servers set the same `$pref::Net::` values in their
  ServerPrefs. No hook change is needed.
- **Crash dialogs and server restarts.** On Linux a crash does not wait on a
  dialog: Loki's signal handler prints `BUG! (Segmentation Fault)  Going down
  hard...` and exits. `$pref::Engine::ExitOnException` and
  `purgeNativeTextures()` do not exist in the Linux binaries. The stock scripts
  already call `purgeResources()` on each mission load. Dedicated servers run
  `tribes2d`, which the hook does not load into; the stock
  `tribes2d-restart.sh` restarts it after a crash.
- **Servers with many bots.** `queryMasterServer()` given only a port (the
  browser's "All servers") defaults the maximum bot count to 16
  (`0x082E6B23`), and the client drops servers reporting more;
  `ChooseFilterDlg.cs` also clamps a filter's Max Bots to 16. With **List
  servers with more than 16 bots** (`browser.many_bots`, on by default) the
  hook raises the default to 255 in memory and redefines
  `FilterEditDlg::setMaxBots` at runtime to allow 0-255. Counts are compared
  unsigned, so values above 127 are handled as written. A server reporting
  200 players, 255 maximum, and 150 bots lists and opens its info dialog
  without the crash reported for the Windows client. A player list longer
  than one 1500-byte reply (200 rows, 4803 bytes declared, 1446 sent) shows
  the 62 complete rows that arrived.

### Quality-of-life options

The TribesNext "QoL fixes" update is Windows only. These hook and launcher
options bring the parts of it that fit the Linux client:

- **HUD bar values** (`ui.hud_get_value`, on) and **vertical HUD bars**
  (`ui.vertical_fill`, on). The health, energy, capacitor and heat bars
  (`HudDamage`, `HudEnergy`, `HudCapacitor`, `HudHeat`, all `HudBarBaseCtrl`)
  register a `value` and a `verticalFill` field, but the game never stores the
  fill it draws and only fills left to right. The hook replaces the part of the
  bar's `onRender` (`0x0818EA5E`) that asks the bar for its fill: the fill is
  stored in `value`, so a custom HUD can read `HudDamage.getValue()`, and a bar
  with `verticalFill = 1` fills from the bottom up. This is the Linux version of
  the Windows `l33thax` patch.
- **Observer IFF** (`ui.observer_iff`, off). The IFF render draws no markers
  while the control object is an observer camera; the hook skips that check
  (`0x081936BF`), as the Windows `l33thax` patch did.
- **Web links** (`browser.web_links`, on). Loki's `gotoWebPage()` formats
  `$LOKI_BROWSER` with the URL and runs it with `system()`, or searches for
  netscape and mozilla. Unless `LOKI_BROWSER` is already set, the hook sets it
  to a marker that its `system()` turns into `xdg-open <url>`: one argument,
  no shell, only `http`, `https`, and `ftp` URLs without quotes, spaces, or
  backslashes, and without the game's `LD_*` loader settings.
- **Packet rate and size** (`net.packet_limits`, on). The client clamps
  `$pref::Net::PacketRateToClient` to 32 and `$pref::Net::PacketSize` to 450
  at three sites (`0x081F8EAE`, `0x081FB382`, `0x081FB683` and the size
  checks after them) before asking the server for that rate. The hook raises
  the limits to 64 and 1000, widens the Network options sliders to match, and
  adds a "TribesNext QoL" preset (64/32/1000). A connection takes the larger
  update delay and the smaller packet size of the two sides' requests
  (`0x081F96A4`), so a stock server keeps sending at its own 32/450; QoL
  servers accept the higher request.
- **Frame rate limit** (`video.fps_limit`, 0 = off). After each
  `SDL_GL_SwapBuffers` the hook sleeps to an absolute `CLOCK_MONOTONIC`
  deadline; a frame that runs more than one period late restarts the
  schedule rather than rushing to catch up.
- **Borderless window** (`video.borderless`, off). Windowed
  `SDL_SetVideoMode` calls get `SDL_NOFRAME`, and `SDL_VIDEO_CENTERED=1`
  places the window. Choose the desktop resolution for borderless fullscreen.
- **VSync** (`video.vsync`, on). The Linux client never probes for a
  swap-control extension, so `$SwapIntervalSupported` stays unset, the Video
  options checkbox is disabled, and `setVerticalSync()` does nothing. The hook
  sets `$SwapIntervalSupported`, replaces `setVerticalSync()` with one that
  calls `glXSwapIntervalEXT` (or the MESA or SGI variant), applies
  `$pref::Video::disableVerticalSync` at startup, and reapplies the setting
  after a video mode change.
- **Gamma** (`video.gamma`, on). The Video options gamma slider (0 to 1)
  reaches `SDL_SetGamma` as gamma 0.48 to 1.48, which sets X11 gamma ramps;
  XWayland and most compositors ignore them. The hook records the value and,
  before each swap, copies the back buffer into a texture and draws it back
  through a GLSL shader computing `pow(colour, 1/gamma)`, saving and
  restoring the GL state around the pass. Neutral is near the middle of the
  slider.
- **Hide `EXT_compiled_vertex_array`** (`video.disable_locked_arrays`, off).
  Some drivers (AMD's rewritten Windows OpenGL driver, per TribesNext forum
  thread 4362) tear and flicker after the game first calls `glLockArraysEXT`,
  and `$pref::OpenGL::disableEXTCompiledVertexArray` does not cover every
  use. The game resolves GL through `SDL_GL_GetProcAddress`; with this option
  the hook hands it a `glGetString` whose extension list omits the extension
  and no `glLockArraysEXT`, so the console lists it under "Disabled
  Extensions". Mesa's radeonsi is not known to be affected, so it is off by
  default.
- **Gamescope** (`video.gamescope`, off). `run-game` starts the game inside
  gamescope with the chosen game and output sizes, scaler, filter (`fsr`,
  `nis`, ...), fullscreen, and relative mouse mode. gamescope itself runs
  without the game's `LD_*` settings, and `window_resize.sh` is skipped.
  Rendering the game below the output size and upscaling it is the Linux
  stand-in for the update's UI scaling; keep the game windowed at the game
  size.
- **Missing map files** (`net.depot_downloads`, off). When a server's map
  uses an interior, shape, or terrain the client lacks, the game disconnects
  with "Unable to load interior: X", "Unable to load shape: X", or "You are
  missing a file needed to play this mission: X". With this option the hook
  wraps the stock `onConnectionToServerLost` in a package: `depot.py` looks
  the file up in the TribesNext depot index
  (`https://www.tribesnext.com/files/depot.txt`, cached for a day), downloads
  it from `depot.tribes2.net`, checks its size and MD5, and saves it under
  `~/.loki/tribes2/base/interiors`, `shapes`, or `terrains` with the name the
  game asked for. The game then rescans with `setModPaths(getModPaths())`
  and rejoins, once per missing file and up to 32 files per server. A server
  that answers the rejoin with "Server is cycling missions" (a map change,
  or an empty server reloading its map) is retried every 5 seconds for a
  minute. Unknown files, failed downloads, and a file that is still missing
  after its download keep the stock DISCONNECTED box. Missing textures don't
  stop a map loading, so they aren't fetched.
- **Native TribesNext authentication** (`net.native_auth`, off). QoL servers
  can authenticate accounts in the connection handshake, and those with
  `$Host::RequireAuth` reject clients that don't. Once the TribesNext account
  is unlocked, the hook retargets the two `writeFlag` calls that send the WON
  certificate flag (`0x08236844` in the challenge request, `0x08237556` in
  the connect request) and replaces the inline flag read in the challenge
  response (`0x082376E2`). The challenge request then carries the QoL
  client's native flag: `0x54`, an 8-byte nonce, the server's IPv4 address,
  and the account blob (name, GUID, the key's exponent and modulus as bytes,
  and the 512-byte certificate signature). A QoL server answers with a
  challenge encrypted to the account key; the hook decrypts it with
  `rsa_helper`, finds the nonce, and sends 40 bytes of it back in the connect
  request. QoL servers read all of this at protocol 51 as well, and their
  protocol-52 game data would not parse in this client, so it keeps
  announcing 51. A stock server ignores the extra bits; one logged in to WON
  accepts the connection without checking a WON certificate.
- **Discord presence** (`discord.presence`, off). Shows the menus, or the
  map, game type, server and player count while you're in a game, in the
  Discord desktop client on the same machine. The stock client keeps what the
  server told it on joining in `$clServerName`, `$clMissionName` and
  `$clMissionType`; every 5 seconds the hook asks a runtime
  `t2launch_presence()` for the state and writes each change as a line to
  `discord_presence.py`, which it starts beside the game. The helper sends it
  over Discord's IPC socket (`$XDG_RUNTIME_DIR/discord-ipc-N`, including the
  Flatpak, Snap and Vesktop locations), connects whenever Discord is running,
  and exits when the game does. `discord.app_id` (advanced) picks the Discord
  application; its art asset should be named `tribes2`.

## Development

### Repository layout

```
t2launch/
├── runtime/
│   ├── run-game               # Launch script
│   ├── build                  # Compile hook/helpers with the host 32-bit toolchain
│   ├── common.sh              # Shared shell helpers
│   ├── tribes2.cfg            # Per-game config
│   ├── window_resize.sh       # Follow game mode and selected display
│   └── compat-libs/           # Loki-era compatibility libraries
├── games/tribes2/             # Built t2_hook.so, rsa_helper, t2_realign.so
├── t2_hook.c                  # LD_PRELOAD hook and runtime game patches
├── t2_realign.c               # LD_AUDIT stack-realignment and OpenAL shim
├── rsa_helper.c               # GMP-backed RSA helper
├── wonemu.py                  # WON directory/auth/DBProxy/IRC/master emulator
├── woncrypto.py               # WON wire-format crypto
├── community_store.py         # SQLite community store
├── tribesnext.py              # TribesNext certificate/challenge/account helpers
├── depot.py                   # TribesNext depot download for missing map files
├── discord_presence.py        # Discord rich presence over the local IPC socket
├── t2options.py               # Profile option declarations
├── t2launch_app.py            # Desktop launcher
├── install.sh                 # Launcher installer and dependency check
└── tests/
```

Run the tests with `python3 -m unittest discover -s tests -v`.

Adding a profile option means one `Option(...)` entry in `t2options.py`, plus a
row in the hook's `hook_options[]` table when the hook applies it;
`tests/test_options.py` checks the two lists agree. New emulator backends are
added as new kinds in `BACKEND_KINDS` in `wonemu.py`, mirrored in
`t2options.py`.

### Debugging

Check that the hook loaded:

```bash
./runtime/run-game tribes2 ~/t2-linux -nologin 2>&1 | grep Registered
```

From the game console, `echo(hook_sha1("abc"));` should print
`a9993e364706816aba3e25717850c26c9cd0d89d`.

To see game console output in the terminal, add to `base/autoexec.cs`:

```cs
$Con::logBufferEnabled = false;
$pref::NoClearConsole = true;
$winConsoleEnabled = true;
enablewinconsole(true);
```

### Hook console commands

| Command | Args | Description |
|---------|------|-------------|
| `hook_test` | none | Returns "ok_from_c" |
| `hook_option` | `<key>` | Value of a hook option from the profile config |
| `hook_sha1` | `<string>` | SHA1 hash → 40-char hex |
| `hook_rsa_mod_exp` | `<base_hex> <exp_hex> <mod_hex>` | RSA modular exponentiation |
| `hook_rand64` | none | Random unpadded 64-bit hex |
| `hook_profile_browser_wake` | none | Confirm runtime browser integration |
| `hook_tn_unlock` | `<username> <password>` | Unlock the installed key in memory; no password logging |
| `hook_tn_certificate_length` | none | Installed certificate length |
| `hook_tn_certificate_chunk` | `<offset>` | One certificate chunk |
| `hook_tn_decrypt` | `<hex>` | Decrypt a server challenge with the unlocked key |
| `hook_tn_clear` | none | Clear in-memory account material |
| `hook_tn_trace` | `<known_stage> [safe_state...]` | Safe diagnostic marker for auth stages |
| `hook_tn_join_trace` | `<address> <ready>` | Log the selected server and key-ready state |
| `hook_tn_ip_trace` | `<ip> <hex>` | Log the parsed IPv4 used in the client challenge |
| `hook_tn_ipv4_hex` | `<dotted_ipv4>` | Encode the T2CSRI IPv4 challenge suffix |
| `hook_tn_hex_ipv4` | `<hex>` | Decode a T2CSRI IPv4 suffix to dotted form |
| `hook_tn_verify_cert` | `<certificate>` | 1 if the TribesNext root key signed the account certificate |

### Binary addresses (Tribes 2 Linux #25034)

| Function | Address | Convention |
|----------|---------|------------|
| `Con::addMethodS` | `0x080E85C0` | `__cdecl` |
| `Con::addMethodV` | `0x080E8618` | `__cdecl` |
| `Con::addMethodI` | `0x080E8670` | `__cdecl` |
| `Con::addMethodB` | `0x080E8720` | `__cdecl` |
| `Con::evaluate` | `0x080E88F4` | `__cdecl` |
| `Con::execute` | `0x080E7560` | `__cdecl` |
| `Con::getReturnBuffer` | `0x080F1C9C` | `__cdecl` |
| Console global ptr | `0x08493DEC` | — |
| `Sim::postEvent` | `0x080FE264` | `__cdecl` |
| `WONGetAuthInfo` | `0x0834E804` | console callback |
| AuthInfo parser | `0x08348FCC` | `__cdecl` |
| `Auth2Certificate::UnpackData` | `0x083615A4` | `__thiscall` |

The dedicated server (`tribes2d.dynamic`) is a separate build with the same
engine code at other addresses. The hook picks its address set from the
executable's name and checks the bytes at every site before patching:

| What | Client | Server |
|------|--------|--------|
| `Con::addMethodS` / `Con::evaluate` | `0x080E85C0` / `0x080E88F4` | `0x080E74B0` / `0x080E77E4` |
| Console global ptr | `0x08493DEC` | `0x0848902C` |
| `Platform::getRealMilliseconds` | `0x081CFA44` | `0x081CD014` |
| WON bootstrap strings | `0x084846B1` | `0x08479751` |
| WON verifier slot | `0x0849A860` | `0x0848F520` |
| Packet rate/size clamps | `0x081F8EAE`, `0x081FB382`, `0x081FB683` | `0x081EDB4E`, `0x081F0022`, `0x081F0323` |
| Challenge reply "server has a certificate" flag | — | `0x0822B604` |
| LAN-only check for servers without a certificate | — | `0x0822BC85` |

The server has no `SDL_GL_SwapBuffers`. `Platform::process` (`0x081CF400`)
ends each main-loop pass with `usleep(1000)`, so the hook interposes `usleep`
and does its main-thread work there.

### AuthInfo (`WONGetAuthInfo`)

The game's "Failed to get account information" check reads `WONGetAuthInfo()`.
The client builds that text from the current Auth2 certificate (global
`0x086C0434`). It ignores the identity data (type 0) and decodes the first
community-data entry (Auth2 data type 1: `u32 communityId, u16 len, blob`),
whose blob is little-endian:

```
u8 tribeCount (<= 10; the client does not check)
  per tribe: u32 id, u16+tag (<= 15), u8 appendTag, u16+name (<= 63), u32 rank,
             u16+title (<= 40) only when rank > 9
u16+warrior name (<= 31)
```

If a length is over its limit, or the entry is missing, `WONGetAuthInfo()`
returns `""`. `woncrypto.t2_auth_info_blob()` builds this blob, and
`Emulator.auth_info_tribes()` fills it at login from TribesNext or the local
community store. The value stays set across game-server disconnects, so the
game's own `checkNamesAndAliases()` runs unmodified.

### Auth protocol notes

The auth listener serves a key block signed with a per-install emulator root
key, which the client verifies against an in-memory replacement for its
embedded WON verifier. The default `compat` key profile uses a compact
safe-prime group to avoid a legacy-client overflow in its 2048-bit ElGamal
output. Message 3 is decoded through the ElGamal wrapper and Blowfish
login-data transform; a valid account completes the encrypted message-4/5
challenge and receives a signed Auth1/Auth2 certificate reply with an encrypted
session-key confirmation. The reply carries the account's registered name, not
the client's uppercased login name. An unknown account gets message 6 with
`WS_AuthServ_UserNotFound` (-1506) and a wrong password
`WS_AuthServ_BadPassword` (-1507), which the stock script shows as "check your
login name and password"; a malformed exchange gets -1.

Auth1 sessions are cached in memory for the IRC listener, whose
`CERT`/`CHALLENGE`/`CHALRESP` exchange encrypts CB33/CB35 with the client's
Auth1 key and includes the signed Auth2 certificate. It validates the known
CB34 proof encoding, with a loopback-only path for legacy CB34 layouts bound to
a verified Auth1 session. DBProxy requests arrive as chunked `DBQA` messages
and long results go out as `DBMSG` continuations.

### DBProxy account messages

The login screen's account dialogs talk to the DBProxy port (service 14).
Requests are message 1002 with a sub type; the reply's sub type is one higher,
and every reply is `status | u16-length text | 0x80 | body`. A status from the
stock script's list (`-2803` name taken, `-2804` bad name, `-2806` no such name,
`-2807` bad CD key) shows its own message; another code with text shows
`Error - <text>`.

| Sub | Dialog | Transport |
|-----|--------|-----------|
| 2 | Create Account: login, password, email, flags, CD key and warrior name | Message 2 fetches DBProxy's public key (reply 3), message 4 sends an ElGamal-wrapped Blowfish key, message 5 carries the encrypted request; the reply is in the clear |
| 6 / 4 | Edit Account: fetch email and "send info", then update password, email, flag | Peer authentication (`0xCB` 0x32–0x35, as for IRC, with a DBProxy certificate signed by the auth key), then `0x0C \| u16 id \| CryptTransform(u16 sequence \| message)` keyed by the 0x33 challenge, on later connections too |
| 8 / 14 | Email my password (by login name) / my login name (by email) | In the clear; their failures carry no text |
| 16 | Account-name lookup | In the clear |

## Credits

- **TribesNext** (tribesnext.com) — community authentication system
- **t2-mapper** (exogen/t2-mapper) — protocol and crypto reference implementation
- **IFC22** — binary address reference and API documentation
- **Loki Software** — Linux port of Tribes 2
- **tribes-2-docker** — the original Docker launcher this project started from

## License

This project is for educational and preservation purposes. Tribes 2 is
copyrighted by Dynamix/Interplay.
