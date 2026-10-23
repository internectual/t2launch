#!/usr/bin/env python3
"""Local WON directory/auth emulator and service proxy for Tribes 2 #25034.

The directory, Auth1 login, and Torque master-server packet formats are recovered
from tribes2.dynamic, the original Torque C# master server, and TribesNext's
unchanged client scripts.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import hmac
import itertools
import json
import os
import secrets
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import ClassVar

import community_store
import tribesnext
import woncrypto

HOST = "127.0.0.1"
DIRECTORY_PORT = 15104
MAX_FRAME = 255999  # RecvMsgOp's upper bound at 0x083B717A
IDENTITY = b"T2LAUNCH-WONEMU/1"
AUTH_STAGE = "directory + local-account Auth1 + IRC auth + SQLite community DB + Torque/TribesNext master proxy"
LOG_DIR = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "t2launch/wonemu"
PORTS = {"directory": 15104, "auth": 15105, "dbproxy": 15106, "irc": 15107, "master": 15108}
TRIBESNEXT_MASTER_URL = "http://master.tribesnext.com"
# Each new message costs one robot read, so a mailbox poll fetches at most this
# many (the TribesNext client's own chunk size).
TRIBESNEXT_MAIL_PAGE = 25
TORQUE_MASTER_PACKET_MAX = 1500
TORQUE_MASTER_SERVER_SIZE = 6
TORQUE_MASTER_MAX_SERVERS = (TORQUE_MASTER_PACKET_MAX - 12) // TORQUE_MASTER_SERVER_SIZE


def implementation_fingerprint():
    digest = hashlib.sha256()
    for module in (__import__(__name__), community_store, tribesnext, woncrypto):
        path = Path(module.__file__).resolve()
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


IMPLEMENTATION_SHA256 = implementation_fingerprint()


def frame(payload):
    length = len(payload) + 4
    if not 4 < length <= MAX_FRAME:
        raise ValueError("WON frame length out of range")
    return struct.pack("<I", length) + payload


def recv_exact(conn, length, clean_eof=False):
    data = bytearray()
    while len(data) < length:
        chunk = conn.recv(length - len(data))
        if not chunk:
            if not data and clean_eof:
                return None
            raise ValueError("Connection closed in the middle of a frame")
        data.extend(chunk)
    return bytes(data)


def recv_frame(conn):
    prefix = recv_exact(conn, 4, clean_eof=True)
    if prefix is None:
        return None
    length, = struct.unpack("<I", prefix)
    if not 4 < length <= MAX_FRAME:
        raise ValueError(f"Invalid frame length: {length}")
    return recv_exact(conn, length - 4)


def digest(data):
    """Log fields for a frame: its size and hash, never its bytes."""
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def take(self, length):
        if length < 0 or self.offset + length > len(self.data):
            raise ValueError("Truncated WON message")
        result = self.data[self.offset:self.offset + length]
        self.offset += length
        return result

    def number(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]

    def wide(self):
        return self.take(self.number("<H") * 2).decode("utf-16le")


def wide(value):
    data = value.encode("utf-16le")
    return struct.pack("<H", len(data) // 2) + data


def read_torque_string(reader):
    """Read BinaryWriter.Write(string)'s 7-bit UTF-8 length-prefixed format."""
    length = 0
    shift = 0
    for _ in range(5):
        value = reader.number("<B")
        length |= (value & 0x7f) << shift
        if not value & 0x80:
            if length > MAX_FRAME:
                raise ValueError("Torque master string exceeds frame limit")
            return reader.take(length).decode("utf-8")
        shift += 7
    raise ValueError("Malformed Torque master string length")


def torque_string(value):
    data = str(value).encode("utf-8")
    length = len(data)
    prefix = bytearray()
    while length >= 0x80:
        prefix.append((length & 0x7f) | 0x80)
        length >>= 7
    prefix.append(length)
    return bytes(prefix) + data


def parse_torque_list_request(payload):
    reader = Reader(payload)
    header, flags, session, key = (reader.number("<B"), reader.number("<B"),
                                   reader.number("<H"), reader.number("<H"))
    if header != 6:
        raise ValueError("Not a Torque master server-list request")
    packet_index = reader.number("<B")
    query = {
        "flags": flags,
        "session": session,
        "key": key,
        "packet_index": packet_index,
        "game_type": read_torque_string(reader),
        "mission_type": read_torque_string(reader),
        "min_players": reader.number("<B"),
        "max_players": reader.number("<B"),
        "regions": reader.number("<I"),
        "version": reader.number("<I"),
        "info_flags": reader.number("<B"),
        "max_bots": reader.number("<B"),
        "min_cpu": reader.number("<H"),
    }
    buddy_count = reader.number("<B")
    query["buddies"] = [reader.number("<I") for _ in range(buddy_count)]
    if reader.offset != len(payload):
        raise ValueError("Trailing bytes in Torque master server-list request")
    return query


def decode_getdir(payload):
    reader = Reader(payload)
    header = reader.number("<B")
    service = reader.number("<H")
    message = reader.number("<H")
    if (header, service, message) != (5, 2, 103):
        raise ValueError(f"Unhandled message header/service/type: {header}/{service}/{message}")
    flags = reader.number("<I")
    per_reply = reader.number("<H")
    path = reader.wide()
    data_type_count = reader.number("<H")
    data_types = []
    for _ in range(data_type_count):
        data_types.append(reader.take(reader.number("<B")).hex())
    if reader.offset != len(payload):
        raise ValueError(f"Trailing bytes after GetDir: {len(payload) - reader.offset}")
    return {"path": path, "flags": flags, "entities_per_reply": per_reply, "data_types": data_types}


def service_entity(name, port, display_name="0", address=HOST):
    return {"name": name, "display_name": display_name, "port": port, "address": address}


def directory_records(ports, address=HOST):
    """Directory entries for each service, advertised at an IPv4 address."""
    return {
        "/TitanServers/Auth": [service_entity("AuthServer", ports["auth"], address=address)],
        "/TitanServers/DBProxy": [service_entity("DBProxyServer", ports["dbproxy"], address=address)],
        "/Tribes2": [service_entity("IRCServer", ports["irc"], address=address),
                     service_entity("MasterServer", ports["master"], address=address)],
    }


# Master-server sources, merged in order. Each is a kind and an address:
#   lan                  game servers that heartbeat to this master
#   tribesnext=URL       the TribesNext HTTP list service
#   torque=HOST:PORT     another Torque-protocol master (read-only)
MASTER_SOURCE_KINDS = ("lan", "tribesnext", "torque")


def parse_master_sources(spec, tribesnext_url=TRIBESNEXT_MASTER_URL):
    """Parse "kind[=address],..." into (kind, address) pairs, in order."""
    sources = []
    for item in (part.strip() for part in str(spec).split(",")):
        if not item:
            continue
        kind, _, address = item.partition("=")
        kind, address = kind.strip().lower(), address.strip()
        if kind not in MASTER_SOURCE_KINDS:
            raise ValueError(f"Unknown master-server source kind: {kind}")
        if kind == "lan":
            address = ""
        elif kind == "tribesnext":
            address = (address or tribesnext_url).rstrip("/")
            if urllib.parse.urlsplit(address).scheme not in ("http", "https"):
                raise ValueError(f"TribesNext master source needs an http(s) URL: {address}")
        else:
            host, port = parse_endpoint(address)
            address = f"{host}:{port}"
        if (kind, address) not in sources:
            sources.append((kind, address))
    return sources


def master_backend_sources(backend, tribesnext_url=TRIBESNEXT_MASTER_URL):
    """Sources equivalent to the older --master-backend choices."""
    url = tribesnext_url.rstrip("/")
    return {"local": [("lan", "")], "tribesnext": [("tribesnext", url)],
            "hybrid": [("tribesnext", url), ("lan", "")]}[backend]


def format_master_sources(sources):
    return ",".join(kind if not address else f"{kind}={address}" for kind, address in sources)


# Backends wonemu can use for each service it offers the game. A kind lists
# the services it supports and how each one is addressed: None means the
# service takes no address, "url" an http(s) URL, "endpoint" a host:port, and
# "chat" an optional host:port (TribesNext IRC: checked serves community data;
# an address also relays chat to that IRC network).
# A profile's backends are an ordered list (priority and fallback order) of
# {"kind": ..., "auth": bool, "master": False|True|address, "irc": ...}.
BACKEND_KINDS = {
    "tribesnext": {"auth": None, "master": "url", "irc": "chat"},
    "native": {"auth": None, "master": None, "irc": None},
    "torque": {"master": "endpoint"},
}
BACKEND_SERVICES = ("auth", "master", "irc")
# The chat server TribesNext's published client fix used; it no longer
# resolves, so chat relaying is off until a current server is configured.
TRIBESNEXT_IRC_SERVER = "irc.arloria.net:6667"
BACKEND_DEFAULT_ADDRESSES = {("tribesnext", "master"): TRIBESNEXT_MASTER_URL}


def default_backends(tribesnext_url=TRIBESNEXT_MASTER_URL):
    """TribesNext first for every service, then wonemu's native services."""
    return [{"kind": "tribesnext", "auth": True, "master": tribesnext_url.rstrip("/"), "irc": True},
            {"kind": "native", "auth": True, "master": True, "irc": True}]


def parse_backends(spec, tribesnext_url=TRIBESNEXT_MASTER_URL):
    """Validate a backend list (JSON text or a list) into canonical dicts."""
    entries = json.loads(spec) if isinstance(spec, str) else spec
    if not isinstance(entries, list):
        raise ValueError("Backends must be a list")  # noqa: TRY004 (configuration errors are ValueError)
    backends = []
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("kind") not in BACKEND_KINDS:
            raise ValueError(f"Unknown backend: {entry!r}")
        kind = entry["kind"]
        backend = {"kind": kind}
        for service in BACKEND_SERVICES:
            value = entry.get(service, False)
            if value == "":
                value = True  # checked with no address
            if service not in BACKEND_KINDS[kind]:
                if value:
                    raise ValueError(f"The {kind} backend has no {service} service")
                continue
            address_type = BACKEND_KINDS[kind][service]
            if not value:
                backend[service] = False
            elif address_type is None or (address_type == "chat" and
                                          (value is True or not str(value).strip())):
                backend[service] = True
            else:
                address = value if isinstance(value, str) and value.strip() else \
                    BACKEND_DEFAULT_ADDRESSES.get((kind, service), "")
                if kind == "tribesnext" and service == "master" and value is True:
                    address = tribesnext_url
                address = str(address).strip().rstrip("/")
                if address_type == "url":
                    if urllib.parse.urlsplit(address).scheme not in ("http", "https"):
                        raise ValueError(f"The {kind} {service} service needs an http(s) URL")
                else:
                    host, port = parse_endpoint(address)
                    address = f"{host}:{port}"
                backend[service] = address
        backends.append(backend)
    if not any(backend.get("auth") for backend in backends):
        raise ValueError("At least one backend must provide auth")
    return backends


def format_backends(backends):
    return json.dumps(backends, separators=(",", ":"))


def backends_master_sources(backends):
    """The ordered master-server sources a backend list enables."""
    sources = []
    for backend in backends:
        value = backend.get("master")
        if not value:
            continue
        source = {"tribesnext": ("tribesnext", value), "native": ("lan", ""),
                  "torque": ("torque", value)}[backend["kind"]]
        if source not in sources:
            sources.append(source)
    return sources


def backends_from_master_sources(sources, tribesnext_url=TRIBESNEXT_MASTER_URL):
    """Backends equivalent to a master-source list, keeping default auth and IRC."""
    backends = default_backends(tribesnext_url)
    backends[0]["master"] = next((address for kind, address in sources if kind == "tribesnext"), False)
    backends[1]["master"] = ("lan", "") in sources
    backends += [{"kind": "torque", "master": address} for kind, address in sources if kind == "torque"]
    return backends


def session_community(session):
    """A session's community backend; older sessions imply it from their login."""
    community = session.get("community")
    if community is None and "community" not in session:
        community = "tribesnext" if session.get("auth_provider") == "tribesnext" else "native"
    return community


def parse_endpoint(text):
    """Parse "host:port", returning (host, port)."""
    host, separator, port_text = str(text).strip().rpartition(":")
    if not separator or not host or not port_text.isdecimal() or not 0 < int(port_text) <= 65535:
        raise ValueError(f"Expected host:port, got {text!r}")
    return host, int(port_text)


def is_loopback(address):
    return address == "localhost" or address.startswith("127.")


def from_this_machine(conn, peer):
    """True for a peer on this host: loopback, or this socket's own address.

    When listening on a LAN address, this machine's own game reaches the
    services through that address rather than loopback."""
    try:
        return is_loopback(peer[0]) or peer[0] == conn.getsockname()[0]
    except OSError:
        return False


def advertised_address(bind_address, advertise_address=""):
    """IPv4 address the directory gives clients for the other services."""
    if advertise_address:
        return socket.gethostbyname(advertise_address)
    if bind_address in ("", "0.0.0.0"):
        # The address of the interface that routes outward, without sending.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            try:
                probe.connect(("192.0.2.1", 9))
                return probe.getsockname()[0]
            except OSError:
                return HOST
    return socket.gethostbyname(bind_address)


def encode_entity(entity, flags):
    """Subset of DirEntity fields consumed by the bootstrap completion code.

    The response advertises its own field mask. It intentionally omits metadata
    such as ACL/data objects until those fields are implemented, rather than
    echoing an unsupported request mask and corrupting the client's parser.
    """
    data = bytearray()
    if flags & 0x10:
        data += b"S"
    if flags & 0x02000000:
        data += wide(entity["name"])
    if flags & 0x04000000:
        # IPAddr::Set at 0x083B42F0 expects sockaddr_in's port then IPv4 bytes.
        address = struct.pack("!H", entity["port"]) + socket.inet_aton(entity.get("address", HOST))
        data += bytes([len(address)]) + address
    if flags & 0x20:
        data += wide(entity["display_name"])
    return bytes(data)


def getdir_response(request, records):
    entries = records.get(request["path"])
    if entries is None:
        # The GetDir parser returns a negative status before reading entities.
        return frame(struct.pack("<BHHh", 5, 2, 3, -1))
    flags = 0x2 | (request["flags"] & (0x10 | 0x02000000 | 0x04000000 | 0x20))
    limit = request["entities_per_reply"] or max(1, len(entries))
    batches = [entries[i:i + limit] for i in range(0, len(entries), limit)] or [[]]
    replies = []
    for index, batch in enumerate(batches):
        if index > 127:
            raise ValueError("Too many directory reply fragments")
        sequence = index | (0x80 if index == len(batches) - 1 else 0)
        payload = struct.pack("<BHHhBIH", 5, 2, 3, 0, sequence, flags, len(batch))
        payload += b"".join(encode_entity(entity, flags) for entity in batch)
        replies.append(frame(payload))
    return b"".join(replies)


def robot_memberships(memberships):
    """Normalize TribesNext robot memberships to id/name/tag/append/rank/title."""
    return [{"id": int(tribe["id"]), "name": tribe["name"], "tag": tribe.get("tag", ""),
             "append": int(str(tribe.get("append", "0")).strip() == "1"),
             "rank": int(tribe["rank"]), "title": tribe["title"]}
            for tribe in memberships]


def community_memberships(rows):
    """Normalize local community memberships to id/name/tag/append/rank/title."""
    return [{"id": int(tribe["tribe_id"]), "name": tribe["name"], "tag": tribe["tag"],
             "append": int(tribe.get("appending") or 0), "rank": int(tribe["access"]),
             "title": tribe["title"]}
            for tribe in rows]


def parse_login_payload(payload, session_key):
    """Decode the native auth payload without returning it to the traffic log."""
    reader = Reader(payload)
    if reader.take(2) != session_key[:2]:
        raise ValueError("Login payload session-key echo mismatch")
    key_block_id = reader.number("<H")
    if key_block_id != 1:
        raise ValueError("Unsupported login key-block id")
    request_kind = reader.number("<I")
    if request_kind not in (14, 15):
        raise ValueError("Unknown native login payload type")
    username = reader.wide()
    password = reader.wide()
    flags = (reader.number("<H"), reader.number("<H"))
    client = reader.wide()
    # Some client builds append login extensions after the stable username,
    # password, flags, and client fields. Ignore the opaque tail and expose only
    # its length; credentials remain validated against the selected local store.
    extension_bytes = len(payload) - reader.offset
    reader.take(extension_bytes)
    return {"key_block_id": key_block_id, "request_kind": request_kind,
            "username": username, "password": password, "flags": flags, "client": client,
            "extension_bytes": extension_bytes}


def decode_login_envelope(payload, keys, private_store=None, account_store=None,
                          refresh_tribesnext=None, auth_order=("tribesnext", "native"),
                          recover_tribesnext=None):
    reader = Reader(payload)
    if reader.take(5) != bytes.fromhex("05 0c 00 03 00"):
        raise ValueError("Not a native login request")
    block_id = reader.number("<H")
    if block_id != 1:
        raise ValueError("Unsupported authentication key-block id")
    encrypted_key = reader.take(reader.number("<H"))
    encrypted_data = reader.take(reader.number("<H"))
    if reader.offset != len(payload):
        raise ValueError("Trailing login request bytes")
    session_key = woncrypto.decrypt_elgamal(keys["auth"], encrypted_key)
    blocks = len(encrypted_key) // (4 + 2 * ((keys["auth"]["p"].bit_length() + 7) // 8))
    result = {"key_block_id": block_id, "elgamal_blocks": blocks,
              "elgamal_plaintext_bytes": len(session_key),
              "elgamal_plaintext_sha256": hashlib.sha256(session_key).hexdigest(),
              "encrypted_login_data_bytes": len(encrypted_data),
              "encrypted_login_data_sha256": hashlib.sha256(encrypted_data).hexdigest(),
              "login_data_decryption": "pending"}
    try:
        symmetric_key, login_data = woncrypto.decrypt_won_login_data(
            keys["auth"], encrypted_key, encrypted_data)
        fields = parse_login_payload(login_data, symmetric_key)
    except (OSError, ValueError) as exc:
        result["login_data_error"] = str(exc)
    else:
        # Try each auth backend in priority order; the first valid one wins.
        # A wrong password or a missing account falls through to the next.
        account_enrolled = False
        tribesnext_validation = "not_used"
        account_validation = "account_missing"
        credential_source = "unknown"
        recovery_status = "not_configured"
        for backend in auth_order:
            if backend == "tribesnext":
                tribesnext_validation = woncrypto.verify_private_store_password(
                    private_store, fields["username"], fields["password"])
                # An account not installed here is fetched from TribesNext
                # with the name and password, then checked like any other.
                if (tribesnext_validation in ("account_missing", "store_missing") and
                        recover_tribesnext is not None):
                    recovery_status = recover_tribesnext(fields["username"], fields["password"])
                    if recovery_status == "refreshed":
                        tribesnext_validation = woncrypto.verify_private_store_password(
                            private_store, fields["username"], fields["password"])
                outcome = tribesnext_validation
            elif backend == "native" and account_store is not None and \
                    account_store.local_account_exists(fields["username"]):
                outcome, account_enrolled = account_store.verify_local_account(
                    fields["username"], fields["password"])
            else:
                continue
            if outcome == "valid":
                account_validation = "valid"
                credential_source = "tribesnext" if backend == "tribesnext" else "local"
                break
            # Report the most specific failure: a known account beats a missing one.
            if account_validation in ("account_missing", "store_missing") or outcome == "invalid":
                account_validation = outcome
        if tribesnext_validation != "valid":
            if recovery_status == "not_configured":
                recovery_status = "not_used"
        elif recovery_status != "refreshed" and refresh_tribesnext is not None:
            try:
                recovery_status = refresh_tribesnext(fields["username"], fields["password"])
            except (OSError, ValueError, UnicodeError):
                recovery_status = "failed"
        result.update(login_data_decryption="decoded", symmetric_key_bytes=len(symmetric_key),
                      login_plaintext_bytes=len(login_data),
                      login_plaintext_sha256=hashlib.sha256(login_data).hexdigest(),
                      login_request_kind=fields["request_kind"],
                       username_utf16_bytes=len(fields["username"].encode("utf-16le")),
                       password_utf16_bytes=len(fields["password"].encode("utf-16le")),
                      client_identifier=fields["client"], login_flags=list(fields["flags"]),
                      login_extension_bytes=fields["extension_bytes"],
                      account_validation=account_validation,
                      credential_source=credential_source,
                      tribesnext_recovery=recovery_status,
                      account_enrolled=account_enrolled)
    return result


