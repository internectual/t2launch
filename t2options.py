"""Per-profile launch options for the WON emulator, the hook, and the runtime.

This registry is the single source of truth: the launcher builds its WON and
Hook tabs from it, turns a profile's choices into environment variables for
runtime/run-game, and writes the hook's key=value config file. Options applied
by the hook must also appear in the option table in t2_hook.c.
"""

from __future__ import annotations

import json
import os
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

WON_MODES = ("off", "local", "remote")
WON_ACTIVE = ("local", "remote")
# Backends wonemu offers the game, mirroring BACKEND_KINDS in wonemu.py: each
# kind lists the services it supports and how each is addressed (None: no
# address, "url": http(s) URL, "endpoint": host:port, "chat": optional host:port).
BACKEND_KINDS = {
    "tribesnext": {"auth": None, "master": "url", "irc": "chat"},
    "native": {"auth": None, "master": None, "irc": None},
    "torque": {"master": "endpoint"},
}
BACKEND_SERVICES = ("auth", "master", "irc")
BACKEND_LABELS = {"tribesnext": "TribesNext", "native": "Native (this wonemu)",
                  "torque": "Torque master"}
BACKEND_DEFAULT_ADDRESSES = {("tribesnext", "master"): "http://master.tribesnext.com"}
# TribesNext first for every service, then wonemu's native services.
DEFAULT_BACKENDS = (
    {"kind": "tribesnext", "auth": True, "master": "http://master.tribesnext.com", "irc": True},
    {"kind": "native", "auth": True, "master": True, "irc": True},
)
# The hook rewrites each 28-byte bootstrap string in place.
WON_ENDPOINT_MAX = 27
_ENDPOINT = re.compile(r"^([A-Za-z0-9.-]+):(\d{1,5})$")


@dataclass(frozen=True)
class Option:
    key: str
    section: str  # heading in the launcher's WON or Hook tab
    label: str
    kind: str  # bool, choice, int, float, text, endpoints, backends
    default: object
    help: str = ""
    choices: tuple = ()
    minimum: float | None = None
    maximum: float | None = None
    requires: tuple = ()  # (key, allowed values) pairs that must all hold
    advanced: bool = False
    env: str | None = None  # applied as an environment variable for run-game
    hook: bool = False  # applied through the hook's config file
    tab: str = field(default="hook")

    @property
    def off_value(self):
        """The value an option takes while its requirements are unmet."""
        return False if self.kind == "bool" else self.default


