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
import itertools
import json
import os
from pathlib import Path
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

import community_store
import woncrypto

HOST = "127.0.0.1"
DIRECTORY_PORT = 15104
MAX_FRAME = 255999  # RecvMsgOp's upper bound at 0x083B717A
IDENTITY = b"T2LAUNCH-WONEMU/1"
AUTH_STAGE = "directory + local-account Auth1 + IRC auth + SQLite community DB + Torque/TribesNext master proxy"
LOG_DIR = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "t2launch/wonemu"
PORTS = {"directory": 15104, "auth": 15105, "dbproxy": 15106, "irc": 15107, "master": 15108}
TRIBESNEXT_MASTER_URL = "http://master.tribesnext.com"
TORQUE_MASTER_PACKET_MAX = 1500
TORQUE_MASTER_SERVER_SIZE = 6
TORQUE_MASTER_MAX_SERVERS = (TORQUE_MASTER_PACKET_MAX - 12) // TORQUE_MASTER_SERVER_SIZE


def implementation_fingerprint():
    digest = hashlib.sha256()
    for module in (__import__(__name__), community_store, woncrypto):
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


def service_entity(name, port, display_name="0"):
    return {"name": name, "display_name": display_name, "port": port}


def directory_records(ports):
    return {
        "/TitanServers/Auth": [service_entity("AuthServer", ports["auth"])],
        "/TitanServers/DBProxy": [service_entity("DBProxyServer", ports["dbproxy"])],
        "/Tribes2": [service_entity("IRCServer", ports["irc"]), service_entity("MasterServer", ports["master"])],
    }


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
        address = struct.pack("!H", entity["port"]) + socket.inet_aton(HOST)
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


def decode_login_envelope(payload, keys, private_store=None, account_store=None):
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
        account_enrolled = False
        if account_store is not None and account_store.local_account_exists(fields["username"]):
            account_validation, account_enrolled = account_store.verify_local_account(
                fields["username"], fields["password"])
        else:
            account_validation = woncrypto.verify_private_store_password(
                private_store, fields["username"], fields["password"])
        result.update(login_data_decryption="decoded", symmetric_key_bytes=len(symmetric_key),
                      login_plaintext_bytes=len(login_data),
                      login_plaintext_sha256=hashlib.sha256(login_data).hexdigest(),
                      login_request_kind=fields["request_kind"],
                       username_utf16_bytes=len(fields["username"].encode("utf-16le")),
                       password_utf16_bytes=len(fields["password"].encode("utf-16le")),
                       client_identifier=fields["client"], login_flags=list(fields["flags"]),
                       login_extension_bytes=fields["extension_bytes"],
                      account_validation=account_validation,
                      account_enrolled=account_enrolled)
    return result