class CaptureLog:
    MAX_BYTES = 5 * 1024 * 1024

    def __init__(self, path):
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.parent.chmod(0o700)
        # Keep one previous generation; the log is diagnostic, not an archive.
        if path.exists() and path.stat().st_size > self.MAX_BYTES:
            path.replace(path.with_name(path.name + ".1"))
        fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
        os.chmod(path, 0o600)
        self.stream = os.fdopen(fd, "a", encoding="utf-8", buffering=1)
        self.lock = threading.Lock()

    def event(self, event, **fields):
        record = {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(),  # noqa: UP017
                  "event": event, **fields}
        with self.lock:
            if not self.stream.closed:
                self.stream.write(json.dumps(record) + "\n")

    def close(self):
        with self.lock:
            self.stream.close()


class TCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False


class UDPServer(socketserver.ThreadingUDPServer):
    allow_reuse_address = True
    daemon_threads = True


class DirectoryHandler(socketserver.BaseRequestHandler):
    def handle(self):
        emulator = self.server.emulator
        conn = self.request
        conn.settimeout(10)
        connection = next(emulator.connections)
        try:
            while True:
                payload = recv_frame(conn)
                if payload is None:
                    break
                # Launcher health checks identify this service, not just an open
                # port. Identity and STOP are answered only for this machine.
                if payload.startswith(IDENTITY) and not from_this_machine(conn, self.client_address):
                    emulator.log.event("control_refused", connection=connection,
                                       peer=list(self.client_address))
                    break
                if payload == IDENTITY:
                    conn.sendall(frame(json.dumps(emulator.identity()).encode("ascii")))
                    continue
                if payload == IDENTITY + b" STOP":
                    conn.sendall(frame(IDENTITY + b" STOPPING"))
                    emulator.stopped.set()
                    break
                emulator.log.event("received", service="directory", connection=connection,
                                   peer=list(self.client_address), **digest(payload))
                try:
                    request = decode_getdir(payload)
                except ValueError as exc:
                    emulator.log.event("unhandled", service="directory", connection=connection, error=str(exc))
                    break
                emulator.log.event("getdir", connection=connection, **request)
                reply = getdir_response(request, emulator.records)
                conn.sendall(reply)
                emulator.log.event("sent", service="directory", connection=connection,
                                   path=request["path"], **digest(reply),
                                   entities=emulator.records.get(request["path"], []))
                print(f"[wonemu] GetDir {request['path']} flags={request['flags']:#010x} "
                      f"→ {len(emulator.records.get(request['path'], []))} service(s)", flush=True)
        except (OSError, ValueError) as exc:
            emulator.log.event("connection_error", service="directory", connection=connection, error=str(exc))


class CaptureHandler(socketserver.BaseRequestHandler):
    def handle(self):
        emulator = self.server.emulator
        connection = next(emulator.connections)
        service = self.server.service
        self.request.settimeout(2)
        data = bytearray()
        emulator.log.event("connected", service=service, connection=connection, peer=list(self.client_address))
        # Unknown service framing is preserved verbatim rather than guessed.
        try:
            while len(data) < MAX_FRAME:
                chunk = self.request.recv(min(4096, MAX_FRAME - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
                emulator.log.event("received", service=service, connection=connection, **digest(chunk))
        except TimeoutError:
            pass
        except OSError as exc:
            emulator.log.event("connection_error", service=service, connection=connection, error=str(exc))
        emulator.log.event("capture_only", service=service, connection=connection, **digest(data))
        print(f"[wonemu] {service}: captured {len(data)} bytes; handler not implemented", flush=True)


# WON DBProxy (service 14) as the stock #25034 login screen uses it. Replies
# share one envelope: u8 5 | u16 14 | u16 message | u16 sub | i16 status |
# u16 length + text | u8 0x80 | body. Account requests are 1002 with a sub
# type; the reply's sub type is the request's plus one.
DBPROXY_SERVICE = 14
DBPROXY_KEY_REQUEST = 2      # reply 3 carries u16 length + DER public key
DBPROXY_SESSION_KEY = 4      # ElGamal-wrapped Blowfish key for message 5
DBPROXY_ENCRYPTED = 5        # CryptTransform(1002 request), answered in the clear
DBPROXY_REQUEST = 1002
DBPROXY_CREATE_ACCOUNT = 2
DBPROXY_UPDATE_ACCOUNT = 4
DBPROXY_ACCOUNT_INFO = 6
DBPROXY_PASSWORD_EMAIL = 8
DBPROXY_LOGIN_NAME_EMAIL = 14
DBPROXY_ACCOUNT_LOOKUP = 16
# Status codes console_start.cs turns into its own messages.
DBPROXY_DB_ERROR = -2800
DBPROXY_NO_MODULE = -2802
DBPROXY_USER_EXISTS = -2803
DBPROXY_INVALID_USERNAME = -2804
DBPROXY_USER_DOES_NOT_EXIST = -2806
DBPROXY_INVALID_CDKEY = -2807
# A code console_start.cs does not name, so it shows "Error - <text>".
DBPROXY_SHOW_TEXT = -2801
DBPROXY_PRODUCT_DATA = 0x62  # data object: u8-length CD key, u8-length warrior name
DBPROXY_SEND_INFO = 1        # data object: the "send me information" checkbox
PEER_SESSION_SECONDS = 60 * 60


def dbproxy_reply(message, sub, status=0, text="", body=b""):
    text = str(text).encode("ascii", "replace")
    return (struct.pack("<BHHHhH", 5, DBPROXY_SERVICE, message, sub, status, len(text)) +
            text + b"\x80" + body)


def read_won_data_objects(reader):
    """WON data objects: kind 1 is u16 key + bool; kind 2 is u16 key + blob."""
    flags, blobs = {}, {}
    for _ in range(reader.number("<H")):
        kind = reader.number("<H")
        data = Reader(reader.take(reader.number("<H")))
        key = data.number("<H")
        if kind == 1:
            flags[key] = bool(data.number("<B")) if data.offset < len(data.data) else None
        elif kind == 2:
            blobs[key] = data.take(len(data.data) - data.offset)
    return flags, blobs


def won_data_flags(flags):
    return struct.pack("<H", len(flags)) + b"".join(
        struct.pack("<HHHB", 1, 3, key, 1 if value else 0) for key, value in flags.items())


def _ascii_field(reader):
    return reader.take(reader.number("<H")).decode("ascii")


def parse_dbproxy_create_account(message):
    reader = Reader(message[7:])
    fields = {"login": reader.wide(), "password": reader.wide(), "email": _ascii_field(reader)}
    flags, blobs = read_won_data_objects(reader)
    product = Reader(blobs.get(DBPROXY_PRODUCT_DATA, b""))
    fields["cd_key"] = product.take(product.number("<B")).decode("ascii")
    fields["warrior"] = product.take(product.number("<B")).decode("latin-1")
    fields["send_info"] = bool(flags.get(DBPROXY_SEND_INFO))
    return fields


def parse_dbproxy_update_account(message):
    reader = Reader(message[7:])
    fields = {"password": reader.wide(), "email": _ascii_field(reader)}
    flags, _blobs = read_won_data_objects(reader)
    fields["send_info"] = bool(flags.get(DBPROXY_SEND_INFO))
    return fields


def account_info_body(email, send_info):
    email = str(email).encode("ascii", "replace")[:255]
    return struct.pack("<B", len(email)) + email + won_data_flags({DBPROXY_SEND_INFO: send_info})


class DBProxyHandler(socketserver.BaseRequestHandler):
    """WON DBProxy: account lookup, the stock Create Account and Edit Account
    dialogs, and the forgotten login/password fetches."""

    def handle(self):
        emulator = self.server.emulator
        self.connection = next(emulator.connections)
        self.wrapped_key = None
        self.challenge = None
        self.request.settimeout(10)
        emulator.log.event("connected", service="dbproxy", connection=self.connection,
                           peer=list(self.client_address))
        try:
            while True:
                payload = recv_frame(self.request)
                if payload is None:
                    break
                reply = self.dispatch(payload)
                if reply is None:
                    emulator.log.event("unhandled", service="dbproxy", connection=self.connection,
                                       header=payload[0], payload_bytes=len(payload),
                                       payload_sha256=hashlib.sha256(payload).hexdigest())
                    break
                if reply:
                    self.request.sendall(frame(reply))
        except (OSError, ValueError, struct.error) as exc:
            emulator.log.event("connection_error", service="dbproxy", connection=self.connection,
                               error=str(exc))

    def dispatch(self, payload):
        """Return the reply, b"" to send nothing, or None for an unknown message."""
        emulator = self.server.emulator
        if payload[:1] == b"\xcb":
            return self.peer_auth(payload)
        if payload[:1] == bytes([woncrypto.WON_PEER_ENCRYPTED]):
            return self.peer_message(payload)
        header, service, message = struct.unpack_from("<BHH", payload)
        if (header, service) != (5, DBPROXY_SERVICE):
            return None
        if message == DBPROXY_KEY_REQUEST and emulator.dbproxy_key:
            key = woncrypto.public_key(emulator.dbproxy_key)
            return struct.pack("<BHHHhHBH", 5, DBPROXY_SERVICE, 3, 0, 0, 0, 0x80, len(key)) + key
        if message == DBPROXY_SESSION_KEY and emulator.dbproxy_key:
            reader = Reader(payload[7:])
            self.wrapped_key = reader.take(reader.number("<H"))
            return b""
        if message == DBPROXY_ENCRYPTED and self.wrapped_key is not None:
            _key, request = woncrypto.decrypt_won_login_data(
                emulator.dbproxy_key, self.wrapped_key, payload[7:])
            return self.account_request(request, encrypted=True)
        if message == DBPROXY_REQUEST:
            return self.account_request(payload)
        return None

    def account_request(self, message, encrypted=False, peer=None):
        emulator = self.server.emulator
        header, service, request, sub = struct.unpack_from("<BHHH", message)
        if (header, service, request) != (5, DBPROXY_SERVICE, DBPROXY_REQUEST):
            return None
        this_machine = from_this_machine(self.request, self.client_address)
        body = b""
        if sub == DBPROXY_ACCOUNT_LOOKUP:
            username = Reader(message[7:]).wide()
            account_found = emulator.account_exists(username)
            status, text = (0, "") if account_found else (-1, "User not found")
            if account_found:
                body = struct.pack("<I", emulator.auth_user_id(username))
            emulator.log.event("dbproxy_account_lookup", connection=self.connection,
                               account_found=account_found, status=status)
        elif sub == DBPROXY_CREATE_ACCOUNT and encrypted:
            status, text = emulator.create_stock_account(
                parse_dbproxy_create_account(message), this_machine)
        elif sub == DBPROXY_ACCOUNT_INFO and peer:
            info = emulator.community.account_info(peer["username"]) or {}
            status, text = 0, ""
            body = account_info_body(info.get("email", ""), info.get("send_info", 0))
        elif sub == DBPROXY_UPDATE_ACCOUNT and peer:
            status, text = emulator.update_stock_account(peer, parse_dbproxy_update_account(message))
        elif sub == DBPROXY_PASSWORD_EMAIL:
            username = Reader(message[7:]).wide()
            # No mail is ever sent; say whether the name exists, nothing more.
            status = (DBPROXY_NO_MODULE if emulator.account_exists(username)
                      else DBPROXY_USER_DOES_NOT_EXIST)
            text = ""
        elif sub == DBPROXY_LOGIN_NAME_EMAIL:
            status, text = DBPROXY_NO_MODULE, ""
        else:
            return None
        emulator.log.event("dbproxy_account_request", connection=self.connection, sub=sub,
                           status=status, encrypted=encrypted or peer is not None)
        return dbproxy_reply(DBPROXY_REQUEST, sub + 1, status, text, body)

    def peer_auth(self, payload):
        """Peer authentication with the Auth1 session of this client's address."""
        emulator = self.server.emulator
        _type, subtype = struct.unpack_from("<II", payload)
        session = emulator.auth_session_for_peer(self.client_address[0])
        if session is None or not emulator.dbproxy_key:
            return None
        client_key = woncrypto.auth1_login_material(session["auth1_reply"])["client_key"]
        if subtype == 0x32:
            # The challenge becomes this session's Blowfish key.
            self.challenge = secrets.token_bytes(8)
            now = int(time.time())
            certificate = woncrypto.auth1_certificate_for(
                emulator.keys["auth"], emulator.dbproxy_key, 1, now, now + 86400)
            return woncrypto.won_peer_challenge(client_key, self.challenge, certificate)
        if subtype == 0x34 and self.challenge:
            secret = woncrypto.won_peer_response_secret(payload, emulator.dbproxy_key, self.challenge)
            emulator.open_peer_session(self.client_address[0], self.challenge, session)
            emulator.log.event("dbproxy_peer_authenticated", connection=self.connection)
            return woncrypto.won_peer_complete(client_key, secret)
        return None

    def peer_message(self, payload):
        emulator = self.server.emulator
        peer = emulator.peer_session(self.client_address[0])
        if peer is None:
            return None
        session_id, sequence, message = woncrypto.decrypt_won_peer_message(peer["key"], payload)
        with emulator.auth_session_lock:
            if sequence <= peer["client_sequence"]:
                raise ValueError("Out-of-order DBProxy peer message")
            peer["client_sequence"] = sequence
            peer["server_sequence"] += 1
            reply_sequence = peer["server_sequence"]
        reply = self.account_request(message, peer=peer)
        if reply is None:
            return None
        return woncrypto.encrypt_won_peer_message(peer["key"], session_id, reply_sequence, reply)


# Chat commands relayed to an upstream IRC network when a session's chat
# backend is remote (TribesNext). DBProxy, the WON certificate handshake, and
# keepalive PINGs stay with wonemu.
IRC_RELAY_COMMANDS = frozenset({"JOIN", "PART", "PRIVMSG", "NOTICE", "NAMES", "TOPIC", "WHO",
                                "WHOIS", "AWAY", "MODE", "LIST", "INVITE", "KICK", "ISON"})
# Upstream registration and MOTD numerics the game has no use for after its own login.
IRC_RELAY_HIDDEN = frozenset({"001", "002", "003", "004", "005", "250", "251", "252", "253", "254",
                              "255", "265", "266", "372", "375", "376", "422"})


def irc_nickname(username):
    """A player's nick on a public IRC network, as TribesNext's client fix forms it."""
    nick = str(username).replace(" ", "_")
    return "".join(c for c in nick if c not in "~@#$!+%/|^{&*()<>")[:30] or "Tribes2Player"


class IRCRelay:
    """One player's connection to an upstream IRC network, mirrored to the game."""

    def __init__(self, handler, address, nickname, ident):
        self.handler = handler
        self.address = address
        self.nickname = nickname
        self.ident = ident
        self.registered = False
        self.closed = False
        self.pending = []
        self.lock = threading.Lock()
        self.socket = None
        self.endpoint = parse_endpoint(address)
        # Connect on the relay thread so a slow chat server never stalls the
        # game's login; client lines queue until registration.
        threading.Thread(target=self._run, daemon=True).start()

    def send(self, line):
        with self.lock:
            if not self.closed and self.socket is not None:
                self.socket.sendall(line.encode("utf-8") + b"\r\n")

    def relay(self, line):
        """Send a client chat line upstream, queueing it until registration."""
        with self.lock:
            if not self.registered:
                if len(self.pending) < 50:
                    self.pending.append(line)
                return
        self.send(line)

    def close(self):
        with self.lock:
            if self.closed:
                return
            self.closed = True
            sock = self.socket
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    def _run(self):
        buffer = b""
        nick_attempts = 0
        try:
            sock = socket.create_connection(self.endpoint, timeout=10)
            sock.settimeout(None)
            with self.lock:
                if self.closed:
                    sock.close()
                    return
                self.socket = sock
            self.send(f"NICK {self.nickname}")
            self.send(f"USER {self.ident} 0 * :Tribes 2 player")
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                buffer += data
                if len(buffer) > 65536:
                    raise ValueError("Upstream IRC line exceeded limit")
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    raw = raw.rstrip(b"\r")
                    text = raw.decode("utf-8", "replace")
                    parts = text.split(" ")
                    command = parts[1] if text.startswith(":") and len(parts) > 1 else parts[0]
                    if command == "PING":
                        self.send("PONG " + text.split(" ", 1)[1] if " " in text else "PONG")
                        continue
                    if command == "433" and not self.registered and nick_attempts < 3:
                        nick_attempts += 1
                        self.nickname = (self.nickname + "_")[:30]
                        self.send(f"NICK {self.nickname}")
                        continue
                    if command == "001":
                        with self.lock:
                            self.registered = True
                            pending, self.pending = self.pending, []
                        upstream_nick = parts[2] if len(parts) > 2 else self.nickname
                        if upstream_nick != self.handler.relay_nickname:
                            self.handler.send_raw(f":{self.handler.relay_nickname}!{self.ident}@localhost "
                                                  f"NICK :{upstream_nick}\r\n".encode())
                            self.handler.relay_nickname = upstream_nick
                        for line in pending:
                            self.send(line)
                    if command in IRC_RELAY_HIDDEN:
                        continue
                    self.handler.send_raw(raw + b"\r\n")
        except (OSError, ValueError):
            pass
        finally:
            was_open = not self.closed
            self.close()
            if was_open:
                self.handler.relay_lost(self)


class IRCHandler(socketserver.BaseRequestHandler):
    """Handle T2's certificate/challenge IRC login exchange."""

    def handle(self):
        emulator = self.server.emulator
        connection = next(emulator.connections)
        peer_ip = self.client_address[0]
        self.request.settimeout(30)
        emulator.log.event("connected", service="irc", connection=connection,
                           peer=[peer_ip, self.client_address[1]])
        session = emulator.auth_session_for_peer(peer_ip)
        if session is None:
            self._send("ERROR :Authentication required\r\n")
            emulator.log.event("irc_rejected", connection=connection, reason="no_auth_session")
            return

        buffer = bytearray()
        self.send_lock = threading.Lock()
        self.relay = None
        self.dbproxy_chunks = {}
        self.rejected_dbproxy_queries = set()
        challenge_sent = False
        pending_ping = None
        ping_sent_at = None
        try:
            while True:
                try:
                    chunk = self.request.recv(4096)
                except TimeoutError:
                    if not session.get("irc_authenticated"):
                        raise
                    if pending_ping is not None:
                        if time.monotonic() - ping_sent_at >= 30:
                            emulator.log.event("irc_idle_timeout", connection=connection)
                            return
                    else:
                        pending_ping = f"{connection}-{time.time_ns()}"
                        ping_sent_at = time.monotonic()
                        self._send(f"PING :{pending_ping}\r\n")
                        emulator.log.event("irc_keepalive_sent", connection=connection)
                    continue
                if not chunk:
                    return
                buffer.extend(chunk)
                if len(buffer) > 65536:
                    raise ValueError("IRC input buffer exceeded limit")
                while b"\r\n" in buffer:
                    line, _, remainder = buffer.partition(b"\r\n")
                    buffer = bytearray(remainder)
                    decoded_line = line.decode("utf-8", "replace")
                    command, _, arguments = decoded_line.partition(" ")
                    if command.upper() == "PONG" and pending_ping is not None and \
                            arguments.lstrip(":") == pending_ping:
                        pending_ping = None
                        ping_sent_at = None
                        emulator.log.event("irc_keepalive_ack", connection=connection)
                    self._handle_line(decoded_line, session,
                                      connection, challenge_sent)
                    if line.upper().startswith(b"CERT "):
                        challenge_sent = True
        except (OSError, ValueError) as exc:
            emulator.log.event("connection_error", service="irc", connection=connection,
                               error=str(exc))
        finally:
            if self.relay is not None:
                self.relay.close()
            if session.get("presence_registered"):
                emulator.update_irc_presence(
                    emulator.auth_user_id(session["username"]), connection, False)

    def _send(self, line):
        self.send_raw(line.encode("utf-8"))

    def send_raw(self, data):
        # The relay thread writes to the same client socket.
        lock = getattr(self, "send_lock", None)
        if lock is None:
            self.request.sendall(data)
            return
        with lock:
            self.request.sendall(data)

    def start_chat_relay(self, session, connection):
        chat = session.get("chat")
        if not chat or chat[0] != "tribesnext":
            return
        emulator = self.server.emulator
        self.relay_nickname = session["irc_nickname"]
        try:
            self.relay = IRCRelay(self, chat[1], irc_nickname(session["username"]), session["irc_ident"])
        except ValueError as exc:
            emulator.log.event("irc_relay_unavailable", connection=connection, server=chat[1],
                               error=str(exc))
            return
        emulator.log.event("irc_relay_started", connection=connection, server=chat[1])

    def relay_lost(self, relay):
        if getattr(self, "relay", None) is not relay:
            return
        self.relay = None
        self.server.emulator.log.event("irc_relay_closed", server=relay.address,
                                       registered=relay.registered)
        state = "Disconnected from" if relay.registered else "Could not reach"
        try:
            self._send(f":wonemu.local NOTICE {self.relay_nickname} :{state} TribesNext chat "
                       f"({relay.address}); using local chat\r\n")
        except OSError:
            pass

    def _send_dbmsg(self, query_id, body):
        prefix = f":wonemu.local DBMSG {query_id} 0 :"
        max_payload = min(400, 510 - len(prefix.encode("utf-8")))
        if max_payload < 1:
            raise ValueError("DBProxy query ID is too long for an IRC reply")
        chunks = []
        current = []
        current_bytes = 0
        for character in body:
            encoded_length = len(character.encode("utf-8"))
            if current and current_bytes + encoded_length > max_payload:
                chunks.append("".join(current))
                current = []
                current_bytes = 0
            current.append(character)
            current_bytes += encoded_length
        if current or not chunks:
            chunks.append("".join(current))
        for index, chunk in enumerate(chunks):
            last_packet = int(index == len(chunks) - 1)
            self._send(f":wonemu.local DBMSG {query_id} {last_packet} :{chunk}\r\n")

    def _handle_line(self, line, session, connection, challenge_sent):
        emulator = self.server.emulator
        command, _, arguments = line.partition(" ")
        command = command.upper()
        if command == "DBQA":
            if not session.get("irc_authenticated"):
                # Initial DBQA chunks carry the native IRC certificate. Auth1
                # already bound this socket's account, so keep no query state.
                emulator.log.event("irc_dbproxy_chunk", connection=connection,
                                   phase="pre_auth_identity", bytes=len(arguments))
                return
            header, separator, chunk = arguments.partition(":")
            fields = header.split()
            if not separator or not fields:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="malformed_dbproxy_chunk")
                return
            try:
                query_id = int(fields[0])
            except ValueError:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="malformed_dbproxy_chunk_id")
                return
            pending = self.dbproxy_chunks.get(query_id, "")
            if len(pending) + len(chunk) > 65536:
                self.dbproxy_chunks.pop(query_id, None)
                self.rejected_dbproxy_queries.add(query_id)
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="dbproxy_chunk_limit", query_id=query_id)
                return
            if len(self.dbproxy_chunks) >= 64 and query_id not in self.dbproxy_chunks:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="dbproxy_pending_query_limit", query_id=query_id)
                return
            self.dbproxy_chunks[query_id] = pending + chunk
            emulator.log.event("irc_dbproxy_chunk", connection=connection,
                               query_id=query_id, bytes=len(chunk),
                               accumulated_bytes=len(pending) + len(chunk))
            return
        if command == "DBQAX":
            header, separator, final_chunk = arguments.partition(":")
            fields = header.split()
            if separator and fields:
                try:
                    query_id = int(fields[0])
                except ValueError:
                    query_id = None
                if query_id is not None:
                    if query_id in self.rejected_dbproxy_queries:
                        self.rejected_dbproxy_queries.discard(query_id)
                        self._send_dbmsg(query_id,
                                         "1\tDBProxy query exceeds size limit\\S0\\S")
                        return
                    pending = self.dbproxy_chunks.pop(query_id, "")
                    if len(pending) + len(final_chunk) > 65536:
                        self._send_dbmsg(query_id,
                                         "1\tDBProxy query exceeds size limit\\S0\\S")
                        return
                    arguments = header + ":" + pending + final_chunk
            self._handle_dbqax(arguments, session, connection)
            return
        if command == "DBQC":
            try:
                query_id = int(arguments.strip())
            except ValueError:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="malformed_dbproxy_cancel")
                return
            self.dbproxy_chunks.pop(query_id, None)
            self.rejected_dbproxy_queries.discard(query_id)
            emulator.log.event("irc_dbproxy_cancel", connection=connection, query_id=query_id)
            return
        arguments = arguments.strip()
        if command == "CERT":
            if challenge_sent:
                return
            token = arguments.lstrip(":")
            try:
                message = woncrypto.parse_won_irc_token(token)
            except ValueError as exc:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="invalid_certificate_token", error=str(exc))
                self._send("ERROR :Invalid certificate\r\n")
                return
            if message["subtype"] != 0x32:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="unexpected_certificate_subtype",
                                   subtype=message["subtype"])
                self._send("ERROR :Invalid certificate\r\n")
                return
            challenge = secrets.token_bytes(8)
            session["irc_challenge"] = challenge
            token = woncrypto.build_won_irc_cb33(session["auth1_reply"], challenge)
            nickname = session["username"].replace(" ", "_")
            self._send(f":wonemu.local CHALLENGE {token}\r\n")
            emulator.log.event("irc_challenge", connection=connection,
                               token_subtype=0x33, token_bytes=len(token),
                               challenge_bytes=len(challenge))
            return
        if command == "CHALRESP":
            token = arguments.lstrip(":")
            try:
                message = woncrypto.parse_won_irc_token(token)
            except ValueError as exc:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="invalid_challenge_response", error=str(exc))
                self._send("ERROR :Invalid challenge response\r\n")
                return
            if message["subtype"] != 0x34 or len(message["payload"]) < 2:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="unexpected_challenge_response",
                                   subtype=message["subtype"])
                self._send("ERROR :Invalid challenge response\r\n")
                return
            encrypted_length, = struct.unpack_from("<H", message["payload"])
            if encrypted_length > len(message["payload"]) - 2:
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="truncated_challenge_response")
                self._send("ERROR :Invalid challenge response\r\n")
                return
            encrypted = message["payload"][2:2 + encrypted_length]
            key_width = (emulator.keys["auth"]["p"].bit_length() + 7) // 8
            if (len(encrypted) < 4 or
                    struct.unpack_from("<I", encrypted)[0] == 0 or
                    len(encrypted) != 4 + struct.unpack_from("<I", encrypted)[0] * 2 * key_width):
                emulator.log.event("irc_rejected", connection=connection,
                                   reason="invalid_challenge_ciphertext")
                self._send("ERROR :Invalid challenge response\r\n")
                return
            if not woncrypto.verify_won_irc_cb34(
                    token, session["client_key"], session["session_key"]):
                response_secret = None
                proof_key = "none"
                for key_name, key in (("auth", emulator.keys["auth"]),
                                      ("client", session["client_key"])):
                    try:
                        response_secret = woncrypto.extract_won_irc_cb34_response(
                            token, key, session.get("irc_challenge", b""))
                    except ValueError:
                        response_secret = None
                    if response_secret:
                        proof_key = key_name
                        break
                # The Auth1 response has already validated the password and
                # bound this socket's loopback peer to that session. Legacy T2
                # builds differ in their CB34 plaintext layout, so keep IRC
                # compatibility local to the Auth1-authenticated connection.
                emulator.log.event("irc_cb34_compat_fallback", connection=connection,
                                   encrypted_bytes=len(encrypted), token_bytes=len(token),
                                   proof_key=proof_key,
                                   response_secret_bytes=len(response_secret or b""))
            else:
                response_secret = session["session_key"]
            token = woncrypto.build_won_irc_cb35(
                session["auth1_reply"], response_secret or session["session_key"])
            nickname = session["username"].replace(" ", "_")
            ident = str(emulator.auth_user_id(session["username"]))
            self._send(f":wonemu.local CHALRESP_REPLY {nickname} {ident} {token}\r\n")
            session["irc_authenticated"] = True
            session["irc_nickname"] = nickname
            session["irc_ident"] = ident
            emulator.update_irc_presence(
                emulator.auth_user_id(session["username"]), connection, True)
            session["presence_registered"] = True
            emulator.log.event("irc_authenticated", connection=connection,
                               token_bytes=len(token))
            self.start_chat_relay(session, connection)
            return
        if command == "PING":
            self._send(f":wonemu.local PONG :{arguments.lstrip(':')}\r\n")
            return
        relay = getattr(self, "relay", None)
        if relay is not None and session.get("irc_authenticated"):
            if command in IRC_RELAY_COMMANDS:
                relay.relay(line)
                return
            if command == "QUIT":
                relay.relay(line)
                relay.close()
        if command == "JOIN" and session.get("irc_authenticated"):
            channel = arguments.split(" ", 1)[0]
            if channel:
                nickname = session["irc_nickname"]
                prefix = f":{nickname}!{session['irc_ident']}@localhost"
                self._send(f"{prefix} JOIN :{channel}\r\n")
                self._send(f":wonemu.local 353 {nickname} = {channel} :{nickname}\r\n")
                self._send(f":wonemu.local 366 {nickname} {channel} :End of /NAMES list.\r\n")
                self._send(f":wonemu.local 331 {nickname} {channel} :No topic is set\r\n")
        elif command == "MODE" and session.get("irc_authenticated"):
            channel = arguments.split(" ", 1)[0]
            if channel.startswith(("#", "&")):
                self._send(f":wonemu.local 324 {session['irc_nickname']} {channel} +nt\r\n")
        elif command == "PART" and session.get("irc_authenticated"):
            channel = arguments.split(" ", 1)[0]
            if channel:
                prefix = f":{session['irc_nickname']}!{session['irc_ident']}@localhost"
                self._send(f"{prefix} PART {channel}\r\n")
        elif command == "PRIVMSG" and session.get("irc_authenticated"):
            target, separator, message = arguments.partition(" ")
            if target and separator:
                prefix = f":{session['irc_nickname']}!{session['irc_ident']}@localhost"
                self._send(f"{prefix} PRIVMSG {target} {message}\r\n")
        elif command == "QUIT":
            self._send(":wonemu.local ERROR :Closing connection\r\n")

    def _handle_dbqax(self, arguments, session, connection):
        emulator = self.server.emulator
        if not session.get("irc_authenticated"):
            emulator.log.event("irc_rejected", connection=connection,
                               reason="dbproxy_before_irc_auth")
            return
        header, separator, query = arguments.partition(":")
        fields = header.split()
        if not separator or len(fields) < 2:
            emulator.log.event("irc_rejected", connection=connection,
                               reason="malformed_dbproxy_query")
            return
        try:
            query_id = int(fields[0])
            operation = fields[1]
            array_query = operation.startswith("C")
            if array_query:
                operation = operation[1:]
            ordinal = int(operation)
        except ValueError:
            emulator.log.event("irc_rejected", connection=connection,
                               reason="malformed_dbproxy_query")
            return

        if (session_community(session) == "tribesnext" and
                (ordinal, array_query) not in TRIBESNEXT_ALLOWED_QUERIES):
            self._send_dbmsg(query_id,
                             "1\tTribesNext community operation unavailable\\S0\\S")
            emulator.log.event("irc_dbproxy_query", connection=connection, query_id=query_id,
                               ordinal=ordinal, array_query=array_query,
                               argument_characters=len(query), supported=False,
                               status=1, row_count=0)
            return

        arguments = query.replace("\\n", "\n").replace("\\\\", "\\")
        try:
            max_rows = int(fields[2]) if len(fields) > 2 else 100
        except ValueError:
            max_rows = 100
        limit = 100 if max_rows <= 0 else min(max_rows, 100)
        dbproxy_query = DBProxyQuery(emulator, session, connection, arguments, fields,
                                     ordinal, array_query, limit)
        status, result, status_code, supported = dbproxy_query.run()
        rows = dbproxy_query.rows

        def escape(value):
            # The client unescapes slash pairs before splitting on the reserved \S token.
            value = value.replace("\r", "").replace("\0", "")
            value = value.replace("\\S", "\\s")
            return value.replace("\\", "\\\\").replace("\n", "\\n")

        body = "\\S".join([escape(status), escape(result)] + [escape(row) for row in rows]) + "\\S"
        self._send_dbmsg(query_id, body)
        emulator.log.event("irc_dbproxy_query", connection=connection, query_id=query_id,
                           ordinal=ordinal, array_query=array_query,
                           argument_characters=len(arguments), supported=supported,
                           status=status_code, row_count=len(rows))


