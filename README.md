# t2launch

Tribes 2 Linux Online Launcher — get Tribes 2 online play working on Linux without Wine.

## Goal

Run the Tribes 2 Linux client (Loki native port, 2001) and connect to the TribesNext online multiplayer servers, without using Wine or any Windows compatibility layer.

## Background

Tribes 2 used GameSpy for online play, which shut down in 2004. The TribesNext community patch replaced GameSpy with a custom authentication system that uses RSA challenge-response for login. The TribesNext RC2a patch was only released as a Windows `.exe` modifier, which means Linux users had to run the entire game under Wine.

This project replaces the Ruby-based crypto layer (which the Windows patch depends on) with native C implementations, allowing the Loki Linux binary to authenticate directly with TribesNext servers.

## Architecture

```
t2launch/
├── runtime/                   # Launch runtime
│   ├── run-game               # Launch script
│   ├── build                  # Compile hook/helpers with the host 32-bit toolchain
│   ├── tribes2.cfg            # Per-game config
│   ├── window_resize.sh       # Follow game mode and selected display
│   └── compat-libs/           # Loki-era compatibility libraries
├── games/tribes2/             # Built t2_hook.so, rsa_helper, t2_realign.so
├── t2_hook.c                  # LD_PRELOAD hook and runtime game patches
├── t2_realign.c               # LD_AUDIT stack-realignment shim
├── wonemu.py                  # Local WON/Auth/IRC/master services
├── tribesnext.py              # TribesNext certificate/challenge helpers
├── woncrypto.py               # WON wire-format crypto
└── t2launch_app.py            # Desktop launcher
```

Your original Tribes 2 Linux game files (not included) go in `~/t2-linux/`:
```
~/t2-linux/
├── tribes2.dynamic            # Loki Linux binary (#25034)
├── base/                      # Original game data and scripts
└── *.vl2                      # Script archives
```

## How It Works

1. **LD_PRELOAD Hook**: `t2_hook.so` registers native console helpers and defers TorqueScript work to the game thread through the SDL event loop.
2. **Native SHA1**: `hook_sha1()` — SHA1 hash computation in C, callable from TorqueScript
3. **Native RSA**: `hook_rsa_mod_exp()` — modular exponentiation for 2048-bit RSA
4. **Credentials**: Store files (`public.store`, `private.store`) for TribesNext certificate/key
5. **Runtime T2CSRI auth**: The native hook unlocks the existing stored key in memory and handles TribesNext certificate/challenge messages without installing or editing game scripts.

`tribesnext.py` implements the official TribesNext account-recovery protocol and
testable certificate/key/challenge helpers. The published TribesNext source does
not contain the production certificate-signing trust key; T2Launch therefore
preserves the official AuthServer certificate unchanged and lets the connected
game server verify its issuer signature. The hook verifies the entered password
against the encrypted private-store key and uses the recovered private exponent
for the server challenge. Passwords and private exponents are not written to disk
or included in diagnostics. If the local WON profile has a different password,
wonemu accepts the TribesNext password only when it validates against that
account's installed private-store entry. On a validated TribesNext login, wonemu
uses the cached installed key; it does not contact the official account-recovery
service on routine logins. To refresh an existing account explicitly, start the
service with `--refresh-tribesnext` or set `T2LAUNCH_REFRESH_TRIBESNEXT=1`.
Refreshes atomically update the public/private stores without creating or renaming
an account.

New accounts can be registered with the official `NAME`/`SIGN` protocol after
building the RSA helper overlay. Run `python3 tribesnext.py create-account NAME`
to check name availability, generate a 1024-bit key using `/dev/urandom` and
GMP, submit the signed registration, then install the returned certificate and
password-encrypted private exponent in the selected game's `base/public.store`
and `base/private.store`. The password is prompted interactively and is not
sent to the authentication server. Use `--game-dir` for a game directory other
than `~/t2-linux`; `--key-bits` permits the legacy 512/768-bit sizes when needed.

