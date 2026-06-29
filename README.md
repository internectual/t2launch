# t2launch

Tribes 2 Linux Online Launcher — get Tribes 2 online play working on Linux without Wine.

## Goal

Run the Tribes 2 Linux client (Loki native port, 2001) and connect to the TribesNext online multiplayer servers, without using Wine or any Windows compatibility layer.

## Background

Tribes 2 used GameSpy for online play, which shut down in 2004. The TribesNext community patch replaced GameSpy with a custom authentication system that uses RSA challenge-response for login. The TribesNext RC2a patch was only released as a Windows `.exe` modifier, which means Linux users had to run the entire game under Wine.

This project replaces the Ruby-based crypto layer (which the Windows patch depends on) with native implementations, allowing the Loki Linux binary to authenticate directly with TribesNext servers.

## Architecture

```
t2launch/
├── docker/                    # Docker compatibility wrapper
│   ├── Dockerfile             # 32-bit Ubuntu 16.04 container
│   ├── asgard-run             # Launch script
│   ├── config/tribes2.cfg     # Per-game config (LD_PRELOAD, etc.)
│   └── run_t2.sh              # In-container launcher
├── t2csri_native.cs           # Native TorqueScript crypto (SHA1, RSA, RC4)
└── README.md
```

Game files (not included) go in `~/t2-linux/`:
```
~/t2-linux/
├── tribes2.dynamic            # Loki Linux binary (#25034)
├── t2_hook.so                 # LD_PRELOAD hook (crypto commands)
├── console_start.cs           # Game startup (loads t2csri_native splits)
├── autoexec.cs / base/        # Game scripts
├── *.vl2                      # Script archives
└── public.store/private.store # TribesNext credentials
```

## How It Works

1. **LD_PRELOAD Hook**: `t2_hook.so` is loaded at game startup, registers console commands (`sha1sum`, etc.) via `Con::addCommand`
2. **Native TorqueScript Crypto**: `t2csri_native.cs` split into <100-line chunks provides `rubyEval` stub and auth flow
3. **Package Wrapping**: `t2launch_package.cs` wraps `DispatchLaunchMode` to detect engine readiness
4. **Credentials**: Store files (`public.store`, `private.store`) provide TribesNext certificate/key

## Quick Start

### Prerequisites

- Docker installed and running
- Your own Tribes 2 Linux game files (from Loki Software release)
- TribesNext account credentials

### Run

```bash
./docker/asgard-run tribes2 /path/to/your/t2-linux -nologin
```

Or to attempt online play:
```bash
./docker/asgard-run tribes2 /path/to/your/t2-linux -online
```

### Verify Hook Loading

```bash
./docker/asgard-run tribes2 /path/to/your/t2-linux -nologin 2>&1 | grep HOOK_LOADED
```

## Current Status

- [x] Game boots to lobby in `-nologin` mode (159 DSOs compiled)
- [x] Original game binary works via Docker (no hook needed for basic boot)
- [x] t2_hook.so builds successfully (pure C, no C++ dependencies)
- [x] LD_PRELOAD mechanism works via `config/tribes2.cfg` EXTRA_ENV
- [x] Hook constructor runs (confirmed with stderr output)
- [x] t2csri_native.cs split into 10 files compiles without errors
- [x] Console commands from hook (sha1sum, rsa_mod_exp) — IN PROGRESS
- [ ] Game server connection (RSA challenge-response)
- [ ] Full online play tested

## Technical Findings

### What Does NOT Cause Compiler Crashes

Large `.cs` files are **NOT** the problem. The original game has a 2592-line `console_start.cs` and compiles fine. The TorqueScript compiler crash at `0x80f1467` is caused by **SYNTAX ERRORS** (mismatched braces, bad split points in scripts).

Use `-prepbuild -nologin` to identify which file has syntax errors — the compiler will show `Line N - Syntax error.` for each failing file.

### LD_PRELOAD Configuration

The hook is loaded via `EXTRA_ENV` in `docker/config/tribes2.cfg`:
```bash
EXTRA_ENV="LD_PRELOAD=/usr/lib/libSDL-1.2.so.0 /home/loki/game/t2_hook.so"
```

This is the **only** reliable way to set LD_PRELOAD — setting it via `docker run -e` gets overridden by the `EXTRA_ENV` variable.

### Hook Requirements

The hook MUST be compiled as pure C (not C++) to avoid symbol conflicts:
```bash
gcc -m32 -shared -fPIC -o t2_hook.so t2_hook.c -lc -O2 -std=c99
```

Using C++ or linking against `libstdc++` causes `undefined symbol: __ti9exception` crashes.

### t2csri_native.cs Splitting

The 708-line `t2csri_native.cs` must be split into 78-line chunks to avoid hitting syntax errors at split points:
```
t2csri_native_0.cs through t2csri_native_9.cs
```

Update `console_start.cs` to exec all splits:
```
exec("t2csri_native_0.cs");
exec("t2csri_native_1.cs");
...
exec("t2csri_native_9.cs");
```

### Key Binary Addresses (from Tribes 2 Linux #25034)

- `Con::addCommand`: `0x080E85C0` (same function at 0x80E85C0, 0x80E8618, 0x80E8670, 0x80E86C8)
- `telnetSetParameters` command: registered at game startup (for remote debugging)
- Game binary MD5: `14d168effa1d854f95b18d959c9fd009`

### Cached DSO Files

The game compiles `.cs` files to `.dso` bytecode and caches them in `~/.loki/tribes2/base/`. After modifying scripts, delete cached `.dso` files:
```bash
find ~/.loki/tribes2 -name "*.dso" -delete
```

### Docker Game Directory Mapping

The game directory is bind-mounted: `~/t2-linux` → `/home/loki/game` inside the container. Hook and script files are accessible at `/home/loki/game/`.

## Known Issues

1. **Login GUI missing**: `LoginDlg` control not loading — need to exec `gui/LoginDlg.gui` from autoexec.cs or console_start.cs
2. **t2csri_native_4.cs duplicated**: `console_start.cs` has duplicate exec lines causing double-load
3. **Hook file locked**: Cannot overwrite `t2_hook.so` while game is running (even zombie processes hold the lock)
4. **Process accumulation**: Multiple game processes accumulate if not killed properly — always `pkill -9 -f tribes2` before testing

## Todo

- [ ] Fix LoginDlg loading (add exec to autoexec.cs)
- [ ] Get hook to register sha1sum/sha1sum commands via Con::addMethodS
- [ ) Implement RSA decrypt in hook (big number modular exponentiation)
- [ ] Test full auth flow: login → lobby → game server
- [ ] Clean up duplicate t2csri_native exec lines in console_start.cs

## Credits

- **TribesNext** (tribesnext.com) — community authentication system
- **t2-mapper** (exogen/t2-mapper) — protocol reference implementation
- **Loki Software** — Linux port of Tribes 2

## License

This project is for educational and preservation purposes. Tribes 2 is copyrighted by Dynamix/Interplay.