OPTIONS = (
    # WON emulator
    Option("won.mode", "WON emulator", "WON emulation", "choice", "off", tab="won",
           choices=WON_MODES, env="T2LAUNCH_WON_MODE",
           help="off: the game's own login (use -nologin for LAN). local: start wonemu on this "
                "machine; TribesNext accounts work here. remote: use a wonemu on another machine "
                "with its own T2Launch accounts. TribesNext logins are local only, since they "
                "need the account's key files on the machine checking the password."),
    Option("won.endpoints", "WON emulator", "Game connects to (host:port)", "endpoints", (),
           tab="won", env="T2LAUNCH_WON_ENDPOINTS", requires=(("won.mode", WON_ACTIVE),),
           help="Where the game looks for the WON directory: up to three host:port entries of at "
                f"most {WON_ENDPOINT_MAX} characters. Leave empty in local mode to use the bind "
                "address and port below (127.0.0.1:15104 by default)."),
    Option("won.verifier", "WON emulator", "Remote verifier file", "text", "", tab="won",
           env="T2LAUNCH_WON_VERIFIER", requires=(("won.mode", ("remote",)),),
           help="verifier.der from ~/.local/state/t2launch/wonemu/ on the machine running "
                "wonemu; the game checks the emulator's signatures with it."),
    Option("won.bind_address", "Local service", "Listen on", "text", "127.0.0.1", tab="won",
           env="T2LAUNCH_WON_BIND", requires=(("won.mode", ("local",)),),
           help="127.0.0.1 keeps the service on this machine; 0.0.0.0 or a LAN address lets other "
                "machines use it."),
    Option("won.base_port", "Local service", "Port", "int", 15104, tab="won", minimum=1,
           maximum=65531, env="T2LAUNCH_WON_BASE_PORT", requires=(("won.mode", ("local",)),),
           help="Directory port; auth, DBProxy, IRC, and master use the next four."),
    Option("won.advertise_address", "Local service", "Advertise address", "text", "", tab="won",
           env="T2LAUNCH_WON_ADVERTISE", advanced=True, requires=(("won.mode", ("local",)),),
           help="Address the directory gives clients for the other services. Empty: the listen "
                "address, or this machine's outward address when listening on 0.0.0.0."),
    Option("won.backends", "Backends", "Backends", "backends", DEFAULT_BACKENDS, tab="won",
           env="T2LAUNCH_WON_BACKENDS", requires=(("won.mode", ("local",)),),
           help="Which backend serves each service, in priority order. Auth tries each checked "
                "backend in turn; Master merges every checked server list; IRC serves a player's "
                "community data from the backend that logged them in (or the next checked one) "
                "and merges searches across checked backends. A TribesNext IRC address also "
                "relays chat to that IRC network; empty keeps chat local."),
    Option("won.refresh_tribesnext", "Local service", "Refresh TribesNext keys at login", "bool",
           False, tab="won", env="T2LAUNCH_REFRESH_TRIBESNEXT", advanced=True,
           requires=(("won.mode", ("local",)),),
           help="Re-download this account's certificate and key from the TribesNext "
                "account server at each login."),
    Option("won.account_creation", "Local service", "Allow account creation", "bool",
           True, tab="won", env="T2LAUNCH_WON_ACCOUNT_CREATION",
           requires=(("won.mode", ("local",)),),
           help="Let the game's Create Account dialog register accounts with the first "
                "checked Auth backend: TribesNext (this machine only) or Native."),
    Option("won.auth_key_profile", "Local service", "Auth key profile", "choice", "compat",
           tab="won", choices=("compat", "native"), env="T2LAUNCH_AUTH_KEY_PROFILE", advanced=True,
           requires=(("won.mode", ("local",)),),
           help="compat avoids the client's oversized-challenge truncation."),
    # Hook: community and TribesNext
    Option("hook.console_commands", "Hook", "Console commands", "bool", True, hook=True,
           requires=(("launcher.load_hook", (True,)),),
           help="Register the hook's hook_* console commands (SHA1, RSA, TribesNext key access)."),
    Option("hook.tribesnext", "Community", "TribesNext login and game-server auth", "bool", True,
           hook=True,
           requires=(("launcher.load_hook", (True,)), ("won.mode", ("local",)),
                     ("hook.console_commands", (True,))),
           help="Unlock the stored TribesNext key at login and answer T2CSRI challenges "
                "when joining TribesNext servers."),
    Option("hook.tribes_tab", "Community", "Browser TRIBES tab fix", "bool", True, hook=True,
           requires=(("launcher.load_hook", (True,)), ("won.mode", WON_ACTIVE)),
           help="Always fetch a player's tribes through the emulator's community service."),
    # Video
    Option("video.full_redraw", "Video", "Redraw the whole GUI every frame", "bool", True,
           hook=True, requires=(("launcher.load_hook", (True,)),),
           help="Avoids flicker and ghost cursors on drivers that do not preserve the back buffer."),
    Option("video.fps_limit", "Video", "Frame rate limit (0 = off)", "int", 0, hook=True,
           minimum=0, maximum=1000, requires=(("launcher.load_hook", (True,)),),
           help="Cap frames per second to save CPU, GPU, and power; 0 leaves the game uncapped."),
    Option("video.borderless", "Video", "Borderless window", "bool", False, hook=True,
           requires=(("launcher.load_hook", (True,)),),
           help="Draw windowed modes without a title bar or frame; pick the desktop resolution "
                "for borderless fullscreen."),
    Option("video.software_gl", "Video", "Software OpenGL", "bool", False,
           env="T2LAUNCH_SOFTWARE_GL", help="Render with Mesa's software rasterizer."),
    Option("video.xrandr_modes", "Video", "Offer the display's real video modes", "bool", True,
           hook=True, requires=(("launcher.load_hook", (True,)),),
           help="Replace Tribes 2's short built-in windowed mode list with the host's modes."),
    # Server browser
    Option("browser.many_bots", "Server browser", "List servers with more than 16 bots", "bool", True,
           hook=True, requires=(("launcher.load_hook", (True,)),),
           help="The browser hides servers with more than 16 bots unless a filter allows more, "
                "and the filter editor stops at 16. Raise both limits to 255."),
    Option("browser.web_links", "Server browser", "Open web links in the default browser", "bool", True,
           hook=True, requires=(("launcher.load_hook", (True,)),),
           help="The game only knows netscape and mozilla; open links from the game with xdg-open."),
    # Network
    Option("net.packet_limits", "Network", "Allow 64 packets/s and 1000-byte packets", "bool", True,
           hook=True, requires=(("launcher.load_hook", (True,)),),
           help="Raise the client's 32/s and 450-byte limits so QoL-patched TribesNext servers can "
                "send more, and add a matching \"TribesNext QoL\" network preset. Stock servers "
                "still send at their own limits."),
    # Gamescope
    Option("video.gamescope", "Gamescope", "Run inside gamescope", "bool", False,
           env="T2LAUNCH_GAMESCOPE",
           help="Run the game in a gamescope window: render at a lower game resolution and "
                "upscale it (a larger HUD on high-DPI screens), with FSR or NIS filtering. "
                "Keep the game windowed at the game size below; gamescope handles fullscreen."),
    Option("video.gamescope_width", "Gamescope", "Game width (0 = gamescope default)", "int", 0,
           minimum=0, maximum=16384, env="T2LAUNCH_GAMESCOPE_WIDTH",
           requires=(("video.gamescope", (True,)),)),
    Option("video.gamescope_height", "Gamescope", "Game height (0 = gamescope default)", "int", 0,
           minimum=0, maximum=16384, env="T2LAUNCH_GAMESCOPE_HEIGHT",
           requires=(("video.gamescope", (True,)),)),
    Option("video.gamescope_output_width", "Gamescope", "Output width (0 = monitor)", "int", 0,
           minimum=0, maximum=16384, env="T2LAUNCH_GAMESCOPE_OUTPUT_WIDTH", advanced=True,
           requires=(("video.gamescope", (True,)),)),
    Option("video.gamescope_output_height", "Gamescope", "Output height (0 = monitor)", "int", 0,
           minimum=0, maximum=16384, env="T2LAUNCH_GAMESCOPE_OUTPUT_HEIGHT", advanced=True,
           requires=(("video.gamescope", (True,)),)),
    Option("video.gamescope_scaler", "Gamescope", "Scaling", "choice", "auto",
           choices=("auto", "integer", "fit", "fill", "stretch"), env="T2LAUNCH_GAMESCOPE_SCALER",
           requires=(("video.gamescope", (True,)),)),
    Option("video.gamescope_filter", "Gamescope", "Upscale filter", "choice", "linear",
           choices=("linear", "nearest", "fsr", "nis", "pixel"), env="T2LAUNCH_GAMESCOPE_FILTER",
           requires=(("video.gamescope", (True,)),)),
    Option("video.gamescope_fullscreen", "Gamescope", "Gamescope fullscreen", "bool", True,
           env="T2LAUNCH_GAMESCOPE_FULLSCREEN", requires=(("video.gamescope", (True,)),)),
    Option("video.gamescope_grab_cursor", "Gamescope", "Relative mouse mode", "bool", True,
           env="T2LAUNCH_GAMESCOPE_GRAB_CURSOR", advanced=True,
           requires=(("video.gamescope", (True,)),),
           help="Always use relative mouse motion; the game steers by warping the pointer."),
    # Audio
    Option("audio.openal_volume_fix", "Audio", "Volume slider fix", "bool", True,
           env="T2LAUNCH_OPENAL_VOLUME_FIX",
           help="Apply in-game volume to interface sounds, the shell hum, and music, which "
                "Loki's OpenAL otherwise plays at full volume."),
    Option("audio.launch_reset", "Audio", "Reset audio at the launch screen", "bool", True,
           hook=True, requires=(("launcher.load_hook", (True,)),),
           help="Restart the audio driver once the launch screen appears and replay the shell hum."),
    # Runtime
    Option("runtime.thread_join_guard", "Runtime", "Guard detached-thread joins", "bool", True,
           hook=True, advanced=True, requires=(("launcher.load_hook", (True,)),),
           help="Refuse the client's joins on detached threads, which crash modern glibc."),
    Option("runtime.monotonic_clock", "Runtime", "Game clock from the system monotonic clock", "bool",
           True, hook=True, requires=(("launcher.load_hook", (True,)),),
           help="Time the game with CLOCK_MONOTONIC instead of the CPU's timestamp counter, which "
                "can run fast or jump between cores on some CPUs and virtual machines."),
    # Debugging
    Option("debug.trace_connects", "Debugging", "Trace resolver and connect() calls", "bool",
           False, env="T2LAUNCH_TRACE_CONNECTS", advanced=True,
           help="Writes to ~/.local/state/t2launch/diagnostics (connect() needs strace)."),
    Option("debug.trace_openal", "Debugging", "Trace OpenAL gain calls", "bool", False,
           env="T2LAUNCH_REALIGN_TRACE_AL", advanced=True),
)
BY_KEY = {option.key: option for option in OPTIONS}
HOOK_CONFIG_DIR = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "t2launch/hook"