class DBProxyQuery:
    """One authenticated DBProxy query, dispatched on ``(ordinal, array_query)``."""

    tribesnext_clan_not_found = object()
    tribesnext_player_not_found = object()

    def __init__(self, emulator, session, connection, arguments, fields, ordinal,
                 array_query, limit):
        self.emulator = emulator
        self.session = session
        self.connection = connection
        self.arguments = arguments
        self.fields = fields
        self.ordinal = ordinal
        self.array_query = array_query
        self.limit = limit
        self.rows = []
        self.robot = emulator.tribesnext_robot
        self.credentials = session.get("tribesnext_credentials")
        self.own_guid = None

    def run(self):
        """Return ``(status, result, status_code, supported)`` for this query."""
        key = (self.ordinal, self.array_query)
        if (session_community(self.session) == "tribesnext" and
                key in self.ROBOT_HANDLERS):
            try:
                if self.credentials is None:
                    raise tribesnext.RobotAPIError("TribesNext session credentials unavailable")
                self.own_guid = self.credentials.certificate.guid
                return (*self.ROBOT_HANDLERS[key](self), True)
            except (OSError, ValueError, tribesnext.RobotAPIError) as exc:
                self.emulator.log.event("tribesnext_robot_unavailable",
                                        connection=self.connection, ordinal=self.ordinal,
                                        array_query=self.array_query,
                                        error=type(exc).__name__)
                self.rows.clear()
                return "1\tTribesNext service unavailable", "0", 1, True
        handler = self.LOCAL_HANDLERS.get(key)
        if handler is None:
            return f"1\tUnsupported DBProxy query {self.ordinal}", "", 1, False
        return (*handler(self), True)

    def require_tribe_admin(self, tribe_name, minimum_access=2):
        membership = self.emulator.community.membership(tribe_name, self.session["username"])
        if membership is None or membership["access"] < minimum_access:
            raise ValueError("Tribe administrator access required")
        return membership

    def require_forum_access(self, forum_id):
        forum = self.emulator.community.ensure_forum(forum_id)
        if forum["tribe_id"]:
            tribe = self.emulator.community.tribe_by_id(forum["tribe_id"])
            if tribe is None or self.emulator.community.membership(
                    tribe["name"], self.session["username"]) is None:
                raise ValueError("Tribe membership required for this forum")
        return forum

    def require_forum_moderator(self, forum_id):
        forum = self.require_forum_access(forum_id)
        if not forum["tribe_id"]:
            raise ValueError("Forum moderator access required")
        tribe = self.emulator.community.tribe_by_id(forum["tribe_id"])
        membership = (self.emulator.community.membership(tribe["name"], self.session["username"])
                      if tribe else None)
        if membership is None or membership["access"] < 3:
            raise ValueError("Tribe moderator access required")
        return forum

    def mail_row(self, message):
        # Quads follow the stock getTextName order: name, tag, append, id.
        sender_quad = (message["sender_name"], message["sender_tag"],
                       str(message["sender_appending"]), str(message["sender_id"]))
        recipient_quad = (message["recipient_name"], message["recipient_tag"],
                          str(message["recipient_appending"]), str(message["recipient_id"]))
        body_count = max(1, len(message["body"].splitlines()))
        created = datetime.datetime.fromtimestamp(message["created_at"]).strftime(
            "%m/%d/%Y %H:%M")
        return "\t".join((str(message["mail_id"]), *sender_quad, *recipient_quad,
                           created, str(message["is_cc"]),
                           str(message["is_deleted"]), str(message["is_read"]),
                           message["to_list"], message["cc_list"], message["subject"],
                           str(body_count), message["body"]))

    def resolve_profile(self, username):
        username = str(username).strip()
        profile = self.emulator.community.get_profile(username)
        if profile is None and username.isdecimal():
            profile = self.emulator.community.get_profile_by_user_id(int(username))
        if profile is None and (self.emulator.community.local_account_exists(username) or
                                woncrypto.private_store_contains(self.emulator.private_store, username)):
            profile = self.emulator.community.ensure_profile(
                username, self.emulator.auth_user_id(username))
        return profile

    def resolve_profile_reference(self, reference):
        reference = str(reference).strip()
        profile = self.resolve_profile(reference) if reference else None
        if profile is None and (not reference or reference.isdecimal()):
            # Some legacy profile tabs pass a numeric profile metadata field
            # rather than the account name/ID. Reuse the profile most
            # recently returned on this authenticated IRC session.
            last_profile = self.session.get("last_profile_username")
            fallback = last_profile or self.session["username"]
            profile = self.emulator.community.get_profile(
                fallback, self.emulator.auth_user_id(fallback))
            if profile is not None:
                self.emulator.log.event(
                    "irc_profile_reference_fallback", connection=self.connection,
                    target_characters=len(reference), target_numeric=reference.isdecimal(),
                    profile_found=True)
        return profile

    def tribesnext_profile(self, reference):
        if session_community(self.session) != "tribesnext":
            return None
        credentials = self.session.get("tribesnext_credentials")
        if credentials is None:
            return False
        # Searched players are not in the local public store; tribesnext_guid
        # falls back to the robot's exact-name search.
        guid = self.tribesnext_guid(reference)
        if guid is False:
            return False
        if guid is None:
            # With native community also checked, an unknown TribesNext name
            # may be a native player: resolve it locally instead.
            if self.emulator.merges_native_community(self.session):
                return None
            return self.tribesnext_player_not_found
        try:
            profile = self.emulator.tribesnext_robot.user_view(credentials, guid)
        except (OSError, ValueError, tribesnext.RobotAPIError) as exc:
            self.emulator.log.event("tribesnext_profile_unavailable", connection=self.connection,
                                    guid=guid, error=type(exc).__name__)
            return False
        profile["guid"] = guid
        return profile

    def tribesnext_search(self, method, search):
        if session_community(self.session) != "tribesnext":
            return None
        credentials = self.session.get("tribesnext_credentials")
        if credentials is None:
            return False
        try:
            return method(credentials, search)
        except (OSError, ValueError, tribesnext.RobotAPIError) as exc:
            self.emulator.log.event("tribesnext_search_unavailable", connection=self.connection,
                                    search_characters=len(search), error=type(exc).__name__)
            return False

    def tribesnext_clan(self, reference):
        if session_community(self.session) != "tribesnext":
            return None
        credentials = self.session.get("tribesnext_credentials")
        if credentials is None:
            return False
        reference = str(reference).strip()
        try:
            if reference.isdecimal():
                clan_id = reference
            else:
                matches = self.emulator.tribesnext_robot.clan_search(credentials, reference)
                match = next((clan for clan in matches
                              if clan["name"].casefold() == reference.casefold()), None)
                if match is None:
                    if self.emulator.merges_native_community(self.session):
                        return None
                    return self.tribesnext_clan_not_found
                clan_id = match["id"]
            return self.emulator.tribesnext_robot.clan_view(credentials, clan_id)
        except (OSError, ValueError, tribesnext.RobotAPIError) as exc:
            self.emulator.log.event("tribesnext_clan_unavailable", connection=self.connection,
                                    reference_characters=len(reference),
                                    error=type(exc).__name__)
            return False

    def known_account_names(self):
        return ({name.casefold() for name in
                 woncrypto.private_store_usernames(self.emulator.private_store)} |
                {name.casefold() for name in self.emulator.community.local_account_usernames()} |
                {self.session["username"].casefold()})

    def tribesnext_guid(self, reference):
        """Resolve a player name or GUID; None when unknown, False when unavailable."""
        reference = str(reference).strip()
        if reference.isdecimal():
            return int(reference)
        guid = self.emulator.tribesnext_user_id(reference)
        if guid is not None:
            return guid
        matches = self.tribesnext_search(self.emulator.tribesnext_robot.user_search, reference)
        if matches is False:
            return False
        match = next((player for player in matches or ()
                      if player["name"].casefold() == reference.casefold()), None)
        return int(match["guid"]) if match else None

    # TribesNext robot translations (sessions whose community backend is TribesNext).

    def tn_set_picture(self):
        graphic = self.arguments.strip()
        if len(graphic) > 255 or any(character in graphic for character in "\t\r\n"):
            status, result, status_code = "1\tInvalid picture", "", 1
        else:
            self.emulator.community.set_tribesnext_picture(self.own_guid, graphic)
            status, result, status_code = "0\tUpdated", "", 0
        return status, result, status_code

    def tn_history(self):
        target = self.arguments.strip()
        guid = (self.own_guid if not target or
                target.casefold() == self.session["username"].casefold()
                else self.tribesnext_guid(target))
        if guid is False:
            raise tribesnext.RobotAPIError("TribesNext player lookup failed")
        if guid is None:
            status, result, status_code = "1\tPlayer not found", "0", 1
        else:
            # The robot lists oldest first; the Browser shows newest first.
            events = self.robot.user_history(self.credentials, guid)[::-1][:self.limit]
            for event in events:
                try:
                    when = datetime.datetime.fromtimestamp(
                        int(event["time"])).strftime("%m/%d/%Y %H:%M")
                except (ValueError, OverflowError, OSError):
                    when = ""
                self.rows.append(f"{when} {tribesnext.expand_history_event(event)}"
                                 .strip())
            status, result, status_code = "0\t", str(len(self.rows)), 0
        return status, result, status_code

    def tn_buddy_list(self):
        target = self.arguments.strip()
        # The robot only exposes the authenticated account's own list.
        buddies = ([] if target and target.casefold() != self.session["username"].casefold()
                   else self.robot.buddy_list(self.credentials)[:self.limit])
        self.rows.extend("\t".join((buddy["name"], buddy["tag"], buddy["append"],
                                     buddy["guid"], "")) for buddy in buddies)
        status, result, status_code = "0\t", str(len(self.rows)), 0
        return status, result, status_code

    def tn_clear_buddies(self):
        for buddy in self.robot.buddy_list(self.credentials):
            self.robot.remove_buddy(self.credentials, buddy["guid"])
        status, result, status_code = "0\tBuddy list cleared", "", 0
        return status, result, status_code

    def tn_tribe_options(self):
        # Tribe OPTIONS: the client draws the page itself once the query succeeds.
        status, result, status_code = "0\t", "0", 0
        return status, result, status_code

    def tn_answer_invite(self):
        query_fields = self.arguments.split("\t")
        action = query_fields[0].strip().casefold()
        tribe = query_fields[1].strip() if len(query_fields) > 1 else ""
        if action not in {"accept", "reject"}:
            status, result, status_code = (
                "1\tTribesNext invitations cannot be retracted here", "", 1)
        else:
            # Invite mail links carry the clan ID; other links carry the name.
            clan_id = (int(tribe) if tribe.isdecimal() else next(
                (int(clan["id"]) for clan in self.robot.clan_search(self.credentials, tribe)
                 if clan["name"].casefold() == tribe.casefold()), None))
            if clan_id is None:
                status, result, status_code = "1\tTribe not found", "", 1
            else:
                try:
                    self.robot.answer_invite(self.credentials, clan_id, action == "accept")
                    status, result, status_code = "0\tInvitation updated", "", 0
                except tribesnext.RobotAPIError as exc:
                    status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def tn_change_buddy(self):
        buddy_name = self.arguments.strip()
        if self.ordinal == 11:
            listed = next((buddy for buddy in self.robot.buddy_list(self.credentials)
                           if buddy["name"].casefold() == buddy_name.casefold()),
                          None)
            guid = int(listed["guid"]) if listed else None
        else:
            guid = self.tribesnext_guid(buddy_name)
        if guid is False:
            raise tribesnext.RobotAPIError("TribesNext player lookup failed")
        if guid is None:
            status, result, status_code = (
                f"1\t{'Player' if self.ordinal == 10 else 'Buddy'} not found", "", 1)
        elif self.ordinal == 10:
            self.robot.add_buddy(self.credentials, guid)
            # The Browser reads the added name from result field 2.
            status, result, status_code = "0\tBuddy added", f"\t\t{buddy_name}", 0
        else:
            self.robot.remove_buddy(self.credentials, guid)
            status, result, status_code = "0\tBuddy removed", "", 0
        return status, result, status_code

    def tn_mailbox(self):
        # Stock T-Mail caches whole messages, so every new header needs a
        # robot read for its body (which also marks it read on TribesNext).
        if self.ordinal == 1:
            after = self.arguments.strip()
            after_id = int(after) if after.isdecimal() else 0
            headers = [header for header in self.robot.mailbox(
                self.credentials, "inbox", 0, TRIBESNEXT_MAIL_PAGE, since=after_id + 1)
                if header["id"] > after_id]
        else:
            headers = self.robot.mailbox(self.credentials, "deleted", 0, TRIBESNEXT_MAIL_PAGE)
        recipient = (self.session["username"], "", "0", str(self.own_guid))
        # The client advances its last-seen ID row by row, so send oldest first.
        for header in sorted(headers, key=lambda header: header["id"])[:self.limit]:
            self.rows.append(tribesnext_mail_row(
                header, self.robot.read_message(self.credentials, header["id"]), recipient))
        status = ("0\t\tYou have no deleted messages." if self.ordinal == 14 and not self.rows
                  else "0\t")
        result, status_code = str(len(self.rows)), 0
        return status, result, status_code

    def tn_send_mail(self):
        query_fields = self.arguments.split("\t")
        if len(query_fields) < 4:
            status, result, status_code = "1\tInvalid mail request", "", 1
        else:
            recipients, missing = ([], []), None
            for field, guids in zip(query_fields[:2], recipients):
                for name in (name.strip() for name in field.split(",")):
                    if not name:
                        continue
                    guid = self.tribesnext_guid(name)
                    if guid is False:
                        raise tribesnext.RobotAPIError("TribesNext player lookup failed")
                    if guid is None:
                        missing = missing or name
                    else:
                        guids.append(guid)
            if missing:
                status, result, status_code = f"1\tPlayer not found: {missing}", "", 1
            elif not recipients[0]:
                status, result, status_code = "1\tA To recipient is required", "", 1
            else:
                try:
                    self.robot.send_mail(self.credentials, query_fields[2],
                                         "\t".join(query_fields[3:]), *recipients)
                    status, status_code = "0\tMail sent", 0
                    result = str(len(recipients[0]) + len(recipients[1]))
                except tribesnext.RobotAPIError as exc:
                    status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def tn_update_mail(self):
        message_id = self.arguments.strip()
        if not message_id.isdecimal():
            status, result, status_code = "1\tMail message not found", "", 1
        else:
            if self.ordinal == 6:
                self.robot.delete_message(self.credentials, int(message_id))
            elif self.ordinal == 7:
                self.robot.read_message(self.credentials, int(message_id))
            # 35 (remove from Deleted) has no robot method; TribesNext purges
            # deleted messages itself.
            status, result, status_code = "0\tUpdated", "", 0
        return status, result, status_code

    def tn_block_list(self):
        blocked = self.robot.player_list(self.credentials, "ignore")[:self.limit]
        self.rows.extend("\t".join((player["name"], player["tag"], player["append"],
                                     player["guid"], "0")) for player in blocked)
        status, result, status_code = "0\t", str(len(self.rows)), 0
        return status, result, status_code

    def tn_change_block(self):
        blocked_name = self.arguments.strip()
        if self.ordinal == 8:
            listed = next((player for player in self.robot.player_list(self.credentials, "ignore")
                           if player["name"].casefold() == blocked_name.casefold()),
                          None)
            guid = int(listed["guid"]) if listed else None
        else:
            guid = self.tribesnext_guid(blocked_name)
        if guid is False:
            raise tribesnext.RobotAPIError("TribesNext player lookup failed")
        if guid is None:
            status, result, status_code = (
                f"1\t{'Blocked sender' if self.ordinal == 8 else 'Player'} not found",
                "", 1)
        elif self.ordinal == 9:
            self.robot.add_list_player(self.credentials, "ignore", guid)
            status, result, status_code = "0\tSender blocked", "", 0
        else:
            self.robot.remove_list_player(self.credentials, "ignore", guid)
            status, result, status_code = "0\tBlock removed", "", 0
        return status, result, status_code

    # Local community backend.

    def inbox(self):
        try:
            after_id = int(self.arguments.strip() or "0")
            messages = self.emulator.community.inbox(self.session["username"], after_id, self.limit)
            status, status_code = "0\t", 0
            result = str(len(messages))
            for message in messages:
                self.rows.append(self.mail_row(message))
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "0", 1
        return status, result, status_code

    def update_mail(self):
        try:
            mail_id = int(self.arguments.strip())
            if not self.emulator.community.update_mail(
                    self.session["username"], mail_id,
                    is_deleted=True if self.ordinal == 6 else None,
                    is_read=True if self.ordinal == 7 else None):
                raise ValueError("Mail message not found")
            status, result, status_code = "0\tUpdated", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def send_mail(self):
        query_fields = self.arguments.split("\t")
        if len(query_fields) < 4:
            status, result, status_code = "1\tInvalid mail request", "", 1
        else:
            to_names = [name.strip() for name in query_fields[0].split(",") if name.strip()]
            cc_names = [name.strip() for name in query_fields[1].split(",") if name.strip()]
            recipients = [(name, False) for name in to_names]
            recipients.extend((name, True) for name in cc_names)
            try:
                known_names = self.known_account_names()
                for name, _ in recipients:
                    if name.casefold() not in known_names:
                        raise ValueError(f"Player not found: {name}")
                    self.emulator.community.ensure_profile(name, self.emulator.auth_user_id(name))
                count = self.emulator.community.send_mail(
                    self.session["username"], self.emulator.auth_user_id(self.session["username"]),
                    recipients, query_fields[2], "\t".join(query_fields[3:]),
                    ", ".join(to_names), ", ".join(cc_names))
                status, result, status_code = "0\tMail sent", str(count), 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def welcome(self):
        status, result, status_code = "0\t", "Welcome to the local Tribes 2 community server.", 0
        return status, result, status_code

    def news(self):
        query_fields = self.arguments.split("\t")
        try:
            if self.ordinal == 100:
                category_id = int(query_fields[-1]) if query_fields and query_fields[-1] else 105
                self.session["news_category"] = category_id
            else:
                category_id = int(self.session.get("news_category", 105))
            page = int(self.fields[2]) if len(self.fields) > 2 else 0
            page = max(0, min(page, 100))
            articles, total = self.emulator.community.list_news(category_id, page * 23, 23)
            status, result, status_code = f"0\t\t{total}\t0", str(total), 0
            for index, article in enumerate(articles):
                created = datetime.datetime.fromtimestamp(article["created_at"]).strftime(
                    "%m/%d/%Y %H:%M")
                row = [str(index), str(article["article_id"]), str(article["article_id"]),
                       "1", created, str(article["updated_at"]), str(article["author_id"]),
                       "", article["author_name"], str(article["author_id"]),
                       article["author_tag"], str(article["author_appending"]),
                       str(article["category_id"]), article["title"], article["body"]]
                self.rows.append("\t".join(row))
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "0", 1
        return status, result, status_code

    def post_news(self):
        query_fields = self.arguments.split("\t")
        try:
            if len(query_fields) < 3:
                raise ValueError("Invalid news post request")
            article_id = self.emulator.community.post_news(
                query_fields[0], self.session["username"],
                self.emulator.auth_user_id(self.session["username"]),
                query_fields[1], "\t".join(query_fields[2:]))
            status, result, status_code = "0\tArticle submitted", str(article_id), 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def edit_news(self):
        query_fields = self.arguments.split("\t")
        try:
            if len(query_fields) < 6 or not self.emulator.community.edit_news(
                    int(query_fields[1]), self.session["username"], int(query_fields[4]),
                    query_fields[5], "\t".join(query_fields[6:])):
                raise ValueError("Article not found or not owned by this account")
            status, result, status_code = "0\tArticle updated", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def block_list(self):
        blocked = self.emulator.community.email_block_list(self.session["username"])[:self.limit]
        status, result, status_code = "0\t", str(len(blocked)), 0
        self.rows.extend("\t".join((entry["username"], entry["tag"],
                                     str(entry["appending"]), str(entry["user_id"]),
                                     str(entry["blocked_count"]))) for entry in blocked)
        return status, result, status_code

    def buddy_list(self):
        target = self.arguments.strip() or self.session["username"]
        profile = self.resolve_profile_reference(target)
        if profile is None:
            status, result, status_code = "1\tPlayer not found", "0", 1
        else:
            buddies = self.emulator.community.list_buddies(profile["username"])[:self.limit]
            status, result, status_code = "0\t", str(len(buddies)), 0
            self.rows.extend("\t".join((buddy["display_name"], buddy["tag"],
                                         str(buddy["appending"]), str(buddy["user_id"]), ""))
                             for buddy in buddies)
        return status, result, status_code

    def tribe_options(self):
        tribe_name = self.arguments.strip()
        if self.emulator.community.get_tribe(tribe_name) is None:
            status, result, status_code = "1\tTribe not found", "0", 1
        else:
            # Tribe security-action rules are not persisted by this local
            # backend yet. Return a valid empty array instead of an
            # unsupported-query error for the tribe Options pane.
            status, result, status_code = "0\t", "0", 0
        return status, result, status_code

    def change_buddy(self):
        buddy_name = self.arguments.strip()
        try:
            known_names = self.known_account_names()
            if buddy_name.casefold() not in known_names:
                raise ValueError("Player not found")
            if self.ordinal == 10:
                self.emulator.community.add_buddy(
                    self.session["username"], self.emulator.auth_user_id(self.session["username"]),
                    buddy_name, self.emulator.auth_user_id(buddy_name))
                status, result = "0\tBuddy added", ""
            else:
                if not self.emulator.community.remove_buddy(self.session["username"], buddy_name):
                    raise ValueError("Buddy not found")
                status, result = "0\tBuddy removed", ""
            status_code = 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def tribe_invitations(self):
        tribe_name = self.arguments.strip()
        try:
            self.require_tribe_admin(tribe_name)
            invitations = self.emulator.community.list_invitations(tribe_name)[:self.limit]
            status, result, status_code = "0\t", str(len(invitations)), 0
            for invitation in invitations:
                if invitation["status"] == "requested":
                    inviter_quad = ("", "0", "", "0")
                else:
                    inviter_quad = (invitation["inviter_name"] or "",
                                    str(invitation["inviter_id"] or 0),
                                    invitation["inviter_tag"] or "",
                                    str(invitation["inviter_appending"] or 0))
                invitee_quad = (invitation["invited_name"], str(invitation["invited_id"]),
                                invitation["invited_tag"],
                                str(invitation["invited_appending"]))
                invite_date = datetime.datetime.fromtimestamp(
                    invitation["created_at"]).strftime("%m/%d/%Y %H:%M")
                self.rows.append("\t".join((str(invitation["invite_id"]), invite_date,
                                             *inviter_quad, *invitee_quad,
                                             str(int(invitation["status"] == "requested")), "0")))
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "0", 1
        return status, result, status_code

    def clear_buddies(self):
        self.emulator.community.clear_buddies(self.session["username"])
        status, result, status_code = "0\tBuddy list cleared", "", 0
        return status, result, status_code

    def deleted_mail(self):
        messages = self.emulator.community.inbox(
            self.session["username"], 0, self.limit, include_deleted=True, deleted_only=True)
        status, status_code = "0\t", 0
        result = str(len(messages))
        self.rows.extend(self.mail_row(message) for message in messages)
        return status, result, status_code

    def delete_news(self):
        query_fields = self.arguments.split("\t")
        try:
            if not query_fields or not self.emulator.community.delete_news(
                    int(query_fields[0]), self.session["username"]):
                raise ValueError("Article not found or not owned by this account")
            status, result, status_code = "0\tArticle deleted", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def change_block(self):
        blocked_name = self.arguments.strip()
        try:
            known_names = self.known_account_names()
            if blocked_name.casefold() not in known_names:
                raise ValueError("Player not found")
            self.emulator.community.ensure_profile(
                blocked_name, self.emulator.auth_user_id(blocked_name))
            if self.ordinal == 9:
                self.emulator.community.set_email_block(self.session["username"], blocked_name)
                status, result = "0\tSender blocked", ""
            else:
                if not self.emulator.community.remove_email_block(self.session["username"], blocked_name):
                    raise ValueError("Blocked sender not found")
                status, result = "0\tBlock removed", ""
            status_code = 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def forums(self):
        forums = self.emulator.community.forum_list(self.session["username"])[:self.limit]
        status, status_code = "0\t", 0
        result = str(len(forums))
        self.rows.extend("\t".join((str(forum["forum_id"]), forum["name"],
                                     str(forum["flags"]), str(forum["tribe_id"])))
                         for forum in forums)
        return status, result, status_code

    def forum_topics(self):
        try:
            forum_id = int(self.arguments.strip())
            page = int(self.fields[2]) if len(self.fields) > 2 else 0
            page = page if 0 <= page <= 5 else 0
            self.require_forum_access(forum_id)
            topics = self.emulator.community.list_forum_topics(forum_id, page * 60, 60)
            all_topics = self.emulator.community.list_forum_topics(forum_id, 0, 100)
            status, status_code = "0\t", 0
            result = str(len(all_topics))
            for topic in topics:
                created = datetime.datetime.fromtimestamp(topic["created_at"]).strftime(
                    "%m/%d/%Y %H:%M")
                row = ["", str(topic["topic_id"]), topic["subject"],
                       str(topic["post_count"]), "", "", created, "",
                       topic["author_name"], "", "", "", "0", str(topic["locked"]),
                       str(topic["update_id"])]
                self.rows.append("\t".join(row))
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "0", 1
        return status, result, status_code

    def forum_posts(self):
        query_fields = self.arguments.split("\t")
        try:
            topic_id = int(query_fields[0])
            after_update_id = int(query_fields[1]) if len(query_fields) > 1 else 0
            forum = self.emulator.community.forum_for_topic(topic_id)
            if forum is None:
                raise ValueError("Forum topic not found")
            self.require_forum_access(forum["forum_id"])
            posts = self.emulator.community.list_forum_posts(topic_id, after_update_id, self.limit)
            status, status_code = "0\t", 0
            result = str(len(posts))
            for post in posts:
                created = datetime.datetime.fromtimestamp(post["created_at"]).strftime(
                    "%m/%d/%Y %H:%M")
                row = [str(int(post["author_name"].casefold() == self.session["username"].casefold())),
                       "", str(post["post_id"]), str(post["parent_id"]),
                       str(post["update_id"]), post["author_name"],
                       str(post["author_id"]), post["author_tag"],
                       str(post["author_appending"]), "", created, "",
                       str(post["is_deleted"]), post["subject"], post["body"]]
                self.rows.append("\t".join(row))
        except (IndexError, ValueError) as exc:
            status, result, status_code = f"1\t{exc}", "0", 1
        return status, result, status_code

    def lock_topic(self):
        query_fields = self.arguments.split("\t")
        try:
            topic_id = int(query_fields[0])
            forum = self.emulator.community.forum_for_topic(topic_id)
            if forum is None:
                raise ValueError("Forum topic not found")
            self.require_forum_moderator(forum["forum_id"])
            reason = "\t".join(query_fields[1:]) if self.ordinal == 66 else ""
            self.emulator.community.set_forum_topic_locked(topic_id, self.ordinal == 66, reason)
            status, result, status_code = (
                ("0\tTopic locked", "", 0) if self.ordinal == 66 else
                ("0\tTopic unlocked", "", 0))
        except (IndexError, ValueError) as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def move_topic(self):
        query_fields = self.arguments.split("\t")
        try:
            topic_id = int(query_fields[0])
            target_forum_id = int(query_fields[1])
            source_forum = self.emulator.community.forum_for_topic(topic_id)
            if source_forum is None:
                raise ValueError("Forum topic not found")
            self.require_forum_moderator(source_forum["forum_id"])
            if target_forum_id < 0:
                self.require_forum_moderator(target_forum_id)
            self.emulator.community.move_forum_topic(topic_id, target_forum_id)
            status, result, status_code = "0\tTopic moved", "", 0
        except (IndexError, ValueError) as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def remove_topic(self):
        query_fields = self.arguments.split("\t")
        try:
            if len(query_fields) < 2 or query_fields[0].strip() != "0":
                raise ValueError("Unsupported topic moderation action")
            topic_id = int(query_fields[1])
            forum = self.emulator.community.forum_for_topic(topic_id)
            if forum is None:
                raise ValueError("Forum topic not found")
            self.require_forum_moderator(forum["forum_id"])
            if not self.emulator.community.delete_forum_topic(topic_id):
                raise ValueError("Forum topic not found")
            status, result, status_code = "0\tTopic removed", "", 0
        except (IndexError, ValueError) as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def history(self):
        target = self.arguments.strip()
        profile = self.resolve_profile_reference(target)
        if profile is None:
            status, result, status_code = "1\tPlayer not found", "0", 1
        else:
            created = datetime.datetime.fromtimestamp(profile["registered_at"]).strftime(
                "%m/%d/%Y %H:%M")
            status, result, status_code = "0\t", "1", 0
            self.rows.append(f"Local community profile first seen on {created}.")
        return status, result, status_code

    def create_forum_post(self):
        query_fields = self.arguments.split("\t")
        if len(query_fields) < 5:
            status, result, status_code = "1\tInvalid forum post request", "", 1
        else:
            try:
                self.require_forum_access(int(query_fields[0]))
                created = self.emulator.community.create_forum_post(
                    int(query_fields[0]), int(query_fields[1] or "0"),
                    int(query_fields[2] or "0"), self.session["username"],
                    self.emulator.auth_user_id(self.session["username"]),
                    query_fields[3], "\t".join(query_fields[4:]))
                status, result, status_code = "0\tPost created", str(created["post_id"]), 0
            except (ValueError, IndexError) as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def memberships(self):
        target = self.arguments.strip()
        remote_profile = self.tribesnext_profile(target)
        profile = (self.resolve_profile_reference(target) if remote_profile is None
                   else None if remote_profile is self.tribesnext_player_not_found
                   else remote_profile)
        if profile is None:
            status, result, status_code = "1\tPlayer not found", "0", 1
        elif profile is False:
            status, result, status_code = "1\tTribesNext profile unavailable", "0", 1
        else:
            if "memberships" in profile:
                tribes = robot_memberships(profile["memberships"][:self.limit])
            else:
                tribes = community_memberships(
                    self.emulator.community.list_profile_tribes(profile["username"], self.limit))
            self.rows.extend("\t".join((tribe["name"], tribe["tag"], str(tribe["id"]),
                                         str(tribe["rank"]), str(int(tribe["rank"] >= 2)),
                                         tribe["title"]))
                             for tribe in tribes)
            status, result, status_code = "0\t", str(len(tribes)), 0
        return status, result, status_code

    def edit_forum_post(self):
        query_fields = self.arguments.split("\t")
        try:
            post_id = int(query_fields[0])
            forum = self.emulator.community.forum_for_post(post_id)
            if forum is None:
                raise ValueError("Forum post not found")
            self.require_forum_access(forum["forum_id"])
            if len(query_fields) < 3 or not self.emulator.community.edit_forum_post(
                    post_id, self.session["username"], query_fields[1], "\t".join(query_fields[2:])):
                raise ValueError("Forum post not found or not owned by this account")
            status, result, status_code = "0\tPost updated", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def delete_forum_post(self):
        try:
            post_id = int(self.arguments.strip())
            forum = self.emulator.community.forum_for_post(post_id)
            if forum is None:
                raise ValueError("Forum post not found")
            self.require_forum_access(forum["forum_id"])
            if not self.emulator.community.delete_forum_post(post_id, self.session["username"]):
                raise ValueError("Forum post not found or not owned by this account")
            status, result, status_code = "0\tPost removed", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def player_profile(self):
        requested = self.arguments.split("\t", 1)[0].strip()
        if not requested:
            # A missing local AuthInfo display name should still open the
            # authenticated player's own Browser profile.
            requested = self.session["username"]
        remote_profile = self.tribesnext_profile(requested)
        if remote_profile is False or remote_profile is self.tribesnext_player_not_found:
            found = False
            local_account = session_account = legacy_account = False
        elif remote_profile is None and session_community(self.session) == "tribesnext":
            # TribesNext did not know the name and native community is also
            # checked: show an existing native profile, never create one.
            local_account = session_account = legacy_account = False
            found = self.emulator.community.get_profile(requested) is not None
        else:
            local_account = self.emulator.community.local_account_exists(requested)
            session_account = requested.casefold() == self.session["username"].casefold()
            legacy_account = woncrypto.private_store_contains(self.emulator.private_store, requested)
            found = remote_profile is not None or local_account or session_account or legacy_account
        self.emulator.log.event("irc_profile_lookup", connection=self.connection,
                                requested_characters=len(requested),
                                matches_session=session_account,
                                local_account=local_account,
                                legacy_account=legacy_account,
                                profile_found=found)
        if remote_profile is False:
            status, result, status_code = "1\tTribesNext profile unavailable", "", 1
        elif remote_profile is self.tribesnext_player_not_found:
            status, result, status_code = "1\tPlayer not found", "", 1
        elif remote_profile is not None:
            self.session["last_profile_username"] = requested
            try:
                created = int(remote_profile.get("creation", "0"))
                registered = datetime.datetime.fromtimestamp(created).strftime("%m/%d/%Y")
            except (ValueError, OverflowError, OSError):
                registered = ""
            status = "\t".join(("0", "", remote_profile.get("name", requested),
                                remote_profile.get("tag", ""),
                                remote_profile.get("append", "0"),
                                str(remote_profile["guid"]), registered,
                                remote_profile.get("online", "0"),
                                remote_profile.get("website", ""),
                                self.emulator.community.tribesnext_picture(
                                    remote_profile["guid"]) or ""))
            result = remote_profile.get("info", "")
            status_code = 0
        elif found:
            profile = self.emulator.community.get_profile(
                requested, self.emulator.auth_user_id(requested))
            self.session["last_profile_username"] = profile["username"]
            registered = datetime.datetime.fromtimestamp(
                profile["registered_at"]).strftime("%m/%d/%Y")
            status = "\t".join(("0", "", profile["display_name"], profile["tag"],
                                  str(profile["appending"]), str(profile["user_id"]),
                                  registered, "1", profile["url"],
                                  profile["graphic"]))
            result = profile["description"]
            status_code = 0
        else:
            status = "1\tPlayer not found"
            result = ""
            status_code = 1
        return status, result, status_code

    def player_search(self):
        query_fields = self.arguments.split("\t")
        raw_search = query_fields[0].strip() if query_fields else ""
        search = raw_search.casefold()
        remote_matches = self.tribesnext_search(
            self.emulator.tribesnext_robot.user_search, raw_search)
        merge = self.emulator.merges_native_community(self.session)
        if remote_matches is False and merge:
            remote_matches = []  # still list the native players
        if remote_matches is False:
            status, result, status_code = "1\tTribesNext search unavailable", "0", 1
        elif remote_matches is None:
            known = {profile["username"].casefold()
                     for profile in self.emulator.community.search_players("", 100)}
            all_names = (set(woncrypto.private_store_usernames(self.emulator.private_store)) |
                         set(self.emulator.community.local_account_usernames()))
            for name in all_names:
                if (name.casefold() not in known and search in name.casefold()):
                    self.emulator.community.ensure_profile(name, self.emulator.auth_user_id(name))
                    known.add(name.casefold())
            matches = self.emulator.community.search_players(search, self.limit)
            result = str(len(matches))
            status_code = 0
            status = "0\t"
            for index, profile in enumerate(matches):
                # The T2 browser consumes display name from column 0 and
                # stable WON user ID from column 3 of each result row.
                self.rows.append("\t".join((str(index), profile["display_name"], "", "",
                                            str(profile["user_id"]))))
        else:
            matches = remote_matches[:self.limit]
            self.rows.extend("\t".join((str(index), player["name"], player.get("tag", ""),
                                         player.get("append", "0"), str(player["guid"])))
                             for index, player in enumerate(matches))
            if merge:
                # Existing native profiles only; a search never creates records.
                seen = {player["name"].casefold() for player in matches}
                for profile in self.emulator.community.search_players(search, self.limit):
                    if len(self.rows) >= self.limit or profile["display_name"].casefold() in seen:
                        continue
                    seen.add(profile["display_name"].casefold())
                    self.rows.append("\t".join((str(len(self.rows)), profile["display_name"], "", "",
                                                str(profile["user_id"]))))
            result = str(len(self.rows))
            status_code = 0
            status = "0\t"
        return status, result, status_code

    def tribe_search(self):
        query_fields = self.arguments.split("\t")
        search = query_fields[0].strip() if query_fields else ""
        remote_matches = self.tribesnext_search(self.emulator.tribesnext_robot.clan_search, search)
        merge = self.emulator.merges_native_community(self.session)
        if remote_matches is False and merge:
            remote_matches = []
        if remote_matches is False:
            status, result, status_code = "1\tTribesNext search unavailable", "0", 1
        else:
            if remote_matches is not None:
                matches = remote_matches[:self.limit]
                self.rows.extend("\t".join((str(index), clan["name"], "", ""))
                                 for index, clan in enumerate(matches))
                if merge:
                    seen = {clan["name"].casefold() for clan in matches}
                    for tribe in self.emulator.community.search_tribes(search, self.limit):
                        if len(self.rows) >= self.limit or tribe["name"].casefold() in seen:
                            continue
                        seen.add(tribe["name"].casefold())
                        self.rows.append("\t".join((str(len(self.rows)), tribe["name"], tribe["tag"],
                                                    str(tribe["member_count"]))))
                    matches = self.rows
            else:
                matches = self.emulator.community.search_tribes(search, self.limit)
                self.rows.extend("\t".join((str(index), tribe["name"], tribe["tag"],
                                             str(tribe["member_count"])))
                                 for index, tribe in enumerate(matches))
            result = str(len(matches))
            status_code = 0
            status = "0\t"
        return status, result, status_code

    def tribe_members(self):
        tribe_name = self.arguments.split("\t", 1)[0].strip()
        remote_clan = self.tribesnext_clan(tribe_name)
        if remote_clan is False:
            members = []
            status, result, status_code = "1\tTribesNext clan unavailable", "0", 1
        elif remote_clan is self.tribesnext_clan_not_found:
            members = []
            status, result, status_code = "1\tTribe not found", "0", 1
        elif remote_clan is not None:
            members = remote_clan["members"][:self.limit]
            self.rows.extend("\t".join((member["name"], member.get("tag", ""),
                                         member.get("append", "0"), member["guid"],
                                         member["title"], member["rank"], "", "",
                                         str(int(int(member["rank"]) >= 2)),
                                         member["online"])) for member in members)
        else:
            members = self.emulator.community.list_tribe_members(tribe_name, self.limit)
            online = self.emulator.online_user_ids()
            self.rows.extend("\t".join([
                member["display_name"], "", "", str(member["user_id"]), member["title"],
                str(member["access"]), str(member["joined_at"]), "",
                str(int(member["access"] >= 2)), str(int(member["user_id"] in online)),
            ]) for member in members)
        if remote_clan is not False:
            result = str(len(members))
            status_code = 0
            status = "0\t"
        return status, result, status_code

    def links(self):
        links = (("Tribes 2 Home Page", "www.tribes2.com"),
                 ("T2 Technical Information", "sierrastudios.com/games/tribes2/support"))
        links = links[:self.limit]
        status, result, status_code = "0\t", str(len(links)), 0
        self.rows.extend(f"0\t{name}\t{address}" for name, address in links)
        return status, result, status_code

    def tribe_description(self):
        query_fields = self.arguments.split("\t")
        if len(query_fields) < 3:
            status, result, status_code = "1\tInvalid tribe description update", "", 1
        else:
            tribe_name = query_fields[0]
            description = "\t".join(query_fields[2:])
            try:
                self.require_tribe_admin(tribe_name)
                self.emulator.community.update_tribe(tribe_name, description=description)
                status, result, status_code = "0\tUpdated", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def create_tribe(self):
        query_fields = self.arguments.split("\t")
        if len(query_fields) < 6:
            status, result, status_code = "1\tInvalid tribe creation request", "", 1
        else:
            description = "\t".join(query_fields[5:])
            try:
                self.emulator.community.create_tribe(
                    query_fields[0], query_fields[1], query_fields[2], query_fields[3],
                    description, self.session["username"],
                    self.emulator.auth_user_id(self.session["username"]))
                status, result, status_code = "0\tCreated", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def tribe_profile(self):
        tribe_name = self.arguments.split("\t", 1)[0].strip()
        remote_clan = self.tribesnext_clan(tribe_name)
        tribe = self.emulator.community.get_tribe(tribe_name) if remote_clan is None else None
        if remote_clan is False:
            status, result, status_code = "1\tTribesNext clan unavailable", "", 1
        elif remote_clan is self.tribesnext_clan_not_found:
            status, result, status_code = "1\tTribe not found", "", 1
        elif remote_clan is not None:
            status = "\t".join(("0", "", str(remote_clan.get("id", "")),
                                remote_clan["name"], remote_clan.get("tag", ""),
                                remote_clan.get("append", "0"),
                                remote_clan.get("recruiting", "0"),
                                remote_clan.get("picture", "")))
            result, status_code = remote_clan.get("info", ""), 0
        elif tribe is None:
            status, result, status_code = "1\tTribe not found", "", 1
        else:
            status = "\t".join(("0", "", str(tribe["tribe_id"]), tribe["name"],
                                 tribe["tag"], str(tribe["appending"]),
                                 str(tribe["recruiting"]), tribe["graphic"]))
            result, status_code = tribe["description"], 0
        return status, result, status_code

    def delete_tribe(self):
        tribe_name = self.arguments.split("\t", 1)[0].strip()
        try:
            self.require_tribe_admin(tribe_name, minimum_access=4)
            if not self.emulator.community.delete_tribe(tribe_name):
                raise ValueError("Tribe not found")
            status, result, status_code = "0\tTribe deleted", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def remove_tribe_member(self):
        query_fields = self.arguments.split("\t")
        tribe_name = query_fields[1].strip() if len(query_fields) > 1 else ""
        member_name = query_fields[0].strip() if query_fields else ""
        try:
            actor = self.require_tribe_admin(tribe_name)
            target = self.emulator.community.membership(tribe_name, member_name)
            if target is None:
                raise ValueError("Tribe member not found")
            if (target["user_id"] == actor["user_id"] or
                    target["access"] >= actor["access"]):
                raise ValueError("Cannot remove an equal- or higher-ranked tribe member")
            if not self.emulator.community.remove_membership(tribe_name, member_name):
                raise ValueError("Tribe member not found")
            status, result, status_code = "0\tMember removed", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def tribe_setting(self):
        query_fields = self.arguments.split("\t")
        action = query_fields[0].strip().casefold() if query_fields else ""
        tribe_name = query_fields[1].strip() if len(query_fields) > 1 else ""
        value = query_fields[2].strip() if len(query_fields) > 2 else "0"
        tribe_field = {"recruiting": "recruiting", "appending": "appending"}.get(action)
        try:
            if tribe_field is None:
                raise ValueError("Unknown tribe setting")
            self.require_tribe_admin(tribe_name)
            self.emulator.community.update_tribe(tribe_name, **{tribe_field: int(value != "0")})
            status, result, status_code = "0\tUpdated", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def update_tribe_member(self):
        query_fields = self.arguments.split("\t")
        tribe_name = query_fields[0].strip() if query_fields else ""
        member_name = query_fields[1].strip() if len(query_fields) > 1 else ""
        title = query_fields[2] if len(query_fields) > 2 else ""
        try:
            access = int(query_fields[3]) if len(query_fields) > 3 else 0
            actor = self.require_tribe_admin(tribe_name, minimum_access=3)
            target = self.emulator.community.membership(tribe_name, member_name)
            if target is None:
                raise ValueError("Tribe membership not found")
            if access < 0 or access > 4:
                raise ValueError("Tribe rank must be between 0 and 4")
            is_self = target["user_id"] == actor["user_id"]
            if ((not is_self and target["access"] >= actor["access"]) or
                    access > actor["access"] or (not is_self and access >= actor["access"])):
                raise ValueError("Cannot modify an equal- or higher-ranked tribe member")
            self.emulator.community.update_membership(tribe_name, member_name,
                                                      title=title, access=access)
            status, result, status_code = "0\tMember profile updated", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def leave_tribe(self):
        tribe_name = self.arguments.split("\t", 1)[0].strip()
        if self.emulator.community.remove_membership(tribe_name, self.session["username"]):
            status, result, status_code = "0\tMembership removed", "", 0
        else:
            status, result, status_code = "1\tTribe membership not found", "", 1
        return status, result, status_code

    def primary_tribe(self):
        primary_tribe = self.arguments.split("\t", 1)[0].strip()
        if primary_tribe.casefold() == "none":
            primary_tribe = ""
        elif self.emulator.community.membership(primary_tribe, self.session["username"]) is None:
            status, result, status_code = "1\tTribe membership not found", "", 1
            primary_tribe = None
        if primary_tribe is not None:
            self.emulator.community.update_profile(self.session["username"],
                                                   self.emulator.auth_user_id(self.session["username"]),
                                                   primary_tribe=primary_tribe)
            status, result, status_code = "0\tPrimary tribe updated", primary_tribe, 0
        return status, result, status_code

    def invite(self):
        query_fields = self.arguments.split("\t")
        tribe_name = query_fields[0].strip() if query_fields else ""
        invited_name = query_fields[1].strip() if len(query_fields) > 1 else ""
        try:
            self.require_tribe_admin(tribe_name)
            if (invited_name.casefold() != self.session["username"].casefold() and
                    not self.emulator.community.local_account_exists(invited_name) and
                    not woncrypto.private_store_contains(self.emulator.private_store, invited_name)):
                raise ValueError("Player not found")
            self.emulator.community.invite(tribe_name, invited_name,
                                           self.emulator.auth_user_id(invited_name),
                                           self.session["username"],
                                           self.emulator.auth_user_id(self.session["username"]))
            status, result, status_code = "0\tInvitation sent", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def answer_invite(self):
        query_fields = self.arguments.split("\t")
        action = query_fields[0].strip().casefold() if query_fields else ""
        tribe_name = query_fields[1].strip() if len(query_fields) > 1 else ""
        target_name = query_fields[2].strip() if len(query_fields) > 2 else self.session["username"]
        try:
            if action == "cancel":
                self.require_tribe_admin(tribe_name)
                expected_status = ("pending", "requested")
            elif target_name.casefold() != self.session["username"].casefold():
                self.require_tribe_admin(tribe_name)
                expected_status = "requested"
            else:
                expected_status = "pending"
            result = self.emulator.community.respond_to_invite(
                action, tribe_name, target_name,
                self.emulator.auth_user_id(target_name), expected_status)
            status, status_code = "0\tInvitation updated", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def request_invitation(self):
        tribe_name = self.arguments.strip()
        try:
            self.emulator.community.request_invitation(
                tribe_name, self.session["username"],
                self.emulator.auth_user_id(self.session["username"]))
            status, result, status_code = "0\tInvitation request sent", "", 0
        except ValueError as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def tribe_graphic_or_tag(self):
        query_fields = self.arguments.split("\t")
        try:
            if self.ordinal == 29:
                tribe_name = query_fields[0].strip()
                graphic = "\t".join(query_fields[1:])
            else:
                tribe = self.emulator.community.tribe_by_id(int(query_fields[0]))
                if tribe is None:
                    raise ValueError("Tribe not found")
                tribe_name = tribe["name"]
                graphic = None
            self.require_tribe_admin(tribe_name, minimum_access=3)
            if self.ordinal == 29:
                self.emulator.community.update_tribe(tribe_name, graphic=graphic)
            else:
                self.emulator.community.update_tribe(tribe_name, tag="\t".join(query_fields[1:]))
            status, result, status_code = "0\tUpdated", "", 0
        except (ValueError, IndexError) as exc:
            status, result, status_code = f"1\t{exc}", "", 1
        return status, result, status_code

    def online_status(self):
        online = self.emulator.online_user_ids()
        result = "".join("1" if user_id.isdecimal() and int(user_id) in online else "0"
                         for user_id in self.arguments.split("\t")) if self.arguments else ""
        status_code = 0
        status = "0\t"
        return status, result, status_code

    def profile_field(self):
        field = {17: "description", 31: "graphic", 32: "url", 33: "display_name"}[self.ordinal]
        self.emulator.community.update_profile(
            self.session["username"], self.emulator.auth_user_id(self.session["username"]),
            **{field: self.arguments})
        result = ""
        status_code = 0
        status = "0\tUpdated"
        return status, result, status_code

    ROBOT_HANDLERS: ClassVar[dict] = {
        (1, True): tn_mailbox,
        (2, True): tn_block_list,
        (5, False): tn_send_mail,
        (5, True): tn_buddy_list,
        (6, False): tn_update_mail,
        (7, False): tn_update_mail,
        (8, False): tn_change_block,
        (9, False): tn_change_block,
        (10, False): tn_change_buddy,
        (10, True): tn_tribe_options,
        (11, False): tn_change_buddy,
        (12, True): tn_history,
        (14, True): tn_mailbox,
        (26, False): tn_clear_buddies,
        (28, False): tn_answer_invite,
        (31, False): tn_set_picture,
        (35, False): tn_update_mail,
    }

    LOCAL_HANDLERS: ClassVar[dict] = {
        (0, False): welcome,
        (0, True): news,
        (1, False): post_news,
        (1, True): inbox,
        (2, False): edit_news,
        (2, True): block_list,
        (3, False): delete_news,
        (3, True): player_search,
        (4, False): tribe_search,
        (4, True): tribe_search,
        (5, False): send_mail,
        (5, True): buddy_list,
        (6, False): update_mail,
        (6, True): tribe_members,
        (7, False): update_mail,
        (7, True): forums,
        (8, False): change_block,
        (8, True): forum_topics,
        (9, False): change_block,
        (9, True): forum_posts,
        (10, False): change_buddy,
        (10, True): tribe_options,
        (11, False): change_buddy,
        (11, True): tribe_invitations,
        (12, False): create_forum_post,
        (12, True): history,
        (13, False): edit_forum_post,
        (13, True): memberships,
        (14, False): delete_forum_post,
        (14, True): deleted_mail,
        (15, False): tribe_description,
        (15, True): links,
        (16, False): create_tribe,
        (16, True): create_tribe,
        (17, False): profile_field,
        (17, True): profile_field,
        (18, False): delete_tribe,
        (18, True): delete_tribe,
        (19, False): remove_tribe_member,
        (19, True): remove_tribe_member,
        (20, False): tribe_setting,
        (20, True): tribe_setting,
        (21, False): update_tribe_member,
        (21, True): update_tribe_member,
        (22, False): tribe_profile,
        (22, True): tribe_profile,
        (23, False): player_profile,
        (23, True): player_profile,
        (24, False): leave_tribe,
        (24, True): leave_tribe,
        (25, False): primary_tribe,
        (25, True): primary_tribe,
        (26, False): clear_buddies,
        (27, False): invite,
        (27, True): invite,
        (28, False): answer_invite,
        (28, True): answer_invite,
        (29, False): tribe_graphic_or_tag,
        (29, True): tribe_graphic_or_tag,
        (30, False): tribe_graphic_or_tag,
        (30, True): tribe_graphic_or_tag,
        (31, False): profile_field,
        (31, True): profile_field,
        (32, False): profile_field,
        (32, True): profile_field,
        (33, False): profile_field,
        (33, True): profile_field,
        (34, False): request_invitation,
        (62, False): remove_topic,
        (66, False): lock_topic,
        (67, False): lock_topic,
        (68, False): move_topic,
        (69, False): online_status,
        (69, True): online_status,
        (100, True): news,
    }