For the #25034 profile browser, the hook also replaces the local player's broken
TRIBES-tab callback at runtime with a `DatabaseQueryArray(13, ...)` request. This
prefetches tribe memberships for the first browser wake, adds those tribes to the
browser tabs and membership checks, and repaints the member list when rows arrive.
It also falls back to the saved login name for an empty AuthInfo display name
and disables IRC nickname filtering on
tribe creation fields. These are in-memory GUI/script changes; no `.cs` files or
archives on disk are modified.

## Quick Start

### Install

You need your own Tribes 2 Linux game files (Loki release #25034) and, for
online play, a TribesNext account.

T2Launch runs the 32-bit game on your distribution's 32-bit libraries, so the
host needs multilib support. On Arch, enable the `[multilib]` repository in
`/etc/pacman.conf` if it is not already (CachyOS enables it by default):

```ini
[multilib]
Include = /etc/pacman.d/mirrorlist
```

Then install the packages:

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

`gamescope` and `strace` are optional (for `USE_GAMESCOPE=1` and
`T2LAUNCH_TRACE_CONNECTS=1`). `install.sh` checks for each of these (32-bit
libraries through the `ldconfig` cache, the compiler with a trial `gcc -m32`
build) and prints one install command for whatever is missing; it still
installs the launcher. The launcher also reports any missing 32-bit library by
name before it starts the game.

Debian/Ubuntu and Fedora are untested but supported by the launcher. The game
starts through `/lib/ld-linux.so.2`, which each distribution's 32-bit C library
provides, and the launcher finds the 32-bit `libpulsedsp.so` under
`/usr/lib32/pulseaudio` (Arch), `/usr/lib/i386-linux-gnu/pulseaudio`
(Debian/Ubuntu), or `/usr/lib/pulseaudio` (Fedora), skipping 64-bit copies. When
something is missing, it prints the install command for the detected
distribution. The rough equivalents of the Arch list are:

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

T2Launch provides a system-tray desktop app for running the native client with
saved profiles. It runs the game on the host's 32-bit libraries (see *Running
the game* below); nothing is downloaded and no game file is modified.

Install the desktop app (into a private Python environment with PySide6) and
start it:

```bash
./install.sh
~/.local/bin/t2launch
```

Open **Settings** from the tray menu to choose the game directory, create/edit
profiles, choose the fullscreen display, toggle the hook, configure autolaunch,
or use **Rebuild** to recompile the support binaries. The tray menu lists
profiles alphabetically; click a profile to launch it. Hovering the tray icon
shows running profiles. The hook is enabled by default. Settings are stored in
`~/.config/t2launch/settings.json` with private permissions.
Launcher and crash-stage diagnostics are written, with login arguments redacted,
to `~/.local/state/t2launch/launch-diagnostics.log`.

### Profile options

