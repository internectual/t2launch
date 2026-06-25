# t2launch

Tribes 2 Linux Online Launcher — get Tribes 2 online play working on Linux without Wine.

## Goal

Run the Tribes 2 Linux client (Loki native port, 2001) and connect to the TribesNext online multiplayer servers, without using Wine or any Windows compatibility layer.

## Background

Tribes 2 used GameSpy for online play, which shut down in 2004. The TribesNext community patch replaced GameSpy with a custom authentication system that uses RSA challenge-response for login. The TribesNext RC2a patch was only released as a Windows `.exe` modifier, which means Linux users had to run the entire game under Wine.

This project replaces the Ruby-based crypto layer (which the Windows patch depends on) with native TorqueScript implementations, allowing the Loki Linux binary to authenticate directly with TribesNext servers.

## Architecture

```
t2launch/
├── console_start.cs          # Patched startup (loads native crypto before VL2)
├── t2csri_native.cs           # Native TorqueScript crypto (SHA1, RSA, RC4)
├── base/t2csri_rc3.vl2        # Patched TribesNext scripts (native login flow)
├── public.store               # Your TribesNext certificate
├── private.store              # Your encrypted private key
├── tribes2.dynamic            # Loki Linux binary (not included, use your own)
├── docker/                    # Docker compatibility wrapper
│   ├── Dockerfile             # 32-bit Ubuntu 16.04 container
│   ├── asgard-run             # Launch script
│   └── asgard-build           # Image builder
└── run_t2.sh                  # Quick start script
```

## How It Works

1. **Native SHA1**: Implements SHA1 hash in TorqueScript (32-bit word operations)
2. **Native RSA**: Implements modular exponentiation for big numbers using 32-bit chunk arrays
3. **Native RC4**: Implements RC4 stream cipher with 2048-byte discard
4. **Pre-decrypted Key**: The private key is decrypted at build time using Python (fast), avoiding the slow RC4-in-TorqueScript path at runtime
5. **Patched Login Flow**: `t2csri_doLogin()` bypasses Ruby's `rubyEval()` entirely

## Quick Start

### Prerequisites

- Docker installed and running
- Your own Tribes 2 Linux game files (from Loki Software release)
- TribesNext account credentials

### Setup

1. **Get your credentials** using the t2-mapper login script:
   ```bash
   git clone https://github.com/exogen/t2-mapper.git
   cd t2-mapper
   npm install
   npm run login
   ```
   Copy the `T2_ACCOUNT_CERTIFICATE` and `T2_ACCOUNT_ENCRYPTED_KEY` values.

2. **Generate store files**:
   ```bash
   python3 scripts/gen_store.py --cert "$CERT" --key "$KEY" --username YourUsername
   ```

3. **Decrypt the private key** (for the hardcoded key in `loginScreens.cs`):
   ```bash
   python3 scripts/decrypt_key.py --key "$KEY" --password YourPassword
   ```

4. **Update the hardcoded values** in `base/t2csri_rc3.vl2` (via extract → modify → repack)

### Run

```bash
./docker/asgard-run tribes2 /path/to/your/t2-linux -online
```

Or with the quick script:
```bash
./run_t2.sh
```

## Current Status

- [x] `$RubyEnabled` check bypassed
- [x] Account credentials loaded from store files
- [x] Login flow bypasses server credential download
- [x] SHA1 implemented natively in TorqueScript
- [x] RC4 key derivation uses hex-string key (matching TribesNext behavior)
- [x] Private key pre-decrypted at build time
- [x] LD_PRELOAD hook loads successfully (t2_hook.so)
- [x] Hook can read game memory (mConsoleConstructor at 0x8487434)
- [x] Hook finds Con::printf at 0x080E61E4 via dlsym
- [ ] Con::printf callable from hook (engine not ready during constructor)
- [ ] Console command registration from hook
- [ ] RSA key setup completes but game hangs at post-login (main menu loading)
- [ ] Full online play tested

## Technical Notes

### The RC4 Key Format Issue

The TribesNext Ruby code derives the RC4 key as:
```ruby
rc4_key = SHA1(password + nonce)  # Returns hex string
# Then uses the 40-CHAR HEX STRING as the RC4 key, not the 20-byte binary hash
```

This is critical — using the 20-byte binary hash instead of the 40-byte hex string produces garbage output. The t2-mapper `auth.ts` code documents this:
> "RC4 key = SHA1(password + nonce) as ASCII hex string (not hex-decoded)"

### LD_PRELOAD Hook (t2_hook.so)

The hook is loaded via `t2_hook.so` listed in the binary's `NEEDED` entries. It uses `__attribute__((constructor))` to run during static initialization.

**What works:**
- Reading game memory at known addresses
- Calling `dlsym` to find engine functions
- Writing to stdout via `write()` syscall

**Limitations:**
- `Con::printf` (at 0x080E61E4) cannot be called from the constructor — the console system isn't initialized yet
- `mConsoleConstructor` (at 0x8487434) linked list isn't built during static init
- `Con::addCommand` requires `__thiscall` with the correct `this` pointer

**Key addresses (from stripped binary):**
- `Con::printf`: `0x080E61E4`
- `mConsoleConstructor`: `0x08487434`
- `Con::addCommand_4`: `0x080E74B0`
- `Con::addCommand_5`: `0x080E7560`
- `Con::addCommand_6`: `0x080E7610`
- `Con::addCommand`: `0x080E774C`

### TorqueScript Compiler Stack Overflow

The TorqueScript compiler in the V12 engine has a stack overflow bug for `.cs` files >100 lines. The original `t2csri_native.cs` (708 lines) was split into multiple smaller files to work around this.

### Cached DSO Files

The game compiles `.cs` files to `.dso` bytecode and caches them in `~/.loki/tribes2/base/`. After modifying scripts, delete the cached `.dso` files:
```bash
find ~/.loki/tribes2 -name "*.dso" -delete
```

### VL2 Archive Modification

The game's scripts are packed in `.vl2` archives (which are just ZIP files). To modify them:
```bash
cd /tmp && mkdir vl2_extract && cd vl2_extract
unzip /path/to/base/t2csri_rc3.vl2
# Edit files...
python3 -c "
import zipfile
with zipfile.ZipFile('/path/to/base/t2csri_rc3.vl2', 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk('.'):
        for f in files:
            zf.write(os.path.join(root, f))
"
```

## Future Plans

- [ ] Call engine functions from hook (need to defer past static init)
- [ ] Register console commands from hook (need mConsoleConstructor ready)
- [ ] Move crypto to native C in the hook for performance
- [ ] Implement full TribesNext protocol in a standalone proxy (no TorqueScript modifications needed)
- [ ] Support for TribesNext clans, browser, and community features
- [ ] Automated credential refresh (currently credentials expire)
- [ ] GUI launcher for server browsing and connection
- [ ] Integration with TacoServer for custom game servers

## Credits

- **TribesNext** (tribesnext.com) — community authentication system
- **t2-mapper** (exogen/t2-mapper) — protocol reference implementation
- **tribes-2-docker** (CodeMasterCody3D) — Docker compatibility wrapper
- **Loki Software** — Linux port of Tribes 2

## License

This project is for educational and preservation purposes. Tribes 2 is copyrighted by Dynamix/Interplay.