def migrate(profile):
    """Return the profile's options, converting the old "wonemu" flag."""
    options = dict(profile.get("options") or {})
    if "won.mode" not in options:
        options["won.mode"] = "local" if profile.get("wonemu") else "off"
    if "won.master_sources" in options and "won.backends" not in options:
        options["won.backends"] = backends_from_master_sources(options["won.master_sources"])
    options.pop("won.master_sources", None)
    return options


def coerce(option, value):
    """Convert a stored or typed value to the option's kind, or raise ValueError."""
    if option.kind == "bool":
        if isinstance(value, str):
            return value.strip().lower() in ("1", "true", "yes", "on")
        return bool(value)
    if option.kind == "endpoints":
        return _coerce_endpoints(option, value)
    if option.kind == "backends":
        return _coerce_backends(option, value)
    if option.kind == "int":
        value = int(value)
    elif option.kind == "float":
        value = float(value)
    else:
        value = str(value).strip()
    if option.kind == "choice" and value not in option.choices:
        raise ValueError(f"{option.label}: choose one of {', '.join(option.choices)}")
    if option.kind in ("int", "float"):
        if option.minimum is not None and value < option.minimum:
            raise ValueError(f"{option.label}: must be at least {option.minimum}")
        if option.maximum is not None and value > option.maximum:
            raise ValueError(f"{option.label}: must be at most {option.maximum}")
    if option.kind == "text" and re.search(r"[\r\n=]", value):
        raise ValueError(f"{option.label}: may not contain '=' or line breaks")
    return value