class CaptureLog:
    def __init__(self, path):
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.parent.chmod(0o700)
        fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
        os.chmod(path, 0o600)
        self.stream = os.fdopen(fd, "a", encoding="utf-8", buffering=1)
        self.lock = threading.Lock()

    def event(self, event, **fields):
        record = {"time": datetime.datetime.now(datetime.timezone.utc).isoformat(), "event": event, **fields}
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
                # Launcher health checks identify this service, not just an open port.
                if payload == IDENTITY:
                    conn.sendall(frame(json.dumps(emulator.identity()).encode("ascii")))
                    continue
                if payload == IDENTITY + b" STOP":
                    conn.sendall(frame(IDENTITY + b" STOPPING"))
                    emulator.stopped.set()
                    break
                emulator.log.event("received", service="directory", connection=connection,
                                   peer=list(self.client_address), frame_hex=frame(payload).hex())
                try:
                    request = decode_getdir(payload)
                except ValueError as exc:
                    emulator.log.event("unhandled", service="directory", connection=connection, error=str(exc))
                    break
                emulator.log.event("getdir", connection=connection, **request)
                reply = getdir_response(request, emulator.records)
                conn.sendall(reply)
                emulator.log.event("sent", service="directory", connection=connection,
                                   path=request["path"], frames_hex=reply.hex(),
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
                emulator.log.event("received", service=service, connection=connection, chunk_hex=chunk.hex())
        except socket.timeout:
            pass
        except OSError as exc:
            emulator.log.event("connection_error", service=service, connection=connection, error=str(exc))
        emulator.log.event("capture_only", service=service, connection=connection, bytes=len(data), data_hex=data.hex())
        print(f"[wonemu] {service}: captured {len(data)} bytes; handler not implemented", flush=True)


class DBProxyHandler(socketserver.BaseRequestHandler):
    """Handle the documented WON DBProxy account-name lookup operation."""

    def handle(self):
        emulator = self.server.emulator
        connection = next(emulator.connections)
        self.request.settimeout(10)
        emulator.log.event("connected", service="dbproxy", connection=connection,
                           peer=list(self.client_address))
        try:
            while True:
                payload = recv_frame(self.request)
                if payload is None:
                    break
                reader = Reader(payload)
                header = reader.number("<B")
                service = reader.number("<H")
                message = reader.number("<H")
                submessage = reader.number("<H")
                if (header, service, message, submessage) != (5, 14, 1002, 16):
                    emulator.log.event("unhandled", service="dbproxy", connection=connection,
                                       header=header, message_type=message, submessage_type=submessage,
                                       payload_bytes=len(payload), payload_sha256=hashlib.sha256(payload).hexdigest())
                    break
                username = reader.wide()
                if reader.offset != len(payload):
                    raise ValueError("Trailing bytes in DBProxy account-name request")

                account_found = (emulator.community.local_account_exists(username) or
                                 woncrypto.private_store_contains(emulator.private_store, username))
                status = 0 if account_found else -1
                error = b"" if account_found else b"User not found"
                reply = (struct.pack("<BHHHhH", 5, 14, 1002, 17, status, len(error)) +
                         error + b"\x80")
                if account_found:
                    reply += struct.pack("<I", woncrypto.local_auth_user_id(username))
                self.request.sendall(frame(reply))
                emulator.log.event("dbproxy_account_lookup", connection=connection,
                                   account_found=account_found, status=status)
        except (OSError, ValueError) as exc:
            emulator.log.event("connection_error", service="dbproxy", connection=connection, error=str(exc))


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
        self.dbproxy_chunks = {}
        self.rejected_dbproxy_queries = set()
        challenge_sent = False
        pending_ping = None
        ping_sent_at = None
        try:
            while True:
                try:
                    chunk = self.request.recv(4096)
                except socket.timeout:
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
            if session.get("presence_registered"):
                emulator.update_irc_presence(
                    woncrypto.local_auth_user_id(session["username"]), connection, False)

    def _send(self, line):
        self.request.sendall(line.encode("utf-8"))

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
            ident = str(woncrypto.local_auth_user_id(session["username"]))
            self._send(f":wonemu.local CHALRESP_REPLY {nickname} {ident} {token}\r\n")
            session["irc_authenticated"] = True
            session["irc_nickname"] = nickname
            session["irc_ident"] = ident
            emulator.update_irc_presence(
                woncrypto.local_auth_user_id(session["username"]), connection, True)
            session["presence_registered"] = True
            emulator.log.event("irc_authenticated", connection=connection,
                               token_bytes=len(token))
            return
        if command == "PING":
            self._send(f":wonemu.local PONG :{arguments.lstrip(':')}\r\n")
        elif command == "JOIN" and session.get("irc_authenticated"):
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

        arguments = query.replace("\\n", "\n").replace("\\\\", "\\")
        rows = []
        try:
            max_rows = int(fields[2]) if len(fields) > 2 else 100
        except ValueError:
            max_rows = 100
        limit = 100 if max_rows <= 0 else min(max_rows, 100)
        supported = ordinal in {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                                15, 16, 17, 18, 19, 20, 21, 22, 23, 26, 34, 62,
                                66, 67, 68, 100,
                                24, 25, 27, 28, 29, 30, 31, 32, 33, 69}

        def require_tribe_admin(tribe_name, minimum_access=2):
            membership = emulator.community.membership(tribe_name, session["username"])
            if membership is None or membership["access"] < minimum_access:
                raise ValueError("Tribe administrator access required")
            return membership

        def require_forum_access(forum_id):
            forum = emulator.community.ensure_forum(forum_id)
            if forum["tribe_id"]:
                tribe = emulator.community.tribe_by_id(forum["tribe_id"])
                if tribe is None or emulator.community.membership(
                        tribe["name"], session["username"]) is None:
                    raise ValueError("Tribe membership required for this forum")
            return forum

        def require_forum_moderator(forum_id):
            forum = require_forum_access(forum_id)
            if not forum["tribe_id"]:
                raise ValueError("Forum moderator access required")
            tribe = emulator.community.tribe_by_id(forum["tribe_id"])
            membership = (emulator.community.membership(tribe["name"], session["username"])
                          if tribe else None)
            if membership is None or membership["access"] < 3:
                raise ValueError("Tribe moderator access required")
            return forum

        def mail_row(message):
            sender_quad = (message["sender_name"], str(message["sender_id"]),
                           message["sender_tag"], str(message["sender_appending"]))
            recipient_quad = (message["recipient_name"], str(message["recipient_id"]),
                              message["recipient_tag"], str(message["recipient_appending"]))
            body_count = max(1, len(message["body"].splitlines()))
            created = datetime.datetime.fromtimestamp(message["created_at"]).strftime(
                "%m/%d/%Y %H:%M")
            return "\t".join((str(message["mail_id"]), *sender_quad, *recipient_quad,
                               created, str(message["is_cc"]),
                               str(message["is_deleted"]), str(message["is_read"]),
                               message["to_list"], message["cc_list"], message["subject"],
                               str(body_count), message["body"]))

        def resolve_profile(username):
            username = str(username).strip()
            profile = emulator.community.get_profile(username)
            if profile is None and username.isdecimal():
                profile = emulator.community.get_profile_by_user_id(int(username))
            if profile is None and (emulator.community.local_account_exists(username) or
                                    woncrypto.private_store_contains(emulator.private_store, username)):
                profile = emulator.community.ensure_profile(
                    username, woncrypto.local_auth_user_id(username))
            return profile

        def resolve_profile_reference(reference):
            reference = str(reference).strip()
            profile = resolve_profile(reference) if reference else None
            if profile is None and (not reference or reference.isdecimal()):
                # Some legacy profile tabs pass a numeric profile metadata field
                # rather than the account name/ID. Reuse the profile most
                # recently returned on this authenticated IRC session.
                last_profile = session.get("last_profile_username")
                fallback = last_profile or session["username"]
                profile = emulator.community.get_profile(
                    fallback, woncrypto.local_auth_user_id(fallback))
                if profile is not None:
                    emulator.log.event(
                        "irc_profile_reference_fallback", connection=connection,
                        target_characters=len(reference), target_numeric=reference.isdecimal(),
                        profile_found=True)
            return profile

        def known_account_names():
            return ({name.casefold() for name in
                     woncrypto.private_store_usernames(emulator.private_store)} |
                    {name.casefold() for name in emulator.community.local_account_usernames()} |
                    {session["username"].casefold()})

        if ordinal == 1 and array_query:
            try:
                after_id = int(arguments.strip() or "0")
                messages = emulator.community.inbox(session["username"], after_id, limit)
                status, status_code = "0\t", 0
                result = str(len(messages))
                for message in messages:
                    rows.append(mail_row(message))
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "0", 1
        elif ordinal in {6, 7} and not array_query:
            try:
                mail_id = int(arguments.strip())
                if not emulator.community.update_mail(
                        session["username"], mail_id,
                        is_deleted=True if ordinal == 6 else None,
                        is_read=True if ordinal == 7 else None):
                    raise ValueError("Mail message not found")
                status, result, status_code = "0\tUpdated", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 5 and not array_query:
            query_fields = arguments.split("\t")
            if len(query_fields) < 4:
                status, result, status_code = "1\tInvalid mail request", "", 1
            else:
                to_names = [name.strip() for name in query_fields[0].split(",") if name.strip()]
                cc_names = [name.strip() for name in query_fields[1].split(",") if name.strip()]
                recipients = [(name, False) for name in to_names]
                recipients.extend((name, True) for name in cc_names)
                try:
                    known_names = known_account_names()
                    for name, _ in recipients:
                        if name.casefold() not in known_names:
                            raise ValueError(f"Player not found: {name}")
                        emulator.community.ensure_profile(name, woncrypto.local_auth_user_id(name))
                    count = emulator.community.send_mail(
                        session["username"], woncrypto.local_auth_user_id(session["username"]),
                        recipients, query_fields[2], "\t".join(query_fields[3:]),
                        ", ".join(to_names), ", ".join(cc_names))
                    status, result, status_code = "0\tMail sent", str(count), 0
                except ValueError as exc:
                    status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 0 and not array_query:
            status, result, status_code = "0\t", "Welcome to the local Tribes 2 community server.", 0
        elif ordinal in {0, 100} and array_query:
            query_fields = arguments.split("\t")
            try:
                if ordinal == 100:
                    category_id = int(query_fields[-1]) if query_fields and query_fields[-1] else 105
                    session["news_category"] = category_id
                else:
                    category_id = int(session.get("news_category", 105))
                page = int(fields[2]) if len(fields) > 2 else 0
                page = max(0, min(page, 100))
                articles, total = emulator.community.list_news(category_id, page * 23, 23)
                status, result, status_code = f"0\t\t{total}\t0", str(total), 0
                for index, article in enumerate(articles):
                    created = datetime.datetime.fromtimestamp(article["created_at"]).strftime(
                        "%m/%d/%Y %H:%M")
                    row = [str(index), str(article["article_id"]), str(article["article_id"]),
                           "1", created, str(article["updated_at"]), str(article["author_id"]),
                           "", article["author_name"], str(article["author_id"]),
                           article["author_tag"], str(article["author_appending"]),
                           str(article["category_id"]), article["title"], article["body"]]
                    rows.append("\t".join(row))
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "0", 1
        elif ordinal == 1 and not array_query:
            query_fields = arguments.split("\t")
            try:
                if len(query_fields) < 3:
                    raise ValueError("Invalid news post request")
                article_id = emulator.community.post_news(
                    query_fields[0], session["username"],
                    woncrypto.local_auth_user_id(session["username"]),
                    query_fields[1], "\t".join(query_fields[2:]))
                status, result, status_code = "0\tArticle submitted", str(article_id), 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 2 and not array_query:
            query_fields = arguments.split("\t")
            try:
                if len(query_fields) < 6 or not emulator.community.edit_news(
                        int(query_fields[1]), session["username"], int(query_fields[4]),
                        query_fields[5], "\t".join(query_fields[6:])):
                    raise ValueError("Article not found or not owned by this account")
                status, result, status_code = "0\tArticle updated", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 2 and array_query:
            blocked = emulator.community.email_block_list(session["username"])[:limit]
            status, result, status_code = "0\t", str(len(blocked)), 0
            rows.extend("\t".join((entry["username"], entry["tag"],
                                    str(entry["appending"]), str(entry["user_id"]),
                                    str(entry["blocked_count"]))) for entry in blocked)
        elif ordinal == 5 and array_query:
            target = arguments.strip() or session["username"]
            profile = resolve_profile_reference(target)
            if profile is None:
                status, result, status_code = "1\tPlayer not found", "0", 1
            else:
                buddies = emulator.community.list_buddies(profile["username"])[:limit]
                status, result, status_code = "0\t", str(len(buddies)), 0
                rows.extend("\t".join((buddy["display_name"], buddy["tag"],
                                        str(buddy["appending"]), str(buddy["user_id"]), ""))
                            for buddy in buddies)
        elif ordinal == 10 and array_query:
            tribe_name = arguments.strip()
            if emulator.community.get_tribe(tribe_name) is None:
                status, result, status_code = "1\tTribe not found", "0", 1
            else:
                # Tribe security-action rules are not persisted by this local
                # backend yet. Return a valid empty array instead of an
                # unsupported-query error for the tribe Options pane.
                status, result, status_code = "0\t", "0", 0
        elif ordinal in {10, 11} and not array_query:
            buddy_name = arguments.strip()
            try:
                known_names = known_account_names()
                if buddy_name.casefold() not in known_names:
                    raise ValueError("Player not found")
                if ordinal == 10:
                    emulator.community.add_buddy(
                        session["username"], woncrypto.local_auth_user_id(session["username"]),
                        buddy_name, woncrypto.local_auth_user_id(buddy_name))
                    status, result = "0\tBuddy added", ""
                else:
                    if not emulator.community.remove_buddy(session["username"], buddy_name):
                        raise ValueError("Buddy not found")
                    status, result = "0\tBuddy removed", ""
                status_code = 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 11 and array_query:
            tribe_name = arguments.strip()
            try:
                require_tribe_admin(tribe_name)
                invitations = emulator.community.list_invitations(tribe_name)[:limit]
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
                    rows.append("\t".join((str(invitation["invite_id"]), invite_date,
                                            *inviter_quad, *invitee_quad,
                                            str(int(invitation["status"] == "requested")), "0")))
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "0", 1
        elif ordinal == 26 and not array_query:
            emulator.community.clear_buddies(session["username"])
            status, result, status_code = "0\tBuddy list cleared", "", 0
        elif ordinal == 14 and array_query:
            messages = emulator.community.inbox(
                session["username"], 0, limit, include_deleted=True, deleted_only=True)
            status, status_code = "0\t", 0
            result = str(len(messages))
            rows.extend(mail_row(message) for message in messages)
        elif ordinal == 3 and not array_query:
            query_fields = arguments.split("\t")
            try:
                if not query_fields or not emulator.community.delete_news(
                        int(query_fields[0]), session["username"]):
                    raise ValueError("Article not found or not owned by this account")
                status, result, status_code = "0\tArticle deleted", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal in {8, 9} and not array_query:
            blocked_name = arguments.strip()
            try:
                known_names = known_account_names()
                if blocked_name.casefold() not in known_names:
                    raise ValueError("Player not found")
                emulator.community.ensure_profile(
                    blocked_name, woncrypto.local_auth_user_id(blocked_name))
                if ordinal == 9:
                    emulator.community.set_email_block(session["username"], blocked_name)
                    status, result = "0\tSender blocked", ""
                else:
                    if not emulator.community.remove_email_block(session["username"], blocked_name):
                        raise ValueError("Blocked sender not found")
                    status, result = "0\tBlock removed", ""
                status_code = 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 7 and array_query:
            forums = emulator.community.forum_list(session["username"])[:limit]
            status, status_code = "0\t", 0
            result = str(len(forums))
            rows.extend("\t".join((str(forum["forum_id"]), forum["name"],
                                    str(forum["flags"]), str(forum["tribe_id"])))
                        for forum in forums)
        elif ordinal == 8 and array_query:
            try:
                forum_id = int(arguments.strip())
                page = int(fields[2]) if len(fields) > 2 else 0
                page = page if 0 <= page <= 5 else 0
                require_forum_access(forum_id)
                topics = emulator.community.list_forum_topics(forum_id, page * 60, 60)
                all_topics = emulator.community.list_forum_topics(forum_id, 0, 100)
                status, status_code = "0\t", 0
                result = str(len(all_topics))
                for topic in topics:
                    created = datetime.datetime.fromtimestamp(topic["created_at"]).strftime(
                        "%m/%d/%Y %H:%M")
                    row = ["", str(topic["topic_id"]), topic["subject"],
                           str(topic["post_count"]), "", "", created, "",
                           topic["author_name"], "", "", "", "0", str(topic["locked"]),
                           str(topic["update_id"])]
                    rows.append("\t".join(row))
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "0", 1
        elif ordinal == 9 and array_query:
            query_fields = arguments.split("\t")
            try:
                topic_id = int(query_fields[0])
                after_update_id = int(query_fields[1]) if len(query_fields) > 1 else 0
                forum = emulator.community.forum_for_topic(topic_id)
                if forum is None:
                    raise ValueError("Forum topic not found")
                require_forum_access(forum["forum_id"])
                posts = emulator.community.list_forum_posts(topic_id, after_update_id, limit)
                status, status_code = "0\t", 0
                result = str(len(posts))
                for post in posts:
                    created = datetime.datetime.fromtimestamp(post["created_at"]).strftime(
                        "%m/%d/%Y %H:%M")
                    row = [str(int(post["author_name"].casefold() == session["username"].casefold())),
                           "", str(post["post_id"]), str(post["parent_id"]),
                           str(post["update_id"]), post["author_name"],
                           str(post["author_id"]), post["author_tag"],
                           str(post["author_appending"]), "", created, "",
                           str(post["is_deleted"]), post["subject"], post["body"]]
                    rows.append("\t".join(row))
            except (IndexError, ValueError) as exc:
                status, result, status_code = f"1\t{exc}", "0", 1
        elif ordinal in {66, 67} and not array_query:
            query_fields = arguments.split("\t")
            try:
                topic_id = int(query_fields[0])
                forum = emulator.community.forum_for_topic(topic_id)
                if forum is None:
                    raise ValueError("Forum topic not found")
                require_forum_moderator(forum["forum_id"])
                reason = "\t".join(query_fields[1:]) if ordinal == 66 else ""
                emulator.community.set_forum_topic_locked(topic_id, ordinal == 66, reason)
                status, result, status_code = (
                    ("0\tTopic locked", "", 0) if ordinal == 66 else
                    ("0\tTopic unlocked", "", 0))
            except (IndexError, ValueError) as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 68 and not array_query:
            query_fields = arguments.split("\t")
            try:
                topic_id = int(query_fields[0])
                target_forum_id = int(query_fields[1])
                source_forum = emulator.community.forum_for_topic(topic_id)
                if source_forum is None:
                    raise ValueError("Forum topic not found")
                require_forum_moderator(source_forum["forum_id"])
                if target_forum_id < 0:
                    require_forum_moderator(target_forum_id)
                emulator.community.move_forum_topic(topic_id, target_forum_id)
                status, result, status_code = "0\tTopic moved", "", 0
            except (IndexError, ValueError) as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 62 and not array_query:
            query_fields = arguments.split("\t")
            try:
                if len(query_fields) < 2 or query_fields[0].strip() != "0":
                    raise ValueError("Unsupported topic moderation action")
                topic_id = int(query_fields[1])
                forum = emulator.community.forum_for_topic(topic_id)
                if forum is None:
                    raise ValueError("Forum topic not found")
                require_forum_moderator(forum["forum_id"])
                if not emulator.community.delete_forum_topic(topic_id):
                    raise ValueError("Forum topic not found")
                status, result, status_code = "0\tTopic removed", "", 0
            except (IndexError, ValueError) as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 12 and array_query:
            target = arguments.strip()
            profile = resolve_profile_reference(target)
            if profile is None:
                status, result, status_code = "1\tPlayer not found", "0", 1
            else:
                created = datetime.datetime.fromtimestamp(profile["registered_at"]).strftime(
                    "%m/%d/%Y %H:%M")
                status, result, status_code = "0\t", "1", 0
                rows.append(f"Local community profile first seen on {created}.")
        elif ordinal == 12 and not array_query:
            query_fields = arguments.split("\t")
            if len(query_fields) < 5:
                status, result, status_code = "1\tInvalid forum post request", "", 1
            else:
                try:
                    require_forum_access(int(query_fields[0]))
                    created = emulator.community.create_forum_post(
                        int(query_fields[0]), int(query_fields[1] or "0"),
                        int(query_fields[2] or "0"), session["username"],
                        woncrypto.local_auth_user_id(session["username"]),
                        query_fields[3], "\t".join(query_fields[4:]))
                    status, result, status_code = "0\tPost created", str(created["post_id"]), 0
                except (ValueError, IndexError) as exc:
                    status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 13 and array_query:
            target = arguments.strip()
            profile = resolve_profile_reference(target)
            if profile is None:
                status, result, status_code = "1\tPlayer not found", "0", 1
            else:
                tribes = emulator.community.list_profile_tribes(profile["username"], limit)
                status, result, status_code = "0\t", str(len(tribes)), 0
                rows.extend("\t".join((tribe["name"], tribe["tag"],
                                        str(tribe["tribe_id"]), str(tribe["access"]),
                                        str(int(tribe["access"] >= 2)), tribe["title"]))
                             for tribe in tribes)
        elif ordinal == 13 and not array_query:
            query_fields = arguments.split("\t")
            try:
                post_id = int(query_fields[0])
                forum = emulator.community.forum_for_post(post_id)
                if forum is None:
                    raise ValueError("Forum post not found")
                require_forum_access(forum["forum_id"])
                if len(query_fields) < 3 or not emulator.community.edit_forum_post(
                        post_id, session["username"], query_fields[1], "\t".join(query_fields[2:])):
                    raise ValueError("Forum post not found or not owned by this account")
                status, result, status_code = "0\tPost updated", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 14 and not array_query:
            try:
                post_id = int(arguments.strip())
                forum = emulator.community.forum_for_post(post_id)
                if forum is None:
                    raise ValueError("Forum post not found")
                require_forum_access(forum["forum_id"])
                if not emulator.community.delete_forum_post(post_id, session["username"]):
                    raise ValueError("Forum post not found or not owned by this account")
                status, result, status_code = "0\tPost removed", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 23:
            requested = arguments.split("\t", 1)[0].strip()
            if not requested:
                # A missing local AuthInfo display name should still open the
                # authenticated player's own Browser profile.
                requested = session["username"]
            local_account = emulator.community.local_account_exists(requested)
            session_account = requested.casefold() == session["username"].casefold()
            legacy_account = woncrypto.private_store_contains(emulator.private_store, requested)
            found = local_account or session_account or legacy_account
            emulator.log.event("irc_profile_lookup", connection=connection,
                               requested_characters=len(requested),
                               matches_session=session_account,
                               local_account=local_account,
                               legacy_account=legacy_account,
                               profile_found=found)
            if found:
                profile = emulator.community.get_profile(
                    requested, woncrypto.local_auth_user_id(requested))
                session["last_profile_username"] = profile["username"]
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
        elif ordinal == 3 and array_query:
            query_fields = arguments.split("\t")
            search = query_fields[0].strip().casefold() if query_fields else ""
            known = {profile["username"].casefold()
                     for profile in emulator.community.search_players("", 100)}
            all_names = (set(woncrypto.private_store_usernames(emulator.private_store)) |
                         set(emulator.community.local_account_usernames()))
            for name in all_names:
                if (name.casefold() not in known and search in name.casefold()):
                    emulator.community.ensure_profile(name, woncrypto.local_auth_user_id(name))
                    known.add(name.casefold())
            matches = emulator.community.search_players(search, limit)
            result = str(len(matches))
            status_code = 0
            status = "0\t"
            for index, profile in enumerate(matches):
                # The T2 browser consumes display name from column 0 and
                # stable WON user ID from column 3 of each result row.
                rows.append("\t".join((str(index), profile["display_name"], "", "",
                                       str(profile["user_id"]))))
        elif ordinal == 4:
            query_fields = arguments.split("\t")
            search = query_fields[0].strip() if query_fields else ""
            matches = emulator.community.search_tribes(search, limit)
            result = str(len(matches))
            status_code = 0
            status = "0\t"
            for index, tribe in enumerate(matches):
                rows.append("\t".join((str(index), tribe["name"], tribe["tag"],
                                       str(tribe["member_count"]))))
        elif ordinal == 6:
            tribe_name = arguments.split("\t", 1)[0].strip()
            members = emulator.community.list_tribe_members(tribe_name, limit)
            online = emulator.online_user_ids()
            result = str(len(members))
            status_code = 0
            status = "0\t"
            for member in members:
                rows.append("\t".join((member["display_name"], "", "",
                                       str(member["user_id"]), member["title"],
                                        str(member["access"]), str(member["joined_at"]), "",
                                        str(int(member["access"] >= 2)),
                                        str(int(member["user_id"] in online)))))
        elif ordinal == 15 and array_query:
            links = (("Tribes 2 Home Page", "www.tribes2.com"),
                     ("T2 Technical Information", "sierrastudios.com/games/tribes2/support"))
            links = links[:limit]
            status, result, status_code = "0\t", str(len(links)), 0
            rows.extend("\t".join(("0", name, address)) for name, address in links)
        elif ordinal == 15 and not array_query:
            query_fields = arguments.split("\t")
            if len(query_fields) < 3:
                status, result, status_code = "1\tInvalid tribe description update", "", 1
            else:
                tribe_name = query_fields[0]
                description = "\t".join(query_fields[2:])
                try:
                    require_tribe_admin(tribe_name)
                    emulator.community.update_tribe(tribe_name, description=description)
                    status, result, status_code = "0\tUpdated", "", 0
                except ValueError as exc:
                    status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 16:
            query_fields = arguments.split("\t")
            if len(query_fields) < 6:
                status, result, status_code = "1\tInvalid tribe creation request", "", 1
            else:
                description = "\t".join(query_fields[5:])
                try:
                    emulator.community.create_tribe(
                        query_fields[0], query_fields[1], query_fields[2], query_fields[3],
                        description, session["username"],
                        woncrypto.local_auth_user_id(session["username"]))
                    status, result, status_code = "0\tCreated", "", 0
                except ValueError as exc:
                    status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 22:
            tribe_name = arguments.split("\t", 1)[0].strip()
            tribe = emulator.community.get_tribe(tribe_name)
            if tribe is None:
                status, result, status_code = "1\tTribe not found", "", 1
            else:
                status = "\t".join(("0", "", str(tribe["tribe_id"]), tribe["name"],
                                     tribe["tag"], str(tribe["appending"]),
                                     str(tribe["recruiting"]), tribe["graphic"]))
                result, status_code = tribe["description"], 0
        elif ordinal == 18:
            tribe_name = arguments.split("\t", 1)[0].strip()
            try:
                require_tribe_admin(tribe_name, minimum_access=4)
                if not emulator.community.delete_tribe(tribe_name):
                    raise ValueError("Tribe not found")
                status, result, status_code = "0\tTribe deleted", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 19:
            query_fields = arguments.split("\t")
            tribe_name = query_fields[1].strip() if len(query_fields) > 1 else ""
            member_name = query_fields[0].strip() if query_fields else ""
            try:
                actor = require_tribe_admin(tribe_name)
                target = emulator.community.membership(tribe_name, member_name)
                if target is None:
                    raise ValueError("Tribe member not found")
                if (target["user_id"] == actor["user_id"] or
                        target["access"] >= actor["access"]):
                    raise ValueError("Cannot remove an equal- or higher-ranked tribe member")
                if not emulator.community.remove_membership(tribe_name, member_name):
                    raise ValueError("Tribe member not found")
                status, result, status_code = "0\tMember removed", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 20:
            query_fields = arguments.split("\t")
            action = query_fields[0].strip().casefold() if query_fields else ""
            tribe_name = query_fields[1].strip() if len(query_fields) > 1 else ""
            value = query_fields[2].strip() if len(query_fields) > 2 else "0"
            tribe_field = {"recruiting": "recruiting", "appending": "appending"}.get(action)
            try:
                if tribe_field is None:
                    raise ValueError("Unknown tribe setting")
                require_tribe_admin(tribe_name)
                emulator.community.update_tribe(tribe_name, **{tribe_field: int(value != "0")})
                status, result, status_code = "0\tUpdated", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 21:
            query_fields = arguments.split("\t")
            tribe_name = query_fields[0].strip() if query_fields else ""
            member_name = query_fields[1].strip() if len(query_fields) > 1 else ""
            title = query_fields[2] if len(query_fields) > 2 else ""
            try:
                access = int(query_fields[3]) if len(query_fields) > 3 else 0
                actor = require_tribe_admin(tribe_name, minimum_access=3)
                target = emulator.community.membership(tribe_name, member_name)
                if target is None:
                    raise ValueError("Tribe membership not found")
                if access < 0 or access > 4:
                    raise ValueError("Tribe rank must be between 0 and 4")
                is_self = target["user_id"] == actor["user_id"]
                if ((not is_self and target["access"] >= actor["access"]) or
                        access > actor["access"] or (not is_self and access >= actor["access"])):
                    raise ValueError("Cannot modify an equal- or higher-ranked tribe member")
                emulator.community.update_membership(tribe_name, member_name,
                                                     title=title, access=access)
                status, result, status_code = "0\tMember profile updated", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 24:
            tribe_name = arguments.split("\t", 1)[0].strip()
            if emulator.community.remove_membership(tribe_name, session["username"]):
                status, result, status_code = "0\tMembership removed", "", 0
            else:
                status, result, status_code = "1\tTribe membership not found", "", 1
        elif ordinal == 25:
            primary_tribe = arguments.split("\t", 1)[0].strip()
            if primary_tribe.casefold() == "none":
                primary_tribe = ""
            elif emulator.community.membership(primary_tribe, session["username"]) is None:
                status, result, status_code = "1\tTribe membership not found", "", 1
                primary_tribe = None
            if primary_tribe is not None:
                emulator.community.update_profile(session["username"],
                                                  woncrypto.local_auth_user_id(session["username"]),
                                                  primary_tribe=primary_tribe)
                status, result, status_code = "0\tPrimary tribe updated", primary_tribe, 0
        elif ordinal == 27:
            query_fields = arguments.split("\t")
            tribe_name = query_fields[0].strip() if query_fields else ""
            invited_name = query_fields[1].strip() if len(query_fields) > 1 else ""
            try:
                require_tribe_admin(tribe_name)
                if (invited_name.casefold() != session["username"].casefold() and
                        not emulator.community.local_account_exists(invited_name) and
                        not woncrypto.private_store_contains(emulator.private_store, invited_name)):
                    raise ValueError("Player not found")
                emulator.community.invite(tribe_name, invited_name,
                                          woncrypto.local_auth_user_id(invited_name),
                                          session["username"],
                                          woncrypto.local_auth_user_id(session["username"]))
                status, result, status_code = "0\tInvitation sent", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 28:
            query_fields = arguments.split("\t")
            action = query_fields[0].strip().casefold() if query_fields else ""
            tribe_name = query_fields[1].strip() if len(query_fields) > 1 else ""
            target_name = query_fields[2].strip() if len(query_fields) > 2 else session["username"]
            try:
                if action == "cancel":
                    require_tribe_admin(tribe_name)
                    expected_status = ("pending", "requested")
                elif target_name.casefold() != session["username"].casefold():
                    require_tribe_admin(tribe_name)
                    expected_status = "requested"
                else:
                    expected_status = "pending"
                result = emulator.community.respond_to_invite(
                    action, tribe_name, target_name,
                    woncrypto.local_auth_user_id(target_name), expected_status)
                status, status_code = "0\tInvitation updated", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 34 and not array_query:
            tribe_name = arguments.strip()
            try:
                emulator.community.request_invitation(
                    tribe_name, session["username"],
                    woncrypto.local_auth_user_id(session["username"]))
                status, result, status_code = "0\tInvitation request sent", "", 0
            except ValueError as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal in {29, 30}:
            query_fields = arguments.split("\t")
            try:
                if ordinal == 29:
                    tribe_name = query_fields[0].strip()
                    graphic = "\t".join(query_fields[1:])
                else:
                    tribe = emulator.community.tribe_by_id(int(query_fields[0]))
                    if tribe is None:
                        raise ValueError("Tribe not found")
                    tribe_name = tribe["name"]
                    graphic = None
                require_tribe_admin(tribe_name, minimum_access=3)
                if ordinal == 29:
                    emulator.community.update_tribe(tribe_name, graphic=graphic)
                else:
                    emulator.community.update_tribe(tribe_name, tag="\t".join(query_fields[1:]))
                status, result, status_code = "0\tUpdated", "", 0
            except (ValueError, IndexError) as exc:
                status, result, status_code = f"1\t{exc}", "", 1
        elif ordinal == 69:
            online = emulator.online_user_ids()
            result = "".join("1" if user_id.isdecimal() and int(user_id) in online else "0"
                             for user_id in arguments.split("\t")) if arguments else ""
            status_code = 0
            status = "0\t"
        elif ordinal in {17, 31, 32, 33}:
            field = {17: "description", 31: "graphic", 32: "url", 33: "display_name"}[ordinal]
            emulator.community.update_profile(
                session["username"], woncrypto.local_auth_user_id(session["username"]),
                **{field: arguments})
            result = ""
            status_code = 0
            status = "0\tUpdated"
        else:
            status = f"1\tUnsupported DBProxy query {ordinal}"
            result = ""
            status_code = 1

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
        try:
            while True:
                payload = recv_frame(self.request)
                if payload is None:
                    break
                emulator.log.event("received", service="auth", connection=connection, frame_hex=frame(payload).hex())
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
                                       frame_hex=reply.hex(), key_block_id=1)
                    print("[wonemu] auth: sent signed public-key block", flush=True)
                else:
                    # Preserve encrypted bytes and decode only known message-3
                    # framing; successful replies require a verified local account.
                    emulator.log.event("capture_only", service="auth", connection=connection,
                                       bytes=len(payload) + 4, data_hex=frame(payload).hex(), message=message)
                    if (header, service, message) == (5, 12, 3):
                        login = None
                        encrypted_key = None
                        encrypted_data = None
                        try:
                            login = decode_login_envelope(payload, emulator.keys,
                                                          emulator.private_store, emulator.community)
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
                        except ValueError as exc:
                            emulator.log.event("login_decode_error", connection=connection, error=str(exc))
                        if login and login.get("account_validation") == "valid" and encrypted_key:
                            try:
                                session_key, login_data = woncrypto.decrypt_won_login_data(
                                    emulator.keys["auth"], encrypted_key, encrypted_data)
                                login_username = parse_login_payload(login_data, session_key)["username"]
                            except (OSError, ValueError) as exc:
                                emulator.log.event("auth_challenge_error", connection=connection, error=str(exc))
                            else:
                                challenge = secrets.token_bytes(16)
                                cipher = woncrypto.encrypt_won_crypt_transform(session_key, challenge)
                                reply = frame(struct.pack("<BHHH", 5, 12, 4, len(cipher)) + cipher)
                                self.request.sendall(reply)
                                emulator.log.event("sent", service="auth", connection=connection, message=4,
                                                   encrypted_challenge_bytes=len(cipher), frame_hex=reply.hex())
                                print("[wonemu] auth: sent encrypted challenge for local account", flush=True)
                                continue
                        # Malformed or non-local credentials receive a native
                        # AuthComplete failure; only a verified account reaches
                        # the encrypted challenge and signed success response.
                        rejection = frame(struct.pack("<BHHh", 5, 12, 6, -1))
                        self.request.sendall(rejection)
                        emulator.log.event("sent", service="auth", connection=connection,
                                           message=6, status=-1, frame_hex=rejection.hex())
                        outcome = login.get("account_validation", "not_decoded") if login else "not_decoded"
                        print(f"[wonemu] auth: explicit rejection; account validation={outcome}", flush=True)
                    elif (header, service, message) == (5, 12, 5) and session_key:
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
                                emulator.keys, session_key, login_username,
                                return_client_key=True)
                            emulator.register_auth_session(self.client_address[0], login_username,
                                                           session_key, reply, client_key)
                            self.request.sendall(frame(reply))
                            emulator.log.event("sent", service="auth", connection=connection,
                                               message=6, status=0, response_bytes=len(reply))
                            print("[wonemu] auth: completed local Auth1 login", flush=True)
                        else:
                            rejection = frame(struct.pack("<BHHh", 5, 12, 6, -1))
                            self.request.sendall(rejection)
                            emulator.log.event("sent", service="auth", connection=connection,
                                               message=6, status=-1, frame_hex=rejection.hex())
                            print("[wonemu] auth: challenge response rejected", flush=True)
                    if message != 3 and not (message == 5 and session_key):
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
                 community_db=None):
        self.ports = dict(ports or PORTS)
        self.records = {path: [] for path in directory_records(self.ports)} if empty else directory_records(self.ports)
        self.empty = empty
        self.keys = keys
        self.private_store = Path(private_store) if private_store is not None else None
        self.community = community_store.CommunityStore(
            community_db or (Path(log_dir) / "community.sqlite3"))
        self.community_db_path = self.community.path
        self.auth_sessions = {}
        self.auth_session_lock = threading.RLock()
        self.irc_presence = {}
        self.irc_presence_lock = threading.RLock()
        if master_backend not in {"local", "tribesnext", "hybrid"}:
            raise ValueError(f"Unknown master-server backend: {master_backend}")
        self.master_backend = master_backend
        self.tribesnext_master_url = tribesnext_master_url.rstrip("/")
        self.master_servers = {}
        self.master_sessions = {}
        self.master_lock = threading.RLock()
        self.log = CaptureLog(Path(log_dir) / "traffic.jsonl")
        self.stopped = threading.Event()
        self.connections = itertools.count(1)
        self.servers = []
        self.threads = []

    def register_auth_session(self, peer_ip, username, session_key, auth1_reply, client_key):
        user_id = woncrypto.local_auth_user_id(username)
        self.community.ensure_profile(username, user_id)
        with self.auth_session_lock:
            self.auth_sessions[peer_ip] = {
                "username": str(username),
                "session_key": bytes(session_key),
                "auth1_reply": bytes(auth1_reply),
                "client_key": dict(client_key),
                "created": time.time(),
            }

    def auth_session_for_peer(self, peer_ip):
        with self.auth_session_lock:
            session = self.auth_sessions.get(peer_ip)
            if session is None:
                return None
            if time.time() - session["created"] > 3600:
                del self.auth_sessions[peer_ip]
                return None
            return dict(session)

    def update_irc_presence(self, user_id, connection, online):
        with self.irc_presence_lock:
            connections = self.irc_presence.setdefault(int(user_id), set())
            if online:
                connections.add(connection)
            else:
                connections.discard(connection)
                if not connections:
                    self.irc_presence.pop(int(user_id), None)

    def online_user_ids(self):
        with self.irc_presence_lock:
            return {user_id for user_id, connections in self.irc_presence.items() if connections}

    def identity(self):
        return {"identity": IDENTITY.decode(), "ports": self.ports, "empty_directory": self.empty,
                "pid": os.getpid(),
                "stage": AUTH_STAGE if self.keys else "directory + DBProxy + Torque master proxy",
                "implementation_sha256": IMPLEMENTATION_SHA256,
                "master_backend": self.master_backend,
                "master_url_sha256": hashlib.sha256(self.tribesnext_master_url.encode()).hexdigest(),
                "verifier_sha256": hashlib.sha256(woncrypto.public_key(self.keys["root"])).hexdigest() if self.keys else None,
                "auth_key_sha256": hashlib.sha256(woncrypto.public_key(self.keys["auth"])).hexdigest() if self.keys else None,
                "private_store_path_sha256": hashlib.sha256(str(self.private_store.resolve()).encode()).hexdigest()
                if self.private_store else None,
                "community_db_path_sha256": hashlib.sha256(
                    str(self.community_db_path).encode()).hexdigest()}

    def fetch_tribesnext(self, path):
        url = self.tribesnext_master_url + path
        request = urllib.request.Request(url, headers={"User-Agent": "Tribes 2"})
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.read(1_000_000).decode("utf-8", errors="replace")

    def tribesnext_servers(self, query):
        game_type = query["game_type"] or "Any"
        mission_type = query["mission_type"] or "Any"
        unfiltered = (game_type.casefold() == "any" and mission_type.casefold() == "any" and
                      query["min_players"] == 0 and query["max_players"] in (0, 100, 255) and
                      query["max_bots"] in (0, 16) and query["regions"] in (0, 0xffffffff) and
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
        text = self.fetch_tribesnext(path)
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

    def tribesnext_types(self):
        text = self.fetch_tribesnext("/listtypes")
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

    def publish_tribesnext_heartbeat(self, peer):
        if self.master_backend not in {"tribesnext", "hybrid"}:
            return
        ip, port = peer
        path = f"/add/{port}/{urllib.parse.quote(ip, safe='')}"
        try:
            text = self.fetch_tribesnext(path)
        except (OSError, ValueError) as exc:
            self.log.event("master_upstream_error", operation="heartbeat", error=str(exc))
            return
        result = next((line.strip() for line in text.splitlines() if line.strip()), "")
        self.log.event("master_upstream_heartbeat", result=result[:32])

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
        if self.master_backend in {"tribesnext", "hybrid"}:
            try:
                results.extend(self.tribesnext_servers(query))
            except (OSError, ValueError) as exc:
                self.log.event("master_upstream_error", operation="server_list", error=str(exc))
        if self.master_backend in {"local", "hybrid"}:
            results.extend(self.local_master_servers(query))
        return list(dict.fromkeys(results))

    def master_game_types(self):
        types = (set(), set())
        if self.master_backend in {"tribesnext", "hybrid"}:
            try:
                upstream = self.tribesnext_types()
                types[0].update(upstream[0])
                types[1].update(upstream[1])
            except (OSError, ValueError) as exc:
                self.log.event("master_upstream_error", operation="game_types", error=str(exc))
        if self.master_backend in {"local", "hybrid"}:
            with self.master_lock:
                local_records = [info for info in self.master_servers.values()
                                 if time.monotonic() - info["last_heartbeat"] <= 180]
            for record in local_records:
                if record["game_type"]:
                    types[0].add(record["game_type"])
                if record["mission_type"]:
                    types[1].add(record["mission_type"])
        return tuple(sorted(items) for items in types)

    def handle_master_datagram(self, data, peer, udp_socket):
        if len(data) < 6:
            self.log.event("unhandled", service="master_udp", peer=list(peer), payload_bytes=len(data))
            return
        packet_type, flags, session, key = struct.unpack_from("<BBHH", data)
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
                self.publish_tribesnext_heartbeat(peer)
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
            for service, port in self.ports.items():
                handler = DirectoryHandler if service == "directory" else CaptureHandler
                if service == "auth" and self.keys:
                    handler = AuthHandler
                elif service == "dbproxy":
                    handler = DBProxyHandler
                elif service == "irc" and self.keys:
                    handler = IRCHandler
                server = TCPServer((HOST, port), handler)
                server.emulator = self
                server.service = service
                self.servers.append(server)
                self.ports[service] = server.server_address[1]
            udp = UDPServer((HOST, self.ports["master"]), MasterDatagramHandler)
            udp.emulator = self
            self.servers.append(udp)
            if not self.empty:
                self.records = directory_records(self.ports)
            for server in self.servers:
                thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True)
                thread.start()
                self.threads.append(thread)
        except Exception:
            self.close()
            raise
        self.log.event("ready", **self.identity())
        print(f"[wonemu] Ready on {HOST}:{self.ports['directory']}; traffic log: {self.log.path}", flush=True)

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