# Player history, buddy list, T-Mail, and block list queries answered from the
# TribesNext robot API (or, for the picture it lacks, a GUID-keyed local record).
TRIBESNEXT_ROBOT_QUERIES = frozenset(DBProxyQuery.ROBOT_HANDLERS)
# Everything a TribesNext session may query; the rest is refused as unavailable.
TRIBESNEXT_ALLOWED_QUERIES = TRIBESNEXT_ROBOT_QUERIES | frozenset({
    (3, True), (4, True), (4, False), (6, True), (13, True),
    (22, False), (23, False), (69, False),
})


def tribesnext_invite_body(message, subject, recipient_name):
    """Render a TribesNext invite's JSON body as text with stock accept/reject links."""
    try:
        invite = json.loads(message["body"])
        clan_id = int(invite["clanid"])
        expires = int(invite["expire"])
    except (ValueError, TypeError, KeyError):
        return None
    sender_name, sender_tag, sender_append = message["sender"][:3]
    sender = sender_name + sender_tag if sender_append == "1" else sender_tag + sender_name
    clan = subject.removeprefix("Invitation to:").strip() or "a tribe"
    try:
        until = datetime.datetime.fromtimestamp(expires).strftime("%m/%d/%Y %H:%M")
    except (ValueError, OverflowError, OSError):
        return None
    text = f"{sender} has invited you to join {clan}.\n\n"
    if expires <= time.time():
        return text + f"This invitation expired on {until}."
    # acceptinvite/rejectinvite are the stock Browser links; they send query 28.
    return (text + f"This invitation is valid until {until}.\n\n"
            f"<a:acceptinvite\t{clan_id}\t{recipient_name}>Accept invitation</a>     "
            f"<a:rejectinvite\t{clan_id}\t{recipient_name}>Reject invitation</a>")