def _coerce_endpoints(option, value):
    items = value.split(",") if isinstance(value, str) else list(value or ())
    endpoints = [str(item).strip() for item in items if str(item).strip()]
    if len(endpoints) > 3:
        raise ValueError(f"{option.label}: at most three entries")
    for endpoint in endpoints:
        match = _ENDPOINT.match(endpoint)
        if not match or not 0 < int(match.group(2)) <= 65535:
            raise ValueError(f"{option.label}: {endpoint!r} is not host:port")
        if len(endpoint) > WON_ENDPOINT_MAX:
            raise ValueError(f"{option.label}: {endpoint!r} is longer than {WON_ENDPOINT_MAX} characters")
    return tuple(endpoints)


def _coerce_backends(option, value):
    if isinstance(value, str):
        value = json.loads(value)
    backends = []
    for entry in value or ():
        kind = entry.get("kind") if isinstance(entry, dict) else None
        if kind not in BACKEND_KINDS:
            raise ValueError(f"{option.label}: unknown backend {kind!r}")
        backend = {"kind": kind}
        for service, address_type in BACKEND_KINDS[kind].items():
            enabled = entry.get(service, False)
            if enabled == "":
                enabled = True  # checked with no address (an unchecked cell saves False)
            if not enabled:
                backend[service] = False
            elif address_type is None or (address_type == "chat" and
                                          (enabled is True or not str(enabled).strip())):
                backend[service] = True
            else:
                address = enabled.strip() if isinstance(enabled, str) else ""
                address = (address or BACKEND_DEFAULT_ADDRESSES.get((kind, service), "")).rstrip("/")
                label = f"{BACKEND_LABELS[kind]} {service}"
                if address_type == "url" and not re.match(r"^https?://[^\s,]+$", address):
                    raise ValueError(f"{option.label}: {label} needs an http(s) URL")
                if address_type in ("endpoint", "chat") and not _ENDPOINT.match(address):
                    raise ValueError(f"{option.label}: {label} needs host:port")
                backend[service] = address
        backends.append(backend)
    if not any(backend.get("auth") for backend in backends):
        raise ValueError(f"{option.label}: check Auth for at least one backend")
    return tuple(backends)