def health(port=DIRECTORY_PORT):
    try:
        with socket.create_connection((HOST, port), timeout=0.5) as conn:
            conn.sendall(frame(IDENTITY))
            payload = recv_frame(conn)
            reply = json.loads(payload)
            return reply if isinstance(reply, dict) and reply.get("identity") == IDENTITY.decode() else None
    except (OSError, ValueError, TypeError):
        return None


def ensure(args):
    if args.ports != PORTS or args.empty_directory:
        raise ValueError("--ensure uses the default local endpoints and populated directory")
    keys = woncrypto.provision(args.log_dir, args.client_binary, args.auth_key_profile)
    private_store = args.private_store or args.client_binary.expanduser().resolve().parent / "base/private.store"
    community_db = args.community_db or (args.log_dir / "community.sqlite3")
    fingerprint = hashlib.sha256(woncrypto.public_key(keys["root"])).hexdigest()
    auth_fingerprint = hashlib.sha256(woncrypto.public_key(keys["auth"])).hexdigest()
    private_store_fingerprint = hashlib.sha256(str(private_store.resolve()).encode()).hexdigest()
    community_db_fingerprint = hashlib.sha256(str(community_db.expanduser().resolve()).encode()).hexdigest()
    master_url_fingerprint = hashlib.sha256(args.tribesnext_master_url.rstrip("/").encode()).hexdigest()
    implementation_fingerprint = IMPLEMENTATION_SHA256
    ready = health()
    if ready:
        if (ready["ports"] == PORTS and not ready["empty_directory"] and
                ready.get("verifier_sha256") == fingerprint and
                ready.get("auth_key_sha256") == auth_fingerprint and ready.get("stage") == AUTH_STAGE and
                ready.get("implementation_sha256") == implementation_fingerprint and
                ready.get("private_store_path_sha256") == private_store_fingerprint and
                ready.get("community_db_path_sha256") == community_db_fingerprint and
                ready.get("master_backend") == args.master_backend and
                ready.get("master_url_sha256") == master_url_fingerprint):
            print(f"[wonemu] Reusing PID {ready['pid']}; logs: {args.log_dir}", flush=True)
            return
        # Only stop a service that answered our private identity probe on the
        # loopback endpoint; this handles auth-key profile upgrades safely.
        with socket.create_connection((HOST, DIRECTORY_PORT), timeout=1) as conn:
            conn.sendall(frame(IDENTITY + b" STOP"))
            if recv_frame(conn) != IDENTITY + b" STOPPING":
                raise ValueError("Running wonemu has different settings; stop it before switching configuration")
        for _ in range(50):
            if health() is None:
                break
            time.sleep(0.1)
        if health() is not None:
            raise ValueError("Existing wonemu did not stop; stop it before switching configuration")
    args.log_dir.mkdir(parents=True, exist_ok=True)
    args.log_dir.chmod(0o700)
    fd = os.open(args.log_dir / "wonemu.log", os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
    with os.fdopen(fd, "a") as log:
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--log-dir", str(args.log_dir),
                                    "--client-binary", str(args.client_binary),
                                    "--auth-key-profile", args.auth_key_profile,
                                    "--private-store", str(private_store),
                                    "--community-db", str(community_db),
                                    "--master-backend", args.master_backend,
                                    "--tribesnext-master-url", args.tribesnext_master_url],
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    for _ in range(50):
        ready = health()
        if (ready and ready["ports"] == PORTS and not ready["empty_directory"] and
                ready.get("verifier_sha256") == fingerprint and ready.get("auth_key_sha256") == auth_fingerprint and
                ready.get("stage") == AUTH_STAGE and
                ready.get("implementation_sha256") == implementation_fingerprint and
                ready.get("private_store_path_sha256") == private_store_fingerprint and
                ready.get("community_db_path_sha256") == community_db_fingerprint and
                ready.get("master_backend") == args.master_backend and
                ready.get("master_url_sha256") == master_url_fingerprint):
            print(f"[wonemu] Ready (PID {ready['pid']}); logs: {args.log_dir}", flush=True)
            return
        if process.poll() is not None:
            # Another launcher may have completed the same startup concurrently.
            ready = health()
            if (ready and ready["ports"] == PORTS and not ready["empty_directory"] and
                    ready.get("verifier_sha256") == fingerprint and ready.get("auth_key_sha256") == auth_fingerprint and
                    ready.get("stage") == AUTH_STAGE and
                    ready.get("implementation_sha256") == implementation_fingerprint and
                    ready.get("private_store_path_sha256") == private_store_fingerprint and
                    ready.get("community_db_path_sha256") == community_db_fingerprint and
                    ready.get("master_backend") == args.master_backend and
                    ready.get("master_url_sha256") == master_url_fingerprint):
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
    parser.add_argument("--log-dir", type=Path, default=LOG_DIR)
    parser.add_argument("--client-binary", type=Path, default=Path.home() / "t2-linux/tribes2.dynamic",
                        help="client from which to recover the public cryptographic parameters")
    parser.add_argument("--auth-key-profile", choices=("compat", "native"), default="compat",
                        help="auth-key group profile; compat avoids the client's oversized-c2 truncation")
    parser.add_argument("--private-store", type=Path,
                        help="legacy password-verification fallback (defaults to base/private.store beside the game binary)")
    parser.add_argument("--community-db", type=Path,
                        help="SQLite community profile database (defaults to community.sqlite3 under --log-dir)")
    parser.add_argument("--master-backend", choices=("local", "tribesnext", "hybrid"),
                        default=os.environ.get("T2LAUNCH_MASTER_BACKEND", "hybrid"),
                        help="master-server discovery sources; hybrid merges TribesNext and local LAN servers")
    parser.add_argument("--tribesnext-master-url",
                        default=os.environ.get("T2LAUNCH_TRIBESNEXT_MASTER_URL", TRIBESNEXT_MASTER_URL),
                        help="HTTP base URL for the TribesNext master service")
    parser.add_argument("--empty-directory", action="store_true", help="serve successful queries with no entities")
    for service, port in PORTS.items():
        parser.add_argument(f"--{service}-port", type=int, default=port)
    args = parser.parse_args()
    args.ports = {service: getattr(args, f"{service}_port") for service in PORTS}
    try:
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
            if not health(args.ports["directory"]):
                raise ValueError("No identified wonemu service is listening")
            with socket.create_connection((HOST, args.ports["directory"]), timeout=1) as conn:
                conn.sendall(frame(IDENTITY + b" STOP"))
                if recv_frame(conn) != IDENTITY + b" STOPPING":
                    raise ValueError("Unexpected stop response")
            print("[wonemu] Stopping local service", flush=True)
            return 0
        keys = woncrypto.provision(args.log_dir, args.client_binary, args.auth_key_profile)
        private_store = args.private_store or args.client_binary.expanduser().resolve().parent / "base/private.store"
        emulator = Emulator(args.ports, args.log_dir, args.empty_directory, keys, private_store,
                            args.master_backend, args.tribesnext_master_url, args.community_db)
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