def tribesnext_mail_row(header, message, recipient):
    """Format a robot message as a stock T-Mail row (the field order of mail_row)."""
    try:
        created = datetime.datetime.fromtimestamp(
            int(message.get("time") or header["time"])).strftime("%m/%d/%Y %H:%M")
    except (ValueError, OverflowError, OSError):
        created = ""
    recipient_guid = recipient[3]
    is_cc = (any(player[3] == recipient_guid for player in message["cc"]) and
             not any(player[3] == recipient_guid for player in message["to"]))
    # The client splits rows on tabs and messages into newline records.
    subject = " ".join(message["subject"].split())
    body = message["body"]
    if message.get("type", header.get("type")) == "invite":
        body = tribesnext_invite_body(message, subject, recipient[0]) or body
    return "\t".join((str(header["id"]), *message["sender"], *recipient, created,
                      str(int(is_cc)), str(int(message.get("deleted", False))),
                      str(int(header["read"])),
                      ",".join(player[0] for player in message["to"]),
                      ",".join(player[0] for player in message["cc"]),
                      subject, str(max(1, len(body.splitlines()))), body))


# Auth1 failures the stock script answers with "check your login name and
# password": WS_AuthServ_UserNotFound and WS_AuthServ_BadPassword.
AUTH_REJECTION_STATUS = {"account_missing": -1506, "store_missing": -1506, "invalid": -1507}