Each profile's editor has three tabs. **Game** holds the command-line flags.
**WON emulator** and **Hook** are generated from `t2options.py`, which declares
every option once: its default, its type, what it depends on, and how it is
applied. An option whose requirements are unmet is greyed out with a
"Requires: …" note (for example, the TribesNext handlers need WON emulation and
the hook's console commands); its saved choice comes back when the dependency
does. **Show advanced options** reveals the debugging and compatibility knobs.

At launch, options become environment variables for `runtime/run-game` (WON
emulation, master sources, software GL, the volume fix, tracing), and the hook's
options are written to `~/.local/state/t2launch/hook/<profile>.conf` (one
`key=value` per line, mode 0600), passed as `T2LAUNCH_HOOK_CONFIG`. The hook
reads a value from that file, then from the option's legacy environment
variable (such as `T2LAUNCH_FULL_REDRAW`), then its default, so command-line
launches keep working. Keys the hook does not know yet are kept, and in-game
scripts can read any option with `hook_option("key")`. Adding an option means
one `Option(...)` entry, plus a row in the hook's `hook_options[]` table when
the hook applies it; `tests/test_options.py` checks the two lists agree.

### Local WON emulator and master proxy

Set **WON emulation** to `local` on a profile's WON emulator tab and leave
**Load t2_hook.so** enabled. The launcher starts or reuses `wonemu.py` and checks readiness before
starting the game. Readiness includes an implementation fingerprint and the
service's addresses, ports, and master sources, so an existing daemon is
restarted when the emulator code or its configuration changes. By default it
listens on `127.0.0.1` at these ports (**Listen on** and **Port** move them;
the other four always follow the directory port):

| Port | Handler |
|------|---------|
| TCP 15104 | WON GetDir requests and service discovery |
| TCP 15105 | Signed public-key block and local-account Auth1 login |
| TCP 15106 | DBProxy account-name lookup; other operations are captured |
| TCP 15107 | Native IRC certificate/challenge login and basic IRC commands |
| UDP 15108 | Native Torque master protocol and TribesNext discovery proxy |

The hook replaces the three Sierra bootstrap strings in the running client's
memory before WON initialization, with the profile's **Game connects to**
field (`T2LAUNCH_WON_ENDPOINTS`): one `host:port`, optionally followed by up to
two comma-separated fallbacks, each at most 27 characters, the size of the
original strings. Left empty in local mode, it is the listen address and port,
`127.0.0.1:15104` unless changed. It checks the original
strings and restores page permissions afterward. The executable on disk is
unchanged. The option is off by default; enabling it on an unsupported executable
fails instead of attempting the patch.

**Remote mode.** Set **WON emulation** to `remote` to use a wonemu on another
machine without starting one locally. On the server machine, choose a LAN
address or `0.0.0.0` for **Listen on** (wonemu prints a warning, since the login,
community, and master services become reachable on the network) and launch
once so the service starts. On each client, enter the server's `host:port` in
**Game connects to**, and copy the server's
`~/.local/state/t2launch/wonemu/verifier.der` to the client and select it as
**Remote verifier file**: the game checks the emulator's signatures against
that key. Remote clients log in with the server's own T2Launch accounts
(reserve them on the server with `python3 wonemu.py --reserve-local-account
NAME`); TribesNext accounts work only in local mode, because checking a
TribesNext password needs that account's key files on the machine doing the
check. wonemu never uses its installed TribesNext keys for a login from
another machine, and answers status and stop requests only for this machine,
so a LAN-reachable instance cannot be stopped remotely. ("This machine" includes
its own LAN address, which its local game uses when the service listens there.)

**Backends.** In local mode the WON emulator tab lists wonemu's backends in
priority order (↑/↓ to reorder) with a column per service, each with a
checkbox and, where the service needs one, an address:

| Backend | Auth | Master (server list) | IRC |
|---|---|---|---|
| TribesNext | the account's installed key files (this machine only) | HTTP list, `http://master.tribesnext.com` | community data through the robot API; with a chat server address, chat relayed to that IRC network |
| Native | wonemu's own accounts | servers that heartbeat to this emulator | wonemu's community store and its own chat |
| Torque master (add any number) | — | another Torque-protocol master, `host:port` | — |

TribesNext is first by default, with every service checked, then Native. Tribes 2
sees one login, one server list, and one community, whichever backends supply them:

- **Auth** tries each checked backend in order; the first that accepts the
  password logs the player in. A wrong password or unknown account falls through
  to the next.
- **Master** merges the server lists of every checked backend, in order. Local
  game servers' heartbeats are forwarded to TribesNext when it is checked.
- **IRC** serves a player's own data (buddies, T-Mail, history, memberships) from
  the backend that logged them in, or the next checked backend that can serve
  them: TribesNext community data needs a TribesNext login, native serves anyone.
  Player and tribe searches merge every checked backend, and a name TribesNext
  does not know opens its existing native profile. Chat goes to the first
  checked backend that offers it. With a chat server entered in TribesNext's IRC
  cell, wonemu connects each player to that IRC network under their account
  name (as TribesNext's own client fix does), relays chat both ways, and keeps
  DBProxy and the WON handshake local. If the chat server cannot be reached or
  drops, the player gets a notice and chat falls back to wonemu's own. The
  address is empty by default: the server TribesNext's published client fix
  used, `irc.arloria.net:6667`, no longer resolves, so enter the current one to
  turn the relay on.

On the command line, `--backends` (or `T2LAUNCH_WON_BACKENDS`) takes the list as
JSON, for example
`[{"kind":"tribesnext","auth":true,"master":true,"irc":"chat.example.net:6667"},{"kind":"native","auth":true,"master":true,"irc":true}]`
(`"irc": true` serves TribesNext community data without relaying chat).
The older `--master-sources kind=address,...` and `--master-backend` options set
only the master column. New backends are added as new kinds in `BACKEND_KINDS`
(mirrored in `t2options.py`).

The directory serves `/TitanServers/Auth`, `/Tribes2`, and
`/TitanServers/DBProxy`, advertising the configured address (the listen
address, or this machine's outward address when listening on `0.0.0.0`). Directory discovery has
been verified against the actual #25034 client. The auth listener serves a
generated auth key block signed with a per-install emulator root key, which the
client verifies against an in-memory replacement for its embedded WON verifier.
Emulator accounts are stored separately in `community.sqlite3` as salted PBKDF2
password hashes; they do not depend on Sierra/WON signing keys or `private.store`.
Reserve a login with `python3 wonemu.py --reserve-local-account NAME`, then enter
that name and its chosen password in T2's online login screen. The first login
enrolls the password hash. Existing `private.store` accounts remain accepted as
a legacy fallback.
The default `compat` auth key profile uses a compact safe-prime group to avoid a
legacy-client overflow/truncation in its 2048-bit ElGamal output. Message 3 is
decoded through the ElGamal wrapper and Blowfish login-data transform. The
decrypted payload is represented in logs only by its length and SHA-256; key and
credential contents are not logged. A valid local account completes the
encrypted message-4/message-5 challenge and receives a signed Auth1/Auth2
certificate reply with an encrypted session-key confirmation. The #25034 client
accepts the local success reply. Unknown accounts and invalid passwords receive
a native AuthComplete failure. Use `--auth-key-profile native` to experiment
with the client's embedded 2048-bit group. The legacy private-store fallback
defaults to `base/private.store` beside the game binary and can be overridden
with `--private-store`. DBProxy account-name lookup accepts both local accounts
and legacy `private.store` entries; unsupported DBProxy
requests are recorded by operation and payload hash. Successful Auth1 sessions are
cached in memory for the IRC listener. Its native `CERT`/`CHALLENGE`/`CHALRESP`
exchange encrypts CB33/CB35 with the client's Auth1 key and includes the signed Auth2
certificate context. It validates the known CB34 proof encoding and has a
loopback-only compatibility path for legacy CB34 layouts bound to a verified
Auth1 session. The IRC service supports
the login handshake, PING/PONG, basic channel joins, player profile response
(query 23), and player search from `private.store` (query 3). It reassembles chunked
`DBQA` requests and emits `DBMSG` continuations for long results. Other DBProxy
support includes tribe search (query 4), local mail send/inbox polling/read/delete and deleted-mail
listing (queries 5, 1, 7, 6, and array query 14), forum list/topics/posts and local
post create/reply/edit/delete (queries 7–9 and 12–14), tribal topic moderation
(queries 62 and 66–68), news article
listing/post/edit/delete (array queries 0 and 100; scalar queries 1–3), local MOTD
and classic web-link rows (scalar query 0 and array query 15), local profile history
(array query 12), and online presence for authenticated IRC sessions (query 69).
Player descriptions, graphics, URLs, and display names are persisted in a local
SQLite community store at `community.sqlite3` under the log directory; override
its path with `--community-db`. Tribes and memberships are persisted there as
well. The current DBProxy subset includes player search/profile and profile
updates, live IRC presence, tribe search/profile/roster, tribe creation and settings,
membership roles, primary-tribe selection, invitations, and persistent buddy lists.
For sessions authenticated with an installed TribesNext account, player/clan
search, player/clan profiles, clan rosters, profile membership reads, player history
(`uhist`), the buddy and block lists (`robot_mail.php` `viewlist`/`addlist`/`dellist`), and
T-Mail (`box`/`read`/`send`/`delete`) use the TribesNext robot API. The stock client caches
whole messages, so each inbox poll reads the bodies of up to 25 new messages, which marks them
read on TribesNext. TribesNext delivers clan invitations as `invite` T-Mail with a JSON body;
these are rendered with the stock accept/reject links, which answer through `uaccept`/`ureject`.
TribesNext drops mail sent to yourself. TribesNext has no player picture, so a chosen picture is kept in the
community store by certificate GUID (visible only on this machine). The
unlocked account key is retained only in session memory. Local community data
is used only for local-authenticated sessions. TribesNext-authenticated sessions
do not fall back to local community records when a robot lookup is unavailable;
unsupported local-only community operations are rejected for those sessions.
Mail supports sending
to local accounts, inbox polling, and read/delete status. Forums provide a local
General board with topic and post persistence; post mutations are limited to the
authenticated author, while tribe moderators can manage their forum topics. News
articles use the recovered T2 category IDs and are editable/deletable by their author.
The stock #25034 online shell has no Forums tab; its forum backend is retained for
existing forum-link flows, without modifying the game's scripts or other assets.
Deferred idea: expose a Forums tab through hook-based runtime script registration,
while leaving the on-disk `.cs` files untouched. This is not implemented yet.
Mail sender block lists support listing,
adding, and removing blocked accounts and prevent delivery from blocked senders.
Deleted-mail views are supported. Existing tribe memberships grant access to the
corresponding negative-ID tribe forums. MOTD administration and other DBProxy
query families remain unsupported.

The master proxy handles native Torque game-type queries, server-list filters,
game-server heartbeats, and server-info replies. Its default `hybrid` backend
translates TribesNext `/list`, `/listtypes`, and heartbeat requests while merging
in locally observed native servers. Select `local` for LAN/alternative-auth
discovery or `tribesnext` for TribesNext-only listings with
`--master-backend`; `--tribesnext-master-url` can point to another compatible
HTTP backend. These can also be selected through `T2LAUNCH_MASTER_BACKEND` and
`T2LAUNCH_TRIBESNEXT_MASTER_URL` when the launcher auto-starts the service. The
existing T2 scripts remain untouched.

For command-line launches:

```bash
T2LAUNCH_WONEMU=1 ./runtime/run-game tribes2 ~/t2-linux
```

The directory is contacted when the **native WON login flow** runs. `-nologin`
skips that flow, and TribesNext replacement scripts may bypass it. A diagnostic
probe in the game console can explicitly start native discovery:

```cs
WONInit();
WONStartLogin("wonemu_probe", "probe_not_a_real_password", false);
echo(WONLoginResult());
```

Logs are in `${XDG_STATE_HOME:-~/.local/state}/t2launch/wonemu/`:
`traffic.jsonl` records decoded directory queries, authentication message
metadata, raw encrypted bytes, response hashes and connections; `wonemu.log`
captures stdout/stderr when auto-started. The generated private root/auth keys
live in `keys.json` in this private-permission state directory. The service
remains available across game instances. Start it interactively or stop it with:

```bash
python3 wonemu.py
python3 wonemu.py --stop
```

`--empty-directory` allows zero-entity response experiments; individual listener
ports can be configured with `--auth-port`, `--dbproxy-port`, etc. The hook's
bootstrap destination remains fixed at 15104. Auto-start uses the default ports
and verifies the running service's identity/settings, rather than reusing an
arbitrary process on that port.

Protocol checks: `python3 -m unittest discover -s tests -v`.

### Running the game

```bash
./runtime/run-game tribes2 /path/to/your/t2-linux -online
```

The game runs on the host's 32-bit libraries plus the bundled Loki-era
libraries in `runtime/compat-libs`. Neither the game binary nor any game file
is modified. The required packages are listed under *Install* above.

The first launch compiles `t2_hook.so`, `rsa_helper`, and `t2_realign.so`
into `games/tribes2/` with `gcc -m32` (`./runtime/build`), and rebuilds them
when their sources change. The launcher checks that every library the game
links resolves before starting it. It uses the hardware GL driver and redraws
the whole GUI canvas each frame (see *Runtime compatibility* below); set
`T2LAUNCH_SOFTWARE_GL=1` for software GL, or `T2LAUNCH_FULL_REDRAW=0` to turn
the redraw off. `T2LAUNCH_TRACE_CONNECTS=1` records resolver names and, when
`strace` is installed, `connect()` calls under
`~/.local/state/t2launch/diagnostics`.

The game renders text from the font caches in `base.vl2` and, for other sizes,
from the TrueType files in your install's `base/fonts/` (the Loki release ships
Arial and Verdana there). It aborts if Arial is missing, so keep that folder
intact; no font files come from this repository.

### Verify Hook Loading

```bash
./runtime/run-game tribes2 /path/to/your/t2-linux -nologin 2>&1 | grep "Registered"
```

### Test in-game

Add to `~/t2-linux/base/autoexec.cs`:
```cs
%result = hook_sha1("abc");
echo("SHA1: " @ %result);
// Expected: a9993e364706816aba3e25717850c26c9cd0d89d
```

## Current Status

- [x] Game boots to lobby in `-nologin` mode (158 DSOs compiled)
- [x] LD_PRELOAD hook mechanism works
- [x] Hook registers console commands via `Con::addMethodS`
- [x] `hook_test()` — basic command test (returns "ok_from_c")
- [x] `hook_sha1()` — SHA1 hash (verified: correct for "abc")
- [x] `hook_rsa_mod_exp()` — RSA modular exponentiation (verified: 2^10 mod 101 = 0e)
- [x] `hook_rand64()` — unpadded random 64-bit hex from `/dev/urandom`
- [x] Runtime T2CSRI client handlers, installed by the native hook without changing game scripts or archives
- [x] Existing `public.store` / `private.store` unlock in memory with password nonce verification
- [x] TribesNext certificate chunk transfer and server challenge-response handlers
- [x] End-to-end TribesNext authentication and server join verified in a live game session
- [x] Live TribesNext login, tribe search, and roster viewing verified in-game
- [x] Account creation via the TribesNext `NAME`/`SIGN` protocol with GMP-backed RSA key generation

## TribesNext Runtime Integration

The hook monitors the existing login fields in memory, uses the entered
username/password to unlock the installed private-store key, and clears the
temporary C password buffer after verification. It sends the existing certificate unchanged
when a TribesNext server requests it. The issuer signature is left to the game
server because the production AuthServer trust key is not present in the
published source. Challenge RSA input is passed to `rsa_helper` over a pipe,
not on the helper process command line.

## Hook Console Commands

| Command | Args | Description |
|---------|------|-------------|
| `hook_test` | none | Returns "ok_from_c" |
| `hook_sha1` | `<string>` | SHA1 hash → 40-char hex |
| `hook_rsa_mod_exp` | `<base_hex> <exp_hex> <mod_hex>` | RSA modular exponentiation |
| `hook_rand64` | none | Random unpadded 64-bit hex |
| `hook_profile_browser_wake` | none | Confirm runtime browser integration |
| `hook_tn_unlock` | `<username> <password>` | Unlock existing key in memory; no password logging |
| `hook_tn_certificate_length` | none | Get installed certificate length |
| `hook_tn_certificate_chunk` | `<offset>` | Return one certificate chunk |
| `hook_tn_decrypt` | `<hex>` | Decrypt server challenge using the unlocked private key |
| `hook_tn_clear` | none | Clear in-memory account material |
| `hook_tn_trace` | `<known_stage> [safe_state...]` | Safe diagnostic marker for auth stages |
| `hook_tn_join_trace` | `<address> <ready>` | Log selected server endpoint and key-ready state |
| `hook_tn_ip_trace` | `<ip> <hex>` | Log the parsed IPv4 used in the client challenge |
| `hook_tn_ipv4_hex` | `<dotted_ipv4>` | Encode the T2CSRI IPv4 challenge suffix |

## Technical Findings

### Hook Registration Timing

The constructor installs the local verifier when WON emulation is enabled, but
defers console registration and TorqueScript changes until the SDL render loop is
running. Hook commands are registered on the game's main thread after the console
table is available, avoiding concurrent mutation while startup scripts load.

### LD_PRELOAD Configuration

The launcher composes `LD_PRELOAD` and `LD_AUDIT` in `runtime/run-game` and sets
them on the game process only. The hook must precede SDL so its main-thread
`SDL_GL_SwapBuffers` interposer can defer runtime TorqueScript work safely:
```
LD_AUDIT=games/tribes2/t2_realign.so
LD_PRELOAD=games/tribes2/t2_hook.so:/usr/lib32/pulseaudio/libpulsedsp.so:runtime/compat-libs/libSDL-1.2.so.0
```

The hook toggle removes only `t2_hook.so`. It registers fifteen native commands
and installs the Player TRIBES-tab override at runtime after the Community
Browser control exists.

### Runtime compatibility

The project first ran the game in a 32-bit Ubuntu 16.04 Docker image. That image
supplied nothing a current multilib host cannot, but four behaviors of the
game need handling on current 32-bit libraries:

- **Stack alignment.** Tribes 2 and its Loki libraries keep the 4-byte stack
  alignment of 2001 compilers; current 32-bit libraries assume 16 bytes and
  use SSE stores such as `movaps` on stack slots. The first crash is in
  libX11's `_XSend`, reached from the bundled SDL's `XSync`. `t2_realign.so`
  is an `LD_AUDIT` module: when an old object binds a function in a modern
  system library (including through `dlsym` and `glXGetProcAddress`), it
  returns a thunk that realigns the stack and copies sixteen argument words.
  glibc's core libraries are excluded so `setjmp` and friends never run in an
  extra frame. The native hook and helpers are built with `-mstackrealign`.
- **Joining detached threads.** At shutdown and while loading missions the
  client joins threads it created detached. LinuxThreads returned `EINVAL`;
  NPTL reads the freed thread descriptor. The hook interposes
  `pthread_create`, `pthread_detach`, and `pthread_join`, and refuses joins on
  threads it did not record as joinable.
- **Back-buffer contents.** Torque's GUI redraws only dirty regions and assumes
  the back buffer still holds the previous frame. Current hardware drivers do
  not preserve it, which shows as flicker and ghost cursors. The hook calls
  `Canvas.reset()` after every swap
  (`T2LAUNCH_FULL_REDRAW=1`, the launcher default), marking the whole canvas
  dirty so each frame is drawn in full.

- **Volume controls.** Torque plays interface sounds, the shell hum, and music
  as `AL_SOURCE_RELATIVE` sources and sets their gain with Loki's linear
  `AL_GAIN_LINEAR_LOKI`. The bundled `libopenal.so.0` applies source gain in its
  distance-attenuation filter, which returns early for relative sources, so
  every volume slider was ignored. `t2_realign.so` tracks relative sources and
  also sets `AL_MAX_GAIN`, which Loki's min/max filter still applies, to the
  value that yields the requested linear gain (Loki maps it non-linearly; the
  table was measured from its output). `T2LAUNCH_OPENAL_VOLUME_FIX=0` turns this
  off and `T2LAUNCH_REALIGN_TRACE_AL=1` logs each gain call.

`MESA_EXTENSION_MAX_YEAR=2000` and `__GL_ExtensionStringVersion=17700` keep
the GL extension string short enough for the client's fixed buffer, and `padsp`
(`lib32-libpulse`) provides the OSS `/dev/dsp` device that Loki OpenAL opens.
The hook reads `base/*.store` from `T2LAUNCH_GAME_DIR` (default: the startup
working directory) and runs the RSA helper from `T2LAUNCH_RSA_HELPER`, so
nothing is copied into the game folder.

### Key Binary Addresses (Tribes 2 Linux #25034)

| Function | Address | Convention |
|----------|---------|------------|
| `Con::addMethodS` | `0x080E85C0` | `__cdecl` |
| `Con::addMethodV` | `0x080E8618` | `__cdecl` |
| `Con::addMethodI` | `0x080E8670` | `__cdecl` |
| `Con::addMethodB` | `0x080E8720` | `__cdecl` |
| `Con::evaluate` | `0x080E88F4` | `__cdecl` |
| `Con::execute` | `0x080E7560` | `__cdecl` |
| `Con::getReturnBuffer` | IFC22:0x08122490 | `__cdecl` |
| Console global ptr | `0x08493DEC` | — |
| `Sim::postEvent` | `0x080FE128` | `__cdecl` |
| `WONGetAuthInfo` | `0x0834E804` | console callback |
| AuthInfo parser | `0x08348FCC` | `__cdecl` |
| `Auth2Certificate::UnpackData` | `0x083615A4` | `__thiscall` |

### AuthInfo (`WONGetAuthInfo`)

The launcher's "Failed to get account information" check reads `WONGetAuthInfo()`.
The client builds that text from the current Auth2 certificate (global
`0x086C0434`). It does not use the identity data (type 0). It decodes the first
community-data entry (Auth2 data type 1: `u32 communityId, u16 len, blob`), whose
blob is little-endian:

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

### Game Console Settings

To see game output in the terminal, add these to `base/autoexec.cs`:
```cs
$Con::logBufferEnabled = false;
$pref::NoClearConsole = true;
$winConsoleEnabled = true;
enablewinconsole(true);
```

### Script Compiler Notes

The TorqueScript compiler crashes at `0x80f1467` on syntax errors (not large files). Use `-prepbuild -nologin` to identify which file has errors. Unknown functions (like `setLogMode`, `DNetSetLogging`) cause compile-time crashes — only use functions that exist in the base game.

### Cached DSO Files

The game compiles `.cs` files to `.dso` bytecode and caches them in `~/.loki/tribes2/base/`. After modifying scripts, delete cached files:
```bash
find ~/.loki/tribes2 -name "*.dso" -delete
rm -f ~/.loki/tribes2/console.log ~/.loki/tribes2/stack-trace.txt
```

CRITICAL: Also `pkill -9 -f tribes2.dynamic` before testing — stale processes cause stale output.

### Game Config

The default config (`runtime/tribes2.cfg`) sets the game binary and
enables the hook. Tribes 2 runs windowed at its selected resolution, without
Gamescope, and uses hardware OpenGL by default. An explicit `-f`/`--fullscreen`
argument or `FULLSCREEN=true` requests fullscreen. An X11 watcher keeps the
window aligned with the selected monitor and scales fullscreen output while
preserving the game's aspect ratio; in windowed mode it follows the saved game
resolution when SDL 1.2 leaves the top-level window at its previous size. The
desktop profile supplies `-online` or `-nologin`:

SDL 1.2's windowed `SDL_ListModes` may report “any size”; Tribes 2 then falls
back to a short built-in 4:3 list. For non-Gamescope runs the launcher enables
SDL XRandR enumeration, and the hook substitutes SDL's actual fullscreen host
mode list only for that windowed sentinel. The list is taken from the host
display, not synthesized or tied to a GPU vendor.

```
USE_GAMESCOPE=0
SOFTWARE_GL=0
USE_PADSP=1
GAME_BINARY="./tribes2.dynamic"
LOAD_HOOK=1
```

The bundled Loki OpenAL backend uses OSS `/dev/dsp`; `USE_PADSP=1` keeps its
provider available and routes playback through PulseAudio/PipeWire. To
select a specific host output for the game, set `T2LAUNCH_PULSE_SINK` to a sink
name reported by `pactl list short sinks`. When unset, the game follows the
host's current PulseAudio/PipeWire default output.

## Credits

- **TribesNext** (tribesnext.com) — community authentication system
- **t2-mapper** (exogen/t2-mapper) — protocol and crypto reference implementation
- **IFC22** — binary address reference and API documentation
- **Loki Software** — Linux port of Tribes 2
- **tribes-2-docker** — the original Docker launcher this project started from

## License

This project is for educational and preservation purposes. Tribes 2 is copyrighted by Dynamix/Interplay.
