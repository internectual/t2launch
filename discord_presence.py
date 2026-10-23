#!/usr/bin/env python3
"""Show what Tribes 2 is doing as Discord rich presence.

The hook starts this helper beside the game and writes one line to its stdin
whenever the game's state changes: "menu", "joining", "demo", or
"game<TAB>server<TAB>map<TAB>game type<TAB>players". The helper passes it on
to the Discord client over Discord's local IPC socket, connecting whenever
Discord is running, and clears the presence and exits when the game closes
its end of the pipe.
"""

from __future__ import annotations

import argparse
import json
import os
import select
import socket
import struct
import sys
import time
import uuid
from pathlib import Path

OP_HANDSHAKE = 0
OP_FRAME = 1
OP_CLOSE = 2
RECONNECT_SECONDS = 15
TIMEOUT_SECONDS = 5
FRAME_MAX_BYTES = 1 << 20
# Discord's own socket and the ones Flatpak and Snap builds and Vesktop make.
SOCKET_FOLDERS = ("", "app/com.discordapp.Discord", "app/com.discordapp.DiscordCanary",
                  "snap.discord", "app/dev.vencord.Vesktop", ".flatpak/dev.vencord.Vesktop/xdg-run")
FIELD_MAX = 128
LARGE_IMAGE = "tribes2"


def socket_paths(environ=os.environ) -> list[Path]:
    bases = []
    for base in (environ.get("XDG_RUNTIME_DIR"), environ.get("TMPDIR"), "/tmp"):
        if base and Path(base) not in bases:
            bases.append(Path(base))
    return [base / folder / f"discord-ipc-{index}"
            for base in bases for folder in SOCKET_FOLDERS for index in range(10)]


def encode_frame(op: int, payload: dict) -> bytes:
    data = json.dumps(payload, separators=(",", ":")).encode()
    return struct.pack("<II", op, len(data)) + data


def read_exact(sock: socket.socket, count: int) -> bytes:
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("Discord closed the connection")
        data += chunk
    return data


def read_frame(sock: socket.socket) -> tuple[int, dict]:
    op, length = struct.unpack("<II", read_exact(sock, 8))
    if length > FRAME_MAX_BYTES:
        raise ConnectionError("Discord sent an oversized frame")
    return op, json.loads(read_exact(sock, length) or b"{}")


class Discord:
    """One IPC connection to the local Discord client."""

    def __init__(self, app_id: str, pid: int, environ=os.environ):
        self.app_id = app_id
        self.pid = pid
        self.environ = environ
        self.sock: socket.socket | None = None

    def connect(self) -> bool:
        for path in socket_paths(self.environ):
            if not path.exists():
                continue
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(TIMEOUT_SECONDS)
            try:
                sock.connect(str(path))
                sock.sendall(encode_frame(OP_HANDSHAKE, {"v": 1, "client_id": self.app_id}))
                op, reply = read_frame(sock)
            except (OSError, ValueError, ConnectionError):
                sock.close()
                continue
            if op == OP_FRAME and reply.get("evt") == "READY":
                self.sock = sock
                return True
            sock.close()
            # The client refused this application ID; another socket won't differ.
            print(f"discord_presence: Discord refused the handshake: {reply.get('message', reply)}",
                  file=sys.stderr)
            return False
        return False

    def set_activity(self, activity: dict | None) -> None:
        if self.sock is None:
            return
        payload = {"cmd": "SET_ACTIVITY", "args": {"pid": self.pid, "activity": activity},
                   "nonce": str(uuid.uuid4())}
        try:
            self.sock.sendall(encode_frame(OP_FRAME, payload))
            op, reply = read_frame(self.sock)
        except (OSError, ValueError, ConnectionError):
            self.close()
            return
        if op == OP_CLOSE:
            self.close()
        elif reply.get("evt") == "ERROR":
            print(f"discord_presence: {reply.get('data', {}).get('message', reply)}", file=sys.stderr)

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None


def clean(text: str) -> str:
    """Drop Torque's colour and tag control characters and fit Discord's limits."""
    text = " ".join("".join(ch for ch in text if ch >= " ").split())
    if len(text) > FIELD_MAX:
        text = text[:FIELD_MAX - 1] + "…"
    return text


def activity_for(state: str, started: int) -> dict:
    fields = state.split("\t")
    activity = {"timestamps": {"start": started},
                "assets": {"large_image": LARGE_IMAGE, "large_text": "Tribes 2"}}
    if fields[0] == "game" and len(fields) >= 5:
        server, mission, mission_type, players = (clean(field) for field in fields[1:5])
        details = " - ".join(part for part in (mission, mission_type) if part)
        # Discord rejects details and state shorter than two characters.
        activity["details"] = details if len(details) >= 2 else "In a game"
        if server and players.isdigit():
            count = int(players)
            server = clean(f"{server} ({count} player{'' if count == 1 else 's'})")
        if len(server) >= 2:
            activity["state"] = server
    elif fields[0] == "joining":
        activity["details"] = "Joining a game"
    elif fields[0] == "demo":
        activity["details"] = "Watching a demo"
    else:
        activity["details"] = "In the menus"
    return activity


def session_key(state: str) -> tuple:
    """States that share a key keep their start time: menus, or one server."""
    fields = state.split("\t")
    if fields[0] in ("game", "joining"):
        return ("game", fields[1] if fields[0] == "game" and len(fields) > 1 else None)
    return (fields[0],)


def run(app_id: str, pid: int, stream, environ=os.environ, clock=time.time) -> int:
    discord = Discord(app_id, pid, environ)
    state = "menu"
    key = session_key(state)
    started = int(clock())
    sent = None
    next_attempt = 0.0
    buffered = b""
    fd = stream.fileno()
    while True:
        if discord.sock is None and clock() >= next_attempt:
            next_attempt = clock() + RECONNECT_SECONDS
            if discord.connect():
                sent = None
        if discord.sock is not None and sent != (state, started):
            discord.set_activity(activity_for(state, started))
            sent = (state, started) if discord.sock is not None else None
        watched = [fd] + ([discord.sock] if discord.sock is not None else [])
        ready, _, _ = select.select(watched, [], [], RECONNECT_SECONDS)
        if discord.sock is not None and discord.sock in ready:
            # Discord sends nothing unasked; readable means it closed or errored.
            try:
                read_frame(discord.sock)
            except (OSError, ValueError, ConnectionError):
                discord.close()
        if fd not in ready:
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            discord.set_activity(None)
            discord.close()
            return 0
        buffered += chunk
        while b"\n" in buffered:
            line, buffered = buffered.split(b"\n", 1)
            state = line.decode("utf-8", errors="replace").rstrip("\r") or "menu"
            if session_key(state) != key:
                key = session_key(state)
                started = int(clock())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--app-id", required=True, help="Discord application ID")
    parser.add_argument("--pid", type=int, default=os.getppid(), help="the game's process ID")
    args = parser.parse_args(argv)
    if not args.app_id.isdigit():
        print("discord_presence: the application ID must be a number", file=sys.stderr)
        return 2
    return run(args.app_id, args.pid, sys.stdin)


if __name__ == "__main__":
    sys.exit(main())