class AuthHandler(socketserver.BaseRequestHandler):
    def handle(self):
        emulator = self.server.emulator
        connection = next(emulator.connections)
        with emulator.auth_session_lock:
            emulator.auth_sessions.pop(self.client_address[0], None)
        self.request.settimeout(10)
        emulator.log.event("connected", service="auth", connection=connection, peer=list(self.client_address))
        session_key = None
        login_username = None
        tribesnext_credentials = None
        auth_provider = None
        try:
            while True:
                payload = recv_frame(self.request)
                if payload is None:
                    break
                emulator.log.event("received", service="auth", connection=connection, **digest(payload))
                if len(payload) < 5:
                    raise ValueError("Truncated authentication header")
                header, service, message = struct.unpack_from("<BHH", payload)
                emulator.log.event("auth_message", connection=connection, header=header, service=service,
                                   message=message, payload_bytes=len(payload))
                if (header, service, message) == (5, 12, 1) and len(payload) == 5:
                    block = woncrypto.key_block(emulator.keys)
                    reply = frame(struct.pack("<BHHhH", 5, 12, 2, 0, len(block)) + block)
                    self.request.sendall(reply)
                    emulator.log.event("sent", service="auth", connection=connection, message=2,
                                       key_block_id=1, **digest(reply))
                    print("[wonemu] auth: sent signed public-key block", flush=True)
                    continue

                if (header, service, message) == (5, 12, 3):
                    emulator.log.event("capture_only", service="auth", connection=connection,
                                       message=message, **digest(payload))
                    login = None
                    encrypted_key = None
                    encrypted_data = None
                    try:
                        # TribesNext keys never serve another machine's login: a
                        # remote client can use only this emulator's own accounts.
                        this_machine = from_this_machine(self.request, self.client_address)
                        login = decode_login_envelope(
                            payload, emulator.keys,
                            emulator.private_store if this_machine else None, emulator.community,
                            emulator.refresh_tribesnext_account
                            if emulator.refresh_tribesnext and this_machine else None,
                            emulator.auth_order,
                            emulator.recover_tribesnext_account
                            if this_machine and emulator.private_store else None)
                        reader = Reader(payload)
                        reader.take(5)
                        reader.take(2)
                        encrypted_key = reader.take(reader.number("<H"))
                        encrypted_data = reader.take(reader.number("<H"))
                        emulator.log.event("login_envelope", connection=connection, **login)
                        state = (f"decrypted login payload ({login['login_plaintext_bytes']} bytes)"
                                 if login["login_data_decryption"] == "decoded"
                                 else "login payload remains encrypted")
                        print(f"[wonemu] auth: parsed message 3 envelope ({login['elgamal_plaintext_bytes']} ElGamal bytes, "
                              f"{login['encrypted_login_data_bytes']} login bytes); {state}", flush=True)
                    except (OSError, ValueError) as exc:
                        emulator.log.event("login_decode_error", connection=connection, error=str(exc))
                    if login and login.get("account_validation") == "valid" and encrypted_key:
                        try:
                            session_key, login_data = woncrypto.decrypt_won_login_data(
                                emulator.keys["auth"], encrypted_key, encrypted_data)
                            decoded_login = parse_login_payload(login_data, session_key)
                            login_username = decoded_login["username"]
                            auth_provider = login.get("credential_source")
                            if (auth_provider == "tribesnext" and this_machine and
                                    emulator.private_store and emulator.public_store):
                                try:
                                    tribesnext_credentials = tribesnext.unlock_account(
                                        emulator.public_store, emulator.private_store,
                                        login_username, decoded_login["password"])
                                except (OSError, ValueError, UnicodeError):
                                    tribesnext_credentials = None
                                if tribesnext_credentials is not None:
                                    # The client uppercases the login name; the game
                                    # shows the name the account was registered with.
                                    login_username = tribesnext_credentials.certificate.username
                            elif auth_provider == "local":
                                login_username = (emulator.community.local_account_name(login_username)
                                                  or login_username)
                        except (OSError, ValueError) as exc:
                            emulator.log.event("auth_challenge_error", connection=connection, error=str(exc))
                        else:
                            challenge = secrets.token_bytes(16)
                            cipher = woncrypto.encrypt_won_crypt_transform(session_key, challenge)
                            reply = frame(struct.pack("<BHHH", 5, 12, 4, len(cipher)) + cipher)
                            self.request.sendall(reply)
                            emulator.log.event("sent", service="auth", connection=connection, message=4,
                                               encrypted_challenge_bytes=len(cipher))
                            print("[wonemu] auth: sent encrypted challenge for local account", flush=True)
                            continue
                    outcome = login.get("account_validation", "not_decoded") if login else "not_decoded"
                    status = AUTH_REJECTION_STATUS.get(outcome, -1)
                    rejection = frame(struct.pack("<BHHh", 5, 12, 6, status))
                    self.request.sendall(rejection)
                    emulator.log.event("sent", service="auth", connection=connection,
                                       message=6, status=status)
                    print(f"[wonemu] auth: explicit rejection; account validation={outcome}", flush=True)
                    break

                if (header, service, message) == (5, 12, 5) and session_key:
                    reader = Reader(payload)
                    reader.take(5)
                    cipher = reader.take(reader.number("<H"))
                    if reader.offset != len(payload):
                        raise ValueError("Trailing bytes in auth challenge response")
                    response = woncrypto.decrypt_won_crypt_transform(session_key, cipher)
                    expected_response = response == b"\0"
                    emulator.log.event("auth_challenge_response", connection=connection,
                                       plaintext_bytes=len(response), expected_response=expected_response)
                    if expected_response and login_username:
                        reply, client_key = woncrypto.auth1_login_reply(
                            emulator.keys, session_key, login_username, return_client_key=True,
                            user_id=emulator.auth_user_id(login_username, log_selection=True),
                            tribes=emulator.auth_info_tribes(login_username, auth_provider,
                                                             tribesnext_credentials))
                        emulator.register_auth_session(self.client_address[0], login_username,
                                                       session_key, reply, client_key,
                                                       tribesnext_credentials, auth_provider)
                        self.request.sendall(frame(reply))
                        emulator.log.event("sent", service="auth", connection=connection,
                                           message=6, status=0, response_bytes=len(reply))
                        print("[wonemu] auth: completed local Auth1 login", flush=True)
                    else:
                        rejection = frame(struct.pack("<BHHh", 5, 12, 6, -1))
                        self.request.sendall(rejection)
                        emulator.log.event("sent", service="auth", connection=connection,
                                           message=6, status=-1)
                        print("[wonemu] auth: challenge response rejected", flush=True)
                    break

                emulator.log.event("capture_only", service="auth", connection=connection,
                                   message=message, **digest(payload))
                print(f"[wonemu] auth: captured message {message} ({len(payload) + 4} bytes); handler not implemented", flush=True)
                break
        except (OSError, ValueError) as exc:
            emulator.log.event("connection_error", service="auth", connection=connection, error=str(exc))


class MasterDatagramHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, _socket = self.request
        self.server.emulator.handle_master_datagram(data, self.client_address, _socket)


