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
├── docker/                    # Docker compatibility wrapper
│   ├── Dockerfile             # 32-bit Ubuntu 16.04 container
│   ├── asgard-run             # Launch script
│   ├── config/tribes2.cfg     # Per-game config (LD_PRELOAD, etc.)
│   ├── asgard-build           # Build image and compile hook
│   ├── alsa-pulse.conf        # Route ALSA audio through PulseAudio/PipeWire
│   ├── window_resize.sh       # Follow game mode and selected display
│   └── compat-libs/           # Loki-era compatibility libraries
├── games/tribes2/             # Generated hook/helper overlays
├── t2_hook.c                  # LD_PRELOAD hook and runtime game patches
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

For the #25034 profile browser, the hook also replaces the local player's broken
TRIBES-tab callback at runtime with a `DatabaseQueryArray(13, ...)` request. This
prefetches tribe memberships for the first browser wake, adds those tribes to the
browser tabs and membership checks, and repaints the member list when rows arrive.
It also falls back to the saved login name for an empty AuthInfo display name and
disables IRC nickname filtering on the tribe creation fields. These are in-memory
GUI/script changes; no `.cs` files or archives on disk are modified.

## Quick Start

### Desktop launcher

T2Launch provides a system-tray desktop app for running the native client with
saved profiles. It builds and runs the Docker image from this checkout; no files
are downloaded from the original `tribes-2-docker` project.

Install the desktop app and start it:

```bash
./install.sh
~/.local/bin/t2launch
```

Open **Settings** from the tray menu to choose the game directory, create/edit
profiles, choose the fullscreen display, toggle the hook, configure autolaunch,
or use **Build / Rebuild image**. The tray menu lists profiles alphabetically;
click a profile to launch it. Hovering the tray icon shows running profiles.
The hook is enabled by default and its library/helper are overlaid into an
external game directory when available. Settings are stored in
`~/.config/t2launch/settings.json` with private permissions.
Launcher and crash-stage diagnostics are written, with login arguments redacted,
to `~/.local/state/t2launch/launch-diagnostics.log`.

The underlying command-line image build remains available:

```bash
./docker/asgard-build tribes2
```

The launcher automatically builds the image if it is missing. Its build
context stages the local `games/tribes2` support files, installs the image's
runtime dependencies, and includes the legacy Loki compatibility libraries in
`docker/compat-libs`; it does not fetch code or libraries from the upstream
project. It compiles `t2_hook.so` and `rsa_helper` from the tracked C sources
and exports both under `games/tribes2/` for overlaying onto an external game
directory.

### Local WON emulator and master proxy

Enable **Use local WON emulator** in a profile and leave **Load t2_hook.so**
enabled. The launcher starts or reuses `wonemu.py` and checks readiness before
starting the game. Readiness includes an implementation fingerprint, so an
existing daemon is restarted when the emulator code changes. It binds only to
localhost:

| Port | Handler |
|------|---------|
| TCP 15104 | WON GetDir requests and service discovery |
| TCP 15105 | Signed public-key block and local-account Auth1 login |
| TCP 15106 | DBProxy account-name lookup; other operations are captured |
| TCP 15107 | Native IRC certificate/challenge login and basic IRC commands |
| UDP 15108 | Native Torque master protocol and TribesNext discovery proxy |

The hook replaces the three Sierra bootstrap strings with `127.0.0.1:15104`
in the running client's memory before WON initialization. It checks the original
strings and restores page permissions afterward. The executable on disk is
unchanged. The option is off by default; enabling it on an unsupported executable
fails instead of attempting the patch.

The directory serves `/TitanServers/Auth`, `/Tribes2`, and
`/TitanServers/DBProxy`, advertising the local endpoints. Directory discovery has
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
T2LAUNCH_WONEMU=1 ./docker/asgard-run tribes2 ~/t2-linux
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

### Prerequisites

- Docker installed and running
- Your own Tribes 2 Linux game files (from Loki Software release)
- TribesNext account credentials

### Build the image and hook

```bash
./docker/asgard-build tribes2
```

### Run

```bash
./docker/asgard-run tribes2 /path/to/your/t2-linux -online
```

### Verify Hook Loading

```bash
./docker/asgard-run tribes2 /path/to/your/t2-linux -nologin 2>&1 | grep "Registered"
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
- [ ] Account creation (needs RSA keygen in C)

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
| `hook_tn_trace` | `<known_stage>` | Safe diagnostic marker for T2CSRI join stages |
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

The launcher composes `LD_PRELOAD` in `docker/asgard-run` when the hook option
is enabled. The hook must precede SDL so its main-thread
`SDL_GL_SwapBuffers` interposer can defer runtime TorqueScript work safely. The
Xenial container also needs `libpthread` preloaded explicitly:
```
LD_PRELOAD=/home/loki/game/t2_hook.so:/usr/lib/i386-linux-gnu/pulseaudio/libpulsedsp.so:/lib/i386-linux-gnu/libpthread.so.0:/usr/lib/libSDL-1.2.so.0
```

The hook toggle removes only `t2_hook.so`; `libpthread` and SDL remain available
for the legacy game runtime. It registers fourteen native commands and installs the
Player TRIBES-tab override at runtime after the Community Browser control exists.

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

### Game Console Settings

To see game output in Docker, add these to `base/autoexec.cs`:
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

The default config (`docker/config/tribes2.cfg`) sets the game binary and
enables the hook. Tribes 2 runs windowed at its selected resolution, without
Gamescope, and uses hardware OpenGL by default. An explicit `-f`/`--fullscreen`
argument or `FULLSCREEN=true` requests fullscreen. An X11 watcher keeps the
window aligned with the selected monitor and scales fullscreen output while
preserving the game's aspect ratio; in windowed mode it follows the saved game
resolution when SDL 1.2 leaves the top-level window at its previous size. The
desktop profile supplies `-online` or `-nologin`:
```
USE_GAMESCOPE=0
SOFTWARE_GL=0
USE_STRACE=0
USE_PADSP=1
USE_PULSE_ALSA=1
GAME_BINARY="./tribes2.dynamic"
LOAD_HOOK=1
```

The bundled Loki OpenAL backend uses OSS `/dev/dsp`; `USE_PADSP=1` keeps its
provider available and routes playback through PulseAudio/PipeWire. The
`USE_PULSE_ALSA=1` overlay also routes ALSA clients through the same socket. To
select a specific host output for the game, set `T2LAUNCH_PULSE_SINK` to a sink
name reported by `pactl list short sinks`. When unset, the game follows the
host's current PulseAudio/PipeWire default output.

## Credits

- **TribesNext** (tribesnext.com) — community authentication system
- **t2-mapper** (exogen/t2-mapper) — protocol and crypto reference implementation
- **IFC22** — binary address reference and API documentation
- **Loki Software** — Linux port of Tribes 2

## License

This project is for educational and preservation purposes. Tribes 2 is copyrighted by Dynamix/Interplay.