def backends_from_master_sources(sources):
    """Backends equivalent to an older won.master_sources list."""
    backends = [dict(backend) for backend in DEFAULT_BACKENDS]
    sources = [tuple(item) for item in sources or ()]
    backends[0]["master"] = next((address for kind, address in sources if kind == "tribesnext"), False)
    backends[1]["master"] = ("lan", "") in sources
    backends += [{"kind": "torque", "master": address} for kind, address in sources if kind == "torque"]
    return backends


def chosen(options):
    """The profile's values merged over the defaults, ignoring unknown keys."""
    values = {option.key: option.default for option in OPTIONS}
    for key, value in (options or {}).items():
        if key in BY_KEY:
            try:
                values[key] = coerce(BY_KEY[key], value)
            except (TypeError, ValueError):
                pass
    return values


def unmet(option, values, context):
    """The requirements of an option that its profile does not meet."""
    merged = {**values, **(context or {})}
    return [(key, allowed) for key, allowed in option.requires if merged.get(key) not in allowed]


def effective(options, context=None):
    """Values after dependencies: an option with unmet requirements is off.

    Requirements are resolved in declaration order, so an option may depend on
    one declared before it (hook.tribesnext on hook.console_commands).
    """
    values = chosen(options)
    for option in OPTIONS:
        if unmet(option, values, context):
            values[option.key] = option.off_value
    return values


def validate(options, context=None):
    """Raise ValueError for the first value or combination the profile rejects."""
    for key, value in (options or {}).items():
        if key in BY_KEY:
            coerce(BY_KEY[key], value)
    values = effective(options, context)
    if values["video.gamescope"] and not shutil.which("gamescope"):
        raise ValueError("Run inside gamescope needs gamescope installed")
    if values["won.mode"] == "remote":
        if not values["won.endpoints"]:
            raise ValueError("Remote WON emulation needs the remote host:port to connect to")
        if not values["won.verifier"]:
            raise ValueError("Remote WON emulation needs the remote wonemu's verifier file")


def _text(value):
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, (tuple, list)):
        if value and isinstance(value[0], dict):  # backends
            return json.dumps(list(value), separators=(",", ":"))
        return ",".join(value)  # endpoints
    return str(value)


def launch_env(options, context=None):
    """Environment variables for runtime/run-game from a profile's options."""
    values = effective(options, context)
    env = {option.env: _text(values[option.key]) for option in OPTIONS
           if option.env and not unmet(option, values, context)}
    # Options a mode does not use are left out, so run-game applies its defaults.
    env["T2LAUNCH_WON_MODE"] = values["won.mode"]
    return env


def hook_config(options, context=None):
    """The hook's config file text (key=value lines) from a profile's options."""
    values = effective(options, context)
    lines = ["# Written by T2Launch for one launch; see t2options.py."]
    lines += [f"{option.key}={_text(values[option.key])}" for option in OPTIONS if option.hook]
    return "\n".join(lines) + "\n"


def write_hook_config(profile_name, options, context=None):
    """Write the hook config for a profile (mode 0600) and return its path."""
    HOOK_CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    HOOK_CONFIG_DIR.chmod(0o700)
    slug = re.sub(r"[^A-Za-z0-9._-]+", "_", profile_name).strip("._") or "profile"
    path = HOOK_CONFIG_DIR / f"{slug}.conf"
    fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        stream.write(hook_config(options, context))
    os.chmod(path, 0o600)
    return path