class Emulator:
    def __init__(self, ports=None, log_dir=LOG_DIR, empty=False, keys=None, private_store=None,
                 master_backend="local", tribesnext_master_url=TRIBESNEXT_MASTER_URL,
                 community_db=None, refresh_tribesnext=False, master_sources=None,
                 bind_address=HOST, advertise_address="", backends=None, account_creation=True):
        self.ports = dict(ports or PORTS)
        self.bind_address = bind_address or "0.0.0.0"
        self.advertise_address = advertised_address(self.bind_address, advertise_address)
        self.records = ({path: [] for path in directory_records(self.ports)} if empty
                        else directory_records(self.ports, self.advertise_address))
        self.empty = empty
        self.keys = keys
        # DBProxy's own key, certified by the auth key for peer authentication.
        self.dbproxy_key = woncrypto.make_key(keys["auth"]) if keys else None
        self.peer_sessions = {}
        self.account_creation = bool(account_creation)
        self.private_store = Path(private_store) if private_store is not None else None
        self.public_store = self.private_store.with_name("public.store") if self.private_store else None
        self.tribesnext_robot = tribesnext.RobotClient()
        self.refresh_tribesnext = bool(refresh_tribesnext)
        self.recovery_secret = secrets.token_bytes(32)
        self.recovery_failures = {}
        self.recovery_lock = threading.Lock()
        self.community = community_store.CommunityStore(
            community_db or (Path(log_dir) / "community.sqlite3"))
        self.community_db_path = self.community.path
        self.auth_sessions = {}
        self.auth_session_lock = threading.RLock()
        self.irc_presence = {}
        self.irc_presence_lock = threading.RLock()
        if backends is None:
            if master_sources is None:
                if master_backend not in {"local", "tribesnext", "hybrid"}:
                    raise ValueError(f"Unknown master-server backend: {master_backend}")
                master_sources = master_backend_sources(master_backend, tribesnext_master_url)
            backends = backends_from_master_sources(master_sources, tribesnext_master_url)
        self.backends = parse_backends(backends, tribesnext_master_url)
        self.master_sources = backends_master_sources(self.backends)
        # Priority order for each service; see BACKEND_KINDS.
        self.auth_order = tuple(backend["kind"] for backend in self.backends if backend.get("auth"))
        self.irc_backends = tuple((backend["kind"], backend["irc"]) for backend in self.backends
                                  if backend.get("irc"))
        own_hosts = {"localhost", "0.0.0.0", self.bind_address, self.advertise_address}
        for kind, address in self.master_sources:
            if kind == "torque":
                host, port = parse_endpoint(address)
                if (host in own_hosts or is_loopback(host)) and port == self.ports["master"]:
                    raise ValueError(f"Master source {address} is this emulator's own master service")
        # Shown in logs and identity; a canonical "kind=address,..." string.
        self.master_backend = format_master_sources(self.master_sources)
        self.tribesnext_master_url = next(
            (address for kind, address in self.master_sources if kind == "tribesnext"),
            tribesnext_master_url.rstrip("/"))
        self.master_servers = {}
        self.master_sessions = {}
        self.master_lock = threading.RLock()
        self.log = CaptureLog(Path(log_dir) / "traffic.jsonl")
        self.stopped = threading.Event()
        self.connections = itertools.count(1)
        self.servers = []
        self.threads = []

    def register_auth_session(self, peer_ip, username, session_key, auth1_reply, client_key,
                              tribesnext_credentials=None, auth_provider="local"):
        auth_provider = auth_provider or "local"
        community, chat = self.session_backends(auth_provider, tribesnext_credentials)
        if community == "native":
            user_id = self.auth_user_id(username)
            self.community.ensure_profile(username, user_id)
        with self.auth_session_lock:
            self.auth_sessions[peer_ip] = {
                "username": str(username),
                "session_key": bytes(session_key),
                "auth1_reply": bytes(auth1_reply),
                "client_key": dict(client_key),
                "tribesnext_credentials": tribesnext_credentials,
                "auth_provider": auth_provider,
                "community": community,
                "chat": chat,
                "created": time.time(),
            }

    def session_backends(self, auth_provider, tribesnext_credentials=None):
        """The community and chat backends for a login, in IRC priority order.

        TribesNext community data needs a TribesNext login (the robot API acts
        with the player's credentials); native community data serves anyone.
        Chat goes to the first IRC backend that offers it; TribesNext chat is a
        public IRC network, so it needs no TribesNext login.
        """
        community = next((kind for kind, _address in self.irc_backends
                          if kind == "native" or (kind == "tribesnext" and auth_provider == "tribesnext"
                                                  and tribesnext_credentials is not None)), None)
        chat = next(((kind, address) for kind, address in self.irc_backends
                     if kind == "native" or isinstance(address, str)), None)
        return community, chat

    def merges_native_community(self, session):
        """Whether a TribesNext community session also sees native records."""
        return (session_community(session) == "tribesnext" and
                any(kind == "native" for kind, _address in self.irc_backends))

    def auth_info_tribes(self, username, auth_provider=None, tribesnext_credentials=None):
        """Return the memberships to embed in the Auth2 AuthInfo, best effort.

        A lookup failure yields no tribes rather than failing the login.
        """
        provider = auth_provider or "local"
        try:
            if provider == "tribesnext":
                guid = self.tribesnext_user_id(username)
                if tribesnext_credentials is None or guid is None:
                    memberships = []
                else:
                    memberships = robot_memberships(self.tribesnext_robot.user_view(
                        tribesnext_credentials, guid)["memberships"])
            else:
                memberships = community_memberships(self.community.list_profile_tribes(username))
        except (OSError, ValueError, KeyError, tribesnext.RobotAPIError) as exc:
            self.log.event("auth_info_tribes_unavailable", provider=provider,
                           error=type(exc).__name__)
            return []
        tribes = []
        for tribe in memberships:
            if len(tribes) == woncrypto.T2_AUTH_INFO_MAX_TRIBES:
                break
            try:
                woncrypto.t2_auth_info_blob("", [tribe])
            except ValueError:
                self.log.event("auth_info_tribe_skipped", provider=provider, tribe_id=tribe["id"])
                continue
            tribes.append(tribe)
        self.log.event("auth_info_tribes", provider=provider, tribes=len(tribes),
                       available=len(memberships))
        return tribes

    def auth_user_id(self, username, log_selection=False):
        """Use the T2 certificate GUID for WON identity when it is locally stored."""
        if self.public_store and self.public_store.is_file():
            try:
                with self.public_store.open("r", encoding="ascii") as public_store:
                    for line in public_store:
                        fields = line.rstrip("\r\n").split("\t")
                        if len(fields) < 2 or fields[0].casefold() != str(username).casefold():
                            continue
                        if fields[1].isdecimal():
                            user_id = int(fields[1])
                            if 0 < user_id <= 0xffffffff:
                                if log_selection:
                                    self.log.event("auth_identity_selected",
                                                   source="tribes2_public_store",
                                                   username_characters=len(str(username)))
                                return user_id
                        break
            except (OSError, UnicodeError):
                pass
        if log_selection:
            self.log.event("auth_identity_selected", source="local_fallback",
                           username_characters=len(str(username)))
        return woncrypto.local_auth_user_id(username)

    def tribesnext_user_id(self, username):
        """Resolve a local account name to its TribesNext certificate GUID."""
        if not self.public_store or not self.public_store.is_file():
            return None
        try:
            with self.public_store.open("r", encoding="ascii") as public_store:
                for line in public_store:
                    fields = line.rstrip("\r\n").split("\t")
                    if len(fields) >= 2 and fields[0].casefold() == str(username).casefold():
                        if fields[1].isdecimal() and 0 < int(fields[1]) <= 0xffffffff:
                            return int(fields[1])
                        return None
        except (OSError, UnicodeError):
            return None
        return None

    def auth_session_for_peer(self, peer_ip):
        with self.auth_session_lock:
            session = self.auth_sessions.get(peer_ip)
            if session is None:
                return None
            if time.time() - session["created"] > 3600:
                del self.auth_sessions[peer_ip]
                return None
            return dict(session)

    def open_peer_session(self, peer_ip, key, auth_session):
        """A DBProxy peer-auth session; the client reuses it on later connections."""
        with self.auth_session_lock:
            self.peer_sessions[peer_ip] = {
                "key": bytes(key), "username": auth_session["username"],
                "auth_provider": auth_session["auth_provider"],
                "client_sequence": 0, "server_sequence": 0, "created": time.time()}

    def peer_session(self, peer_ip):
        with self.auth_session_lock:
            peer = self.peer_sessions.get(peer_ip)
            if peer is not None and time.time() - peer["created"] > PEER_SESSION_SECONDS:
                del self.peer_sessions[peer_ip]
                peer = None
            return peer

    def account_exists(self, username):
        return (self.community.local_account_exists(username) or
                woncrypto.private_store_contains(self.private_store, username))

    def create_stock_account(self, fields, this_machine):
        """Create Account from the stock dialog; returns a DBProxy status and text."""
        if not self.account_creation:
            return DBPROXY_SHOW_TEXT, "Account creation is turned off on this server."
        if not woncrypto.cd_key_is_valid(fields["cd_key"]):
            return DBPROXY_INVALID_CDKEY, ""
        # The client uppercases the login name; keep the warrior name's
        # capitalization when the two are the same name.
        username = (fields["warrior"] if fields["warrior"].casefold() == fields["login"].casefold()
                    else fields["login"])
        # TribesNext keys are only installed for, and used by, this machine.
        backends = [kind for kind in self.auth_order
                    if kind == "native" or (kind == "tribesnext" and this_machine and self.private_store)]
        if not backends:
            return DBPROXY_SHOW_TEXT, "This server cannot create accounts for another machine."
        if self.account_exists(username):
            return DBPROXY_USER_EXISTS, ""
        provider = backends[0]
        try:
            if provider == "tribesnext":
                tribesnext.create_account(username, fields["password"], self.public_store,
                                          self.private_store)
            else:
                user_id = woncrypto.local_auth_user_id(username)
                self.community.create_local_account(username, user_id, fields["password"])
                if fields["warrior"] != username:
                    self.community.update_profile(username, user_id, display_name=fields["warrior"])
        except tribesnext.AccountServerError as exc:
            self.log.event("account_created", provider=provider, result=exc.code)
            if exc.code == "TAKEN":
                return DBPROXY_USER_EXISTS, ""
            if exc.code in ("TOOSHORT", "TOOLONG", "INVALID", "INVALIDNAME"):
                return DBPROXY_INVALID_USERNAME, ""
            return DBPROXY_SHOW_TEXT, str(exc)
        except ValueError as exc:
            self.log.event("account_created", provider=provider, result="rejected")
            return DBPROXY_SHOW_TEXT, str(exc)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            self.log.event("account_created", provider=provider, result="failed",
                           error_type=type(exc).__name__)
            return DBPROXY_DB_ERROR, type(exc).__name__
        self.community.set_account_info(username, provider, fields["email"], fields["send_info"])
        self.log.event("account_created", provider=provider, result="created")
        return 0, ""

    def update_stock_account(self, peer, fields):
        """Edit Account's Update for the peer session's own account."""
        username = peer["username"]
        if peer["auth_provider"] == "tribesnext":
            # TribesNext has no password change, and re-encrypting only this
            # machine's private.store would split it from RECOVER elsewhere.
            if woncrypto.verify_private_store_password(
                    self.private_store, username, fields["password"]) != "valid":
                return DBPROXY_SHOW_TEXT, ("TribesNext passwords can't be changed in the game. "
                                           "Nothing was updated.")
            provider = "tribesnext"
        else:
            status, _enrolled = self.community.verify_local_account(username, fields["password"])
            if status != "valid":
                self.community.set_local_password(username, fields["password"])
            provider = "native"
        self.community.set_account_info(username, provider, fields["email"], fields["send_info"])
        self.log.event("account_updated", provider=provider)
        return 0, ""

    def update_irc_presence(self, user_id, connection, online):
        with self.irc_presence_lock:
            connections = self.irc_presence.setdefault(int(user_id), set())
            if online:
                connections.add(connection)
            else:
                connections.discard(connection)
                if not connections:
                    self.irc_presence.pop(int(user_id), None)

    RECOVERY_FAILURE_SECONDS = 5 * 60

    def recover_tribesnext_account(self, username, password):
        """Install a TribesNext account missing here; failures are not retried for a while."""
        # Keyed by name and an HMAC of the password under a per-process
        # secret, so a typo followed by the right password is not blocked.
        attempt = (username.casefold(),
                   hmac.new(self.recovery_secret, password.encode("utf-8", "replace"),
                            hashlib.sha256).digest())
        now = time.monotonic()
        with self.recovery_lock:
            self.recovery_failures = {key: until for key, until in self.recovery_failures.items()
                                      if until > now}
            if attempt in self.recovery_failures:
                self.log.event("tribesnext_recovery", result="recently_failed")
                return "recently_failed"
        result = self.refresh_tribesnext_account(username, password)
        if result != "refreshed":
            with self.recovery_lock:
                self.recovery_failures[attempt] = now + self.RECOVERY_FAILURE_SECONDS
        return result

    def refresh_tribesnext_account(self, username, password):
        if not self.private_store:
            self.log.event("tribesnext_recovery", result="store_missing")
            return "store_missing"
        public_store = self.private_store.with_name("public.store")
        try:
            tribesnext.recover_account(
                username, password, public_store, self.private_store, timeout=8.0)
        except (OSError, ValueError, UnicodeError) as exc:
            result = "failed"
            self.log.event("tribesnext_recovery", result=result,
                           error_type=type(exc).__name__)
            return result
        self.log.event("tribesnext_recovery", result="refreshed")
        return "refreshed"

    def online_user_ids(self):
        with self.irc_presence_lock:
            return {user_id for user_id, connections in self.irc_presence.items() if connections}

    def identity(self):
        return {"identity": IDENTITY.decode(), "ports": self.ports, "empty_directory": self.empty,
                "pid": os.getpid(),
                "stage": AUTH_STAGE if self.keys else "directory + DBProxy + Torque master proxy",
                "implementation_sha256": IMPLEMENTATION_SHA256,
                "backends": format_backends(self.backends),
                "bind_address": self.bind_address,
                "advertise_address": self.advertise_address,
                "refresh_tribesnext": self.refresh_tribesnext,
                "account_creation": self.account_creation,
                "verifier_sha256": hashlib.sha256(woncrypto.public_key(self.keys["root"])).hexdigest() if self.keys else None,
                "auth_key_sha256": hashlib.sha256(woncrypto.public_key(self.keys["auth"])).hexdigest() if self.keys else None,
                "private_store_path_sha256": hashlib.sha256(str(self.private_store.resolve()).encode()).hexdigest()
                if self.private_store else None,
                "community_db_path_sha256": hashlib.sha256(
                    str(self.community_db_path).encode()).hexdigest()}

    def fetch_tribesnext(self, path, base_url=None):
        url = (base_url or self.tribesnext_master_url) + path
        request = urllib.request.Request(url, headers={"User-Agent": "Tribes 2"})
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.read(1_000_000).decode("utf-8", errors="replace")

    def tribesnext_servers(self, query, base_url=None):
        # The client's own default is lowercase "any"; TribesNext's filtered
        # list only matches "Any".
        game_type = query["game_type"] or "Any"
        mission_type = query["mission_type"] or "Any"
        if game_type.casefold() == "any":
            game_type = "Any"
        if mission_type.casefold() == "any":
            mission_type = "Any"
        # 255 bots is the default once the hook lifts the client's 16-bot limit.
        unfiltered = (game_type == "Any" and mission_type == "Any" and
                      query["min_players"] == 0 and query["max_players"] in (0, 100, 255) and
                      query["max_bots"] in (0, 16, 255) and query["regions"] in (0, 0xffffffff) and
                      query["version"] == 0 and query["min_cpu"] == 0 and
                      query["info_flags"] == 0 and not query["buddies"])
        if unfiltered:
            path = "/list"
        else:
            flags = str(query["info_flags"])
            if query["buddies"]:
                flags += " " + " ".join(str(value) for value in query["buddies"])
            values = (game_type, mission_type, query["min_players"], query["max_players"],
                      query["max_bots"], flags)
            path = "/list/" + "/".join(urllib.parse.quote(str(value), safe="") for value in values)
        text = self.fetch_tribesnext(path, base_url)
        servers = []
        for line in text.splitlines():
            value = line.strip()
            if ":" not in value:
                continue
            host, port_text = value.rsplit(":", 1)
            try:
                address = socket.inet_aton(host)
                port = int(port_text)
            except (OSError, ValueError):
                continue
            if 1 <= port <= 65535:
                servers.append((address, port))
        return servers

    def tribesnext_types(self, base_url=None):
        text = self.fetch_tribesnext("/listtypes", base_url)
        game_types = set()
        mission_types = set()
        for line in text.splitlines():
            fields = line.strip().split(None, 1)
            if len(fields) != 2:
                continue
            if fields[0] == "0":
                game_types.add(fields[1])
            elif fields[0] == "1":
                mission_types.add(fields[1])
        return sorted(game_types), sorted(mission_types)

    def publish_tribesnext_heartbeat(self, peer, base_url=None):
        ip, port = peer
        path = f"/add/{port}/{urllib.parse.quote(ip, safe='')}"
        try:
            text = self.fetch_tribesnext(path, base_url)
        except (OSError, ValueError) as exc:
            self.log.event("master_upstream_error", operation="heartbeat", error=str(exc))
            return
        result = next((line.strip() for line in text.splitlines() if line.strip()), "")
        self.log.event("master_upstream_heartbeat", result=result[:32])

    def torque_master_exchange(self, address, request, reply_type):
        """Send one Torque master request; return the replies of reply_type."""
        host, port = parse_endpoint(address)
        replies = {}
        deadline = time.monotonic() + 2.0
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as conn:
            conn.connect((socket.gethostbyname(host), port))
            conn.send(request)
            session_key = request[2:6]
            while time.monotonic() < deadline:
                conn.settimeout(max(0.05, deadline - time.monotonic()))
                try:
                    data = conn.recv(TORQUE_MASTER_PACKET_MAX)
                except TimeoutError:
                    break
                if len(data) < 6 or data[0] != reply_type or data[2:6] != session_key:
                    continue
                if reply_type != 8:
                    return [data]
                index, count = data[6], data[7]
                replies[index] = data
                if count and len(replies) >= count:
                    break
        return [replies[index] for index in sorted(replies)]

    def torque_master_servers(self, query, address):
        session, key = secrets.randbelow(0xffff), secrets.randbelow(0xffff)
        request = (struct.pack("<BBHHB", 6, 0, session, key, 0xff) +
                   torque_string(query["game_type"] or "any") +
                   torque_string(query["mission_type"] or "any") +
                   struct.pack("<BBIIBBH", query["min_players"], query["max_players"], query["regions"],
                               query["version"], query["info_flags"], query["max_bots"],
                               min(query["min_cpu"], 0xffff)) +
                   struct.pack("<B", min(len(query["buddies"]), 255)) +
                   b"".join(struct.pack("<I", buddy) for buddy in query["buddies"][:255]))
        servers = []
        for reply in self.torque_master_exchange(address, request, 8):
            count = struct.unpack_from("<H", reply, 8)[0]
            for offset in range(10, min(len(reply), 10 + count * 6), 6):
                servers.append((reply[offset:offset + 4], struct.unpack_from("<H", reply, offset + 4)[0]))
        return servers

    def torque_master_types(self, address):
        session, key = secrets.randbelow(0xffff), secrets.randbelow(0xffff)
        replies = self.torque_master_exchange(address, struct.pack("<BBHH", 2, 0, session, key), 4)
        if not replies:
            return [], []
        reader = Reader(replies[0])
        reader.take(6)
        game_types = [read_torque_string(reader) for _ in range(reader.number("<B"))]
        mission_types = [read_torque_string(reader) for _ in range(reader.number("<B"))]
        return game_types, mission_types

    def local_master_servers(self, query):
        now = time.monotonic()
        with self.master_lock:
            for endpoint, info in list(self.master_servers.items()):
                if now - info["last_heartbeat"] > 180:
                    del self.master_servers[endpoint]
                    self.master_sessions.pop(endpoint, None)
            servers = list(self.master_servers.values())
        result = []
        for info in servers:
            if query["game_type"] and query["game_type"].casefold() not in ("any", info["game_type"].casefold()):
                continue
            if query["mission_type"] and query["mission_type"].casefold() not in ("any", info["mission_type"].casefold()):
                continue
            players = info["player_count"]
            if players < query["min_players"]:
                continue
            if query["max_players"] not in (0, 100) and players > query["max_players"]:
                continue
            if query["max_bots"] and info["num_bots"] > query["max_bots"]:
                continue
            if query["regions"] and not (info["regions"] & query["regions"]):
                continue
            if query["info_flags"] and (info["info_flags"] & query["info_flags"]) != query["info_flags"]:
                continue
            if query["info_flags"] & 0x80 and info["version"] != query["version"]:
                continue
            if query["min_cpu"] and info["cpu_speed"] < query["min_cpu"]:
                continue
            if query["buddies"] and not set(query["buddies"]).intersection(info["players"]):
                continue
            result.append(info["endpoint"])
        return result

    def master_list(self, query):
        results = []
        for kind, address in self.master_sources:
            try:
                if kind == "lan":
                    results.extend(self.local_master_servers(query))
                elif kind == "tribesnext":
                    results.extend(self.tribesnext_servers(query, address))
                elif kind == "torque":
                    results.extend(self.torque_master_servers(query, address))
            except (OSError, ValueError, struct.error) as exc:
                self.log.event("master_upstream_error", operation="server_list", source=kind,
                               error=str(exc))
        return list(dict.fromkeys(results))

    def master_game_types(self):
        types = (set(), set())
        for kind, address in self.master_sources:
            try:
                if kind == "lan":
                    with self.master_lock:
                        local_records = [info for info in self.master_servers.values()
                                         if time.monotonic() - info["last_heartbeat"] <= 180]
                    upstream = ([record["game_type"] for record in local_records],
                                [record["mission_type"] for record in local_records])
                elif kind == "tribesnext":
                    upstream = self.tribesnext_types(address)
                else:
                    upstream = self.torque_master_types(address)
            except (OSError, ValueError, struct.error) as exc:
                self.log.event("master_upstream_error", operation="game_types", source=kind,
                               error=str(exc))
                continue
            types[0].update(value for value in upstream[0] if value)
            types[1].update(value for value in upstream[1] if value)
        return tuple(sorted(items) for items in types)

    def handle_master_datagram(self, data, peer, udp_socket):
        if len(data) < 6:
            self.log.event("unhandled", service="master_udp", peer=list(peer), payload_bytes=len(data))
            return
        packet_type, _flags, session, key = struct.unpack_from("<BBHH", data)
        try:
            if packet_type == 22:  # Game heartbeat
                with self.master_lock:
                    info = self.master_servers.get(peer)
                    if info is None:
                        info = {"endpoint": (socket.inet_aton(peer[0]), peer[1]), "game_type": "",
                                "mission_type": "", "max_players": 0, "regions": 0, "version": 0,
                                "info_flags": 0, "num_bots": 0, "cpu_speed": 0,
                                "player_count": 0, "players": []}
                        self.master_servers[peer] = info
                    info["last_heartbeat"] = time.monotonic()
                    session = secrets.randbelow(0xffff)
                    key = secrets.randbelow(0xffff)
                    self.master_sessions[peer] = (session, key, time.monotonic())
                reply = struct.pack("<BBHH", 10, 0, session, key)
                udp_socket.sendto(reply, peer)
                self.log.event("master_heartbeat", peer=list(peer), session=session)
            elif packet_type == 12:  # Game-master info response
                with self.master_lock:
                    expected = self.master_sessions.get(peer)
                if (expected is None or expected[:2] != (session, key) or
                        time.monotonic() - expected[2] > 30):
                    self.log.event("master_info_ignored", peer=list(peer), reason="session mismatch")
                    return
                reader = Reader(data)
                reader.take(6)
                game_type = read_torque_string(reader)
                mission_type = read_torque_string(reader)
                info = {"endpoint": (socket.inet_aton(peer[0]), peer[1]),
                        "game_type": game_type, "mission_type": mission_type,
                        "max_players": reader.number("<B"), "regions": reader.number("<I"),
                        "version": reader.number("<I"), "info_flags": reader.number("<B"),
                        "num_bots": reader.number("<B"), "cpu_speed": reader.number("<I")}
                info["player_count"] = reader.number("<B")
                info["players"] = [reader.number("<I") for _ in range(info["player_count"])]
                if reader.offset != len(data):
                    raise ValueError("Trailing bytes in Torque game-info response")
                info["last_heartbeat"] = time.monotonic()
                with self.master_lock:
                    self.master_servers[peer] = info
                    self.master_sessions.pop(peer, None)
                for kind, address in self.master_sources:
                    if kind == "tribesnext":
                        self.publish_tribesnext_heartbeat(peer, address)
                self.log.event("master_game_info", peer=list(peer), game_type=game_type,
                               mission_type=mission_type, players=info["player_count"])
            elif packet_type == 6:  # Server-list request
                query = parse_torque_list_request(data)
                servers = self.master_list(query)
                packet_count = max(1, (len(servers) + TORQUE_MASTER_MAX_SERVERS - 1) // TORQUE_MASTER_MAX_SERVERS)
                if packet_count > 254:
                    servers = servers[:254 * TORQUE_MASTER_MAX_SERVERS]
                    packet_count = 254
                for packet_index in range(packet_count):
                    part = servers[packet_index * TORQUE_MASTER_MAX_SERVERS:
                                   (packet_index + 1) * TORQUE_MASTER_MAX_SERVERS]
                    reply = bytearray(struct.pack("<BBHHBBH", 8, 0, session, key,
                                                  packet_index, packet_count, len(part)))
                    for address, port in part:
                        reply += address + struct.pack("<H", port)
                    udp_socket.sendto(reply, peer)
                self.log.event("master_server_list", peer=list(peer), entities=len(servers),
                               server_endpoints=[f"{socket.inet_ntoa(address)}:{port}"
                                                 for address, port in servers],
                               packets=packet_count, backend=self.master_backend)
            elif packet_type == 2:  # Game-type request
                game_types, mission_types = self.master_game_types()
                game_types = game_types[:255]
                mission_types = mission_types[:255]
                reply = bytearray(struct.pack("<BBHHB", 4, 0, session, key, len(game_types)))
                reply.extend(b"".join(torque_string(value) for value in game_types))
                reply.append(len(mission_types))
                reply.extend(b"".join(torque_string(value) for value in mission_types))
                udp_socket.sendto(reply, peer)
                self.log.event("master_game_types", peer=list(peer), game_types=len(game_types),
                               mission_types=len(mission_types), backend=self.master_backend)
            elif packet_type == 24:  # Master-server info request
                name = "TribesNext / T2Launch Master"
                region = "0"
                count = len(self.master_list({"game_type": "", "mission_type": "", "min_players": 0,
                                              "max_players": 0, "max_bots": 0, "regions": 0,
                                              "version": 0, "info_flags": 0, "min_cpu": 0,
                                              "buddies": []}))
                reply = (struct.pack("<BBHH", 26, 0, session, key) + torque_string(name) +
                         torque_string(region) + struct.pack("<H", min(count, 0xffff)))
                udp_socket.sendto(reply, peer)
                self.log.event("master_info", peer=list(peer), entities=count)
            else:
                self.log.event("unhandled", service="master_udp", peer=list(peer),
                               packet_type=packet_type, payload_bytes=len(data),
                               payload_sha256=hashlib.sha256(data).hexdigest())
        except (OSError, ValueError, KeyError, struct.error) as exc:
            self.log.event("master_protocol_error", peer=list(peer), packet_type=packet_type,
                           error=str(exc), payload_bytes=len(data))

    def start(self):
        try:
            # The master service shares one port number over UDP (the Torque
            # protocol) and TCP. Bind UDP first so an ephemeral port is taken
            # from the UDP side, and retry if that number is busy over TCP.
            for attempt in range(5):
                udp = UDPServer((self.bind_address, self.ports["master"]), MasterDatagramHandler)
                try:
                    master_tcp = TCPServer((self.bind_address, udp.server_address[1]), CaptureHandler)
                    break
                except OSError:
                    udp.server_close()
                    if self.ports["master"] or attempt == 4:
                        raise
            self.ports["master"] = udp.server_address[1]
            udp.emulator = self
            master_tcp.emulator = self
            master_tcp.service = "master"
            # Registered now so close() releases them if a later bind fails.
            self.servers += [master_tcp, udp]
            for service, port in self.ports.items():
                if service != "master":
                    handler = DirectoryHandler if service == "directory" else CaptureHandler
                    if service == "auth" and self.keys:
                        handler = AuthHandler
                    elif service == "dbproxy":
                        handler = DBProxyHandler
                    elif service == "irc" and self.keys:
                        handler = IRCHandler
                    server = TCPServer((self.bind_address, port), handler)
                    server.emulator = self
                    server.service = service
                    self.servers.append(server)
                    self.ports[service] = server.server_address[1]
            if not self.empty:
                self.records = directory_records(self.ports, self.advertise_address)
            for server in self.servers:
                thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True)
                thread.start()
                self.threads.append(thread)
        except Exception:
            self.close()
            raise
        self.log.event("ready", **self.identity())
        print(f"[wonemu] Ready on {self.bind_address}:{self.ports['directory']} (advertising "
              f"{self.advertise_address}); traffic log: {self.log.path}", flush=True)
        if not is_loopback(self.bind_address):
            print(f"[wonemu] WARNING: listening on {self.bind_address}; other machines on this "
                  "network can reach the login, community, and master services", flush=True)

    def close(self):
        for server, thread in zip(self.servers, self.threads):
            if thread.is_alive():
                server.shutdown()
        for server in self.servers:
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)
        self.servers = []
        self.threads = []
        self.log.close()


