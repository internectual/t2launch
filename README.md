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
│   └── run_t2.sh              # In-container launcher
├── t2_hook.c                  # LD_PRELOAD hook (SHA1, RSA, console commands)
└── README.md
```

Game files (not included) go in `~/t2-linux/`:
```
~/t2-linux/
├── tribes2.dynamic            # Loki Linux binary (#25034)
├── t2_hook.so                 # Compiled hook (built from t2_hook.c)
├── console_start.cs           # Game startup script
├── autoexec.cs / base/        # Game scripts
└── *.vl2                      # Script archives
```

## How It Works

1. **LD_PRELOAD Hook**: `t2_hook.so` is loaded at game startup, spawns a thread that waits for the console hash table to initialize, then registers console commands via `Con::addMethodS`
2. **Native SHA1**: `hook_sha1()` — SHA1 hash computation in C, callable from TorqueScript
3. **Native RSA** (TODO): `hook_rsa_mod_exp()` — modular exponentiation for 2048-bit RSA
4. **Credentials** (TODO): Store files (`public.store`, `private.store`) for TribesNext certificate/key

## Quick Start

### Prerequisites

- Docker installed and running
- Your own Tribes 2 Linux game files (from Loki Software release)
- TribesNext account credentials

### Build the hook

```bash
docker run --rm -v /path/to/t2_hook.c:/build/t2_hook.c -v ~/t2-linux:/output tribes2:hook \
  bash -c 'cd /build; gcc -m32 -shared -fPIC -o t2_hook.so t2_hook.c -lc -O2 -std=c99'
```

### Run

```bash
./docker/asgard-run tribes2 /path/to/your/t2-linux -nologin
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
- [x] `hook_rand64()` — random 64-bit hex from /dev/urandom
- [ ] TribesNext script integration (official t2-scripts)
- [ ] Full auth flow: login → lobby → game server
- [ ] Full online play tested

## Hook Console Commands

| Command | Args | Description |
|---------|------|-------------|
| `hook_test` | none | Returns "ok_from_c" |
| `hook_sha1` | `<string>` | SHA1 hash → 40-char hex |
| `hook_rsa_mod_exp` | `<base_hex> <exp_hex> <mod_hex>` | RSA modular exponentiation |
| `hook_rand64` | none | Random 16-char hex |
- [ ] Full auth flow: login → lobby → game server connection
- [ ] Full online play tested

## Technical Findings

### Hook Registration Timing

The hook CANNOT register commands from its constructor — the console hash table isn't initialized yet. The working approach:
1. Constructor spawns a detached `pthread`
2. Thread polls `0x8493dec + 4` (console hash table size) every 500ms
3. When `table_size > 0`, calls `Con::addMethodS()` to register commands
4. Registration typically completes within 1-2 seconds of game startup

### LD_PRELOAD Configuration

The hook is loaded via `EXTRA_ENV` in `docker/config/tribes2.cfg`:
```bash
EXTRA_ENV="LD_PRELOAD=/usr/lib/libSDL-1.2.so.0 /home/loki/game/t2_hook.so"
```

This is the **only** reliable way to set LD_PRELOAD — setting it via `docker run -e` gets overridden by the `EXTRA_ENV` variable.

### Hook Build Requirements

The hook MUST be compiled as pure C (not C++):
```bash
gcc -m32 -shared -fPIC -o t2_hook.so t2_hook.c -lc -O2 -std=c99
```

Compiling in the `tribes2:hook` container ensures glibc 2.23 compatibility.

### Key Binary Addresses (Tribes 2 Linux #25034)

| Function | Address | Convention |
|----------|---------|------------|
| `Con::addMethodS` | `0x080E85C0` | `__cdecl` |
| `Con::addMethodV` | `0x080E8618` | `__cdecl` |
| `Con::addMethodI` | `0x080E8670` | `__cdecl` |
| `Con::addMethodB` | `0x080E8720` | `__cdecl` |
| `Con::evaluate` | `0x080E88F4` | `__cdecl` |
| `Con::execute` | `0x080E7560` | `__cdecl` |
| `Con::getVariable` | `0x080E7388` | `__cdecl` |
| Console global ptr | `0x08493DEC` | — |
| `Sim::findObject` | `0x080FE454` | `__cdecl` |
| `dAtoi` | `0x081CF68C` | `__cdecl` |
| `dAtob` | `0x081CF6E8` | `__cdecl` |

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

## Credits

- **TribesNext** (tribesnext.com) — community authentication system
- **t2-mapper** (exogen/t2-mapper) — protocol reference implementation
- **IFC22** — binary address reference and API documentation
- **Loki Software** — Linux port of Tribes 2

## License

This project is for educational and preservation purposes. Tribes 2 is copyrighted by Dynamix/Interplay.