def control_host(bind_address):
    """Address the launcher uses to reach a local service bound to bind_address."""
    return HOST if bind_address in ("", "0.0.0.0") or is_loopback(bind_address) else bind_address


def health(port=DIRECTORY_PORT, bind_address=HOST):
    try:
        with socket.create_connection((control_host(bind_address), port), timeout=0.5) as conn:
            conn.sendall(frame(IDENTITY))
            payload = recv_frame(conn)
            reply = json.loads(payload)
            return reply if isinstance(reply, dict) and reply.get("identity") == IDENTITY.decode() else None
    except (OSError, ValueError, TypeError):
        return None


def ensure(args):
    if args.empty_directory:
        raise ValueError("--ensure serves a populated directory")
    keys = woncrypto.provision(args.log_dir, args.client_binary, args.auth_key_profile)
    private_store = args.private_store or args.client_binary.expanduser().resolve().parent / "base/private.store"
    community_db = args.community_db or (args.log_dir / "community.sqlite3")
    expected = {
        "ports": args.ports, "empty_directory": False, "stage": AUTH_STAGE,
        "verifier_sha256": hashlib.sha256(woncrypto.public_key(keys["root"])).hexdigest(),
        "auth_key_sha256": hashlib.sha256(woncrypto.public_key(keys["auth"])).hexdigest(),
        "implementation_sha256": IMPLEMENTATION_SHA256,
        "refresh_tribesnext": args.refresh_tribesnext,
        "account_creation": args.account_creation,
        "private_store_path_sha256": hashlib.sha256(str(private_store.resolve()).encode()).hexdigest(),
        "community_db_path_sha256": hashlib.sha256(
            str(community_db.expanduser().resolve()).encode()).hexdigest(),
        "backends": format_backends(args.backends),
        "bind_address": args.bind_address,
        "advertise_address": advertised_address(args.bind_address, args.advertise_address),
    }

    def matches(ready):
        return bool(ready) and all(ready.get(key) == value for key, value in expected.items())

    directory_port = args.ports["directory"]
    ready = health(directory_port, args.bind_address)
    if ready:
        if matches(ready):
            print(f"[wonemu] Reusing PID {ready['pid']}; logs: {args.log_dir}", flush=True)
            return
        # Only stop a service that answered our private identity probe on the
        # loopback endpoint; this handles configuration changes safely.
        with socket.create_connection((control_host(args.bind_address), directory_port), timeout=1) as conn:
            conn.sendall(frame(IDENTITY + b" STOP"))
            if recv_frame(conn) != IDENTITY + b" STOPPING":
                raise ValueError("Running wonemu has different settings; stop it before switching configuration")
        for _ in range(50):
            if health(directory_port, args.bind_address) is None:
                break
            time.sleep(0.1)
        if health(directory_port, args.bind_address) is not None:
            raise ValueError("Existing wonemu did not stop; stop it before switching configuration")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    args.log_dir.chmod(0o700)
    fd = os.open(args.log_dir / "wonemu.log", os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
    with os.fdopen(fd, "a") as log:
        child_args = [sys.executable, str(Path(__file__).resolve()), "--log-dir", str(args.log_dir),
                      "--client-binary", str(args.client_binary),
                      "--auth-key-profile", args.auth_key_profile,
                      "--private-store", str(private_store),
                      "--community-db", str(community_db),
                      "--backends", format_backends(args.backends),
                      "--bind-address", args.bind_address]
        if args.advertise_address:
            child_args += ["--advertise-address", args.advertise_address]
        for service, port in args.ports.items():
            child_args += [f"--{service}-port", str(port)]
        if args.refresh_tribesnext:
            child_args.append("--refresh-tribesnext")
        if not args.account_creation:
            child_args.append("--no-account-creation")
        process = subprocess.Popen(child_args,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    for _ in range(50):
        ready = health(directory_port, args.bind_address)
        if matches(ready):
            print(f"[wonemu] Ready (PID {ready['pid']}); logs: {args.log_dir}", flush=True)
            return
        if process.poll() is not None:
            # Another launcher may have completed the same startup concurrently.
            if matches(health(directory_port, args.bind_address)):
                return
            raise ValueError(f"wonemu failed to bind/start; see {args.log_dir / 'wonemu.log'}")
        time.sleep(0.1)
    process.terminate()
    process.wait(timeout=5)
    raise ValueError(f"wonemu readiness timeout; see {args.log_dir / 'wonemu.log'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--ensure", action="store_true", help="start/reuse the local service and wait for readiness")
    actions.add_argument("--stop", action="store_true", help="stop the identified local wonemu service")
    actions.add_argument("--reserve-local-account", metavar="USERNAME",
                         help="reserve a local login; the first game login enrolls its password")
    actions.add_argument("--generate-cd-key", action="store_true",
                         help="print a CD key that the game's Create Account dialog accepts")
    parser.add_argument("--log-dir", type=Path, default=LOG_DIR)
    parser.add_argument("--client-binary", type=Path, default=Path.home() / "t2-linux/tribes2.dynamic",
                        help="client from which to recover the public cryptographic parameters")
    parser.add_argument("--auth-key-profile", choices=("compat", "native"),
                        default=os.environ.get("T2LAUNCH_AUTH_KEY_PROFILE", "compat"),
                        help="auth-key group profile; compat avoids the client's oversized-c2 truncation")
    parser.add_argument("--private-store", type=Path,
                        help="legacy password-verification fallback (defaults to base/private.store beside the game binary)")
    parser.add_argument("--community-db", type=Path,
                        help="SQLite community profile database (defaults to community.sqlite3 under --log-dir)")
    parser.add_argument("--backends", default=os.environ.get("T2LAUNCH_WON_BACKENDS", ""),
                        help="ordered JSON list of backends, each {kind, auth, master, irc}; kinds: "
                             + ", ".join(BACKEND_KINDS) + " (default: TribesNext, then native)")
    parser.add_argument("--master-sources", default=os.environ.get("T2LAUNCH_MASTER_SOURCES", ""),
                        help="ordered master-server sources, e.g. 'tribesnext=URL,lan,torque=HOST:PORT' "
                             "(kinds: " + ", ".join(MASTER_SOURCE_KINDS) + ")")
    parser.add_argument("--master-backend", choices=("local", "tribesnext", "hybrid"),
                        default=os.environ.get("T2LAUNCH_MASTER_BACKEND", "hybrid"),
                        help="older shorthand used when --master-sources is empty; hybrid merges "
                             "TribesNext and LAN servers")
    parser.add_argument("--bind-address", default=os.environ.get("T2LAUNCH_WON_BIND", HOST),
                        help="address the services listen on (0.0.0.0 for every interface)")
    parser.add_argument("--advertise-address", default=os.environ.get("T2LAUNCH_WON_ADVERTISE", ""),
                        help="address the directory gives clients (default: the bind address, or this "
                             "machine's outward address when binding 0.0.0.0)")
    parser.add_argument("--base-port", type=int, default=int(os.environ.get("T2LAUNCH_WON_BASE_PORT") or 0),
                        help="directory port; the auth, DBProxy, IRC, and master ports follow it")
    parser.add_argument("--tribesnext-master-url",
                        default=os.environ.get("T2LAUNCH_TRIBESNEXT_MASTER_URL", TRIBESNEXT_MASTER_URL),
                        help="HTTP base URL for the TribesNext master service")
    parser.add_argument("--refresh-tribesnext", action="store_true",
                        default=os.environ.get("T2LAUNCH_REFRESH_TRIBESNEXT", "0") == "1",
                        help="refresh existing TribesNext certificate/key from the official account server on login")
    parser.add_argument("--no-account-creation", dest="account_creation", action="store_false",
                        default=os.environ.get("T2LAUNCH_WON_ACCOUNT_CREATION", "1") != "0",
                        help="refuse the game's Create Account dialog")
    parser.add_argument("--empty-directory", action="store_true", help="serve successful queries with no entities")
    for service, port in PORTS.items():
        parser.add_argument(f"--{service}-port", type=int, default=port)
    args = parser.parse_args()
    args.ports = {service: getattr(args, f"{service}_port") for service in PORTS}
    if args.base_port:
        if not 1 <= args.base_port <= 65535 - len(PORTS) + 1:
            parser.error("--base-port must leave room for five consecutive ports")
        for offset, service in enumerate(PORTS):
            if args.ports[service] == PORTS[service]:
                args.ports[service] = args.base_port + offset
    try:
        if args.backends.strip():
            args.backends = parse_backends(args.backends, args.tribesnext_master_url)
        else:
            # Older options set only the master column; auth and IRC keep defaults.
            sources = (parse_master_sources(args.master_sources, args.tribesnext_master_url)
                       if args.master_sources.strip() else
                       master_backend_sources(args.master_backend, args.tribesnext_master_url))
            args.backends = parse_backends(backends_from_master_sources(sources, args.tribesnext_master_url))
    except ValueError as exc:
        parser.error(str(exc))
    try:
        if args.generate_cd_key:
            print(woncrypto.generate_cd_key())
            return 0
        if args.reserve_local_account:
            community_db = args.community_db or (args.log_dir / "community.sqlite3")
            store = community_store.CommunityStore(community_db)
            profile = store.reserve_local_account(
                args.reserve_local_account,
                woncrypto.local_auth_user_id(args.reserve_local_account))
            username = profile["username"]
            print(f"[wonemu] Reserved local account {username}; enter its chosen password at the game login screen.",
                  flush=True)
            return 0
        if args.ensure:
            ensure(args)
            return 0
        if args.stop:
            if not health(args.ports["directory"], args.bind_address):
                raise ValueError("No identified wonemu service is listening")
            with socket.create_connection((control_host(args.bind_address), args.ports["directory"]),
                                          timeout=1) as conn:
                conn.sendall(frame(IDENTITY + b" STOP"))
                if recv_frame(conn) != IDENTITY + b" STOPPING":
                    raise ValueError("Unexpected stop response")
            print("[wonemu] Stopping local service", flush=True)
            return 0
        keys = woncrypto.provision(args.log_dir, args.client_binary, args.auth_key_profile)
        private_store = args.private_store or args.client_binary.expanduser().resolve().parent / "base/private.store"
        emulator = Emulator(args.ports, args.log_dir, args.empty_directory, keys, private_store,
                            args.master_backend, args.tribesnext_master_url, args.community_db,
                            args.refresh_tribesnext, backends=args.backends,
                            bind_address=args.bind_address, advertise_address=args.advertise_address,
                            account_creation=args.account_creation)
        def stop(_signum, _frame):
            emulator.stopped.set()
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        try:
            emulator.start()
            emulator.stopped.wait()
        finally:
            # start() closes partially bound sockets on failure.
            if emulator.servers:
                emulator.close()
        return 0
    except (OSError, ValueError) as exc:
        print(f"[wonemu] ERROR: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
