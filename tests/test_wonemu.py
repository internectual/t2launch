import hashlib
import json
import socket
import sqlite3
import struct
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

import community_store
import tribesnext
import woncrypto
import wonemu

# Recovered from GetDirOp::GetNextRequest and WriteBuffer in #25034.
TRIBES2_QUERY = bytes.fromhex(
    "23 00 00 00 05 02 00 67 00 22 00 00 06 00 00 08 00 "
    "2f 00 54 00 72 00 69 00 62 00 65 00 73 00 32 00 00 00"
)
AUTH_QUERY = bytes.fromhex(
    "37 00 00 00 05 02 00 67 00 37 08 02 06 00 00 12 00 "
    "2f 00 54 00 69 00 74 00 61 00 6e 00 53 00 65 00 72 00 "
    "76 00 65 00 72 00 73 00 2f 00 41 00 75 00 74 00 68 00 00 00"
)
SMALL_AUTH_P = 280113052720621736320677874810934783027
SMALL_AUTH_Q = 140056526360310868160338937405467391513
SMALL_AUTH_X = 0x123456789ABCDEF
def native_won_get_auth_info(auth2_certificate, signature_bytes):
    """Mirror Loki #25034 WONGetAuthInfo(): Auth2 unpack, 0x8348fcc, 0x8348eac."""
    body = auth2_certificate[:-signature_bytes]
    family, _, _, count = struct.unpack_from("<HIIH", body)
    assert family == 2
    offset, user_id, communities = 12, None, {}
    for _ in range(count):
        data_type, length = struct.unpack_from("<HH", body, offset)
        data = body[offset + 4:offset + 4 + length]
        offset += 4 + length
        if data_type == 0:
            user_id, = struct.unpack_from("<I", data)
        elif data_type == 1:
            community_id, blob_length = struct.unpack_from("<IH", data)
            assert len(data) == 6 + blob_length
            communities[community_id] = data[6:]
    assert offset == len(body)
    if not communities:
        return ""
    stream = communities[min(communities)]
    position = 0

    def take(size):
        nonlocal position
        chunk = stream[position:position + size]
        position += size
        return chunk

    def text(limit):
        length, = struct.unpack("<H", take(2))
        if length > limit:
            raise OverflowError
        return take(length).decode("latin-1")

    try:
        tribes = []
        for _ in range(take(1)[0]):
            tribe_id, = struct.unpack("<I", take(4))
            tag = text(15)
            append = int(take(1)[0] != 0)
            name = text(63)
            rank, = struct.unpack("<I", take(4))
            title = text(40) if rank > 9 else ""
            tribes.append((name, tag, append, tribe_id, rank, title))
        warrior = text(31)
    except OverflowError:
        return ""
    if tribes:
        lines = [f"{warrior}\t{tribes[0][1]}\t{tribes[0][2]}\t{user_id}", str(len(tribes))]
    else:
        lines = [f"{warrior}\t\t1\t{user_id}", "0"]
    lines.extend("\t".join(str(field) for field in tribe) for tribe in tribes)
    return "\n".join(lines) + "\n"


SMALL_AUTH_ENCRYPTED_KEY = bytes.fromhex(
    "01000000acf325a19af96a77fb4852da2913d469b355de39da085b28a72f0337c6f8a8b4"
)
SMALL_AUTH_ENCRYPTED_DATA = bytes.fromhex(
    "d723ae63f1623aa4c1fc770467afeb64ddc1e3cffaacef151582954ad57c5e04"
    "c9d963eaa522646e9223b18b070beb4554d1004c3526579dff6838a9f1bfc62a"
    "0724a7a62d43a113a3766b22a5ae595fbf7fef751d6ce0aeb6892f5ee28d0de0"
    "9bc588a4b4726c7e98173224e7c17168f04f55d85271c609d7e9a59af680e816"
)


def parse_service_reply(payload):
    # Independent decode of the subset at 0x08390590: header, type, name,
    # sockaddr bytes, display name. Consume the full frame to catch misalignment.
    header, service, message, status, sequence, flags, count = struct.unpack_from("<BHHhBIH", payload)
    assert (header, service, message, status) == (5, 2, 3, 0)
    offset = 14
    records = []
    for _ in range(count):
        entity = {}
        if flags & 0x10:
            assert payload[offset:offset + 1] == b"S"
            offset += 1
        if flags & 0x02000000:
            length, = struct.unpack_from("<H", payload, offset)
            offset += 2
            entity["name"] = payload[offset:offset + 2 * length].decode("utf-16le")
            offset += 2 * length
        if flags & 0x04000000:
            length = payload[offset]
            offset += 1
            assert length == 6
            port, = struct.unpack_from("!H", payload, offset)
            entity["address"] = (socket.inet_ntoa(payload[offset + 2:offset + 6]), port)
            offset += length
        if flags & 0x20:
            length, = struct.unpack_from("<H", payload, offset)
            offset += 2
            entity["display_name"] = payload[offset:offset + 2 * length].decode("utf-16le")
            offset += 2 * length
        records.append(entity)
    assert offset == len(payload)
    return sequence, flags, records


def read_dbmsg(stream):
    query_id = None
    chunks = []
    while True:
        packet = stream.readline()
        if not packet:
            raise AssertionError("Connection closed before the DBMSG final packet")
        if len(packet) > 512:
            raise AssertionError(f"IRC response exceeds the 512-byte line limit: {len(packet)}")
        line = packet.decode("utf-8").rstrip("\r\n")
        prefix, separator, chunk = line.partition(" :")
        fields = prefix.split()
        if not separator or len(fields) != 4 or fields[1] != "DBMSG":
            raise AssertionError(f"Invalid DBMSG packet: {line!r}")
        if query_id is None:
            query_id = fields[2]
        if fields[2] != query_id:
            raise AssertionError("DBMSG packet query ID changed during response")
        chunks.append(chunk)
        if fields[3] == "1":
            return f":wonemu.local DBMSG {query_id} 1 :{''.join(chunks)}"


NO_TRIBESNEXT_CHAT = [{"kind": "tribesnext", "auth": True, "master": True, "irc": False},
                      {"kind": "native", "auth": True, "master": True, "irc": True}]
TRIBESNEXT_ONLY_IRC = [{"kind": "tribesnext", "auth": True, "master": True, "irc": True},
                       {"kind": "native", "auth": True, "master": True, "irc": False}]


class ProtocolTests(unittest.TestCase):
    def test_tribesnext_session_merges_native_searches_and_lookups(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory))
            emulator.community.ensure_profile("NativePlayer", 4242)
            emulator.community.create_tribe("Native Clan", "[NC]", "0", "1", "local", "NativePlayer", 4242)
            session = {"username": "Player", "irc_authenticated": True, "community": "tribesnext",
                       "tribesnext_credentials": object(), "auth_provider": "tribesnext"}
            emulator.tribesnext_user_id = lambda username: None
            emulator.tribesnext_robot.user_search = lambda supplied, search: [
                {"name": "TNPlayer", "tag": "", "append": "0", "guid": "777"}]
            emulator.tribesnext_robot.clan_search = lambda supplied, search: []

            class Request:
                def __init__(self):
                    self.sent = []

                def sendall(self, payload):
                    self.sent.append(payload.decode("utf-8"))

            def query(text):
                handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
                handler.server = SimpleNamespace(emulator=emulator)
                handler.request = Request()
                handler._handle_dbqax(text, session, 1)
                return "".join(handler.request.sent)

            search = query("1 C3 10 :player")
            self.assertLess(search.index("TNPlayer"), search.index("NativePlayer"))  # priority order
            self.assertIn("Native Clan", query("2 C4 10 :native"))
            # An unknown TribesNext name resolves to the native profile.
            self.assertIn("NativePlayer", query("3 23 0 :NativePlayer"))
            self.assertIn("Native Clan", query("4 22 0 :Native Clan"))
            # Without native IRC, the same session stays TribesNext-only.
            strict = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory) / "strict",
                                     backends=TRIBESNEXT_ONLY_IRC)
            self.assertFalse(strict.merges_native_community(session))
            emulator.log.close()
            strict.log.close()

    def test_irc_relay_registers_queues_and_mirrors_upstream_chat(self):
        import queue
        import threading

        listener = socket.socket()
        listener.bind((wonemu.HOST, 0))
        listener.listen(1)
        received = queue.Queue()
        queued = threading.Event()

        def upstream():
            conn, _ = listener.accept()
            with conn:
                stream = conn.makefile("rb")
                lines = []
                for _ in range(3):  # NICK, USER, retried NICK
                    lines.append(stream.readline().decode().strip())
                    if len(lines) == 2:
                        conn.sendall(b":srv 433 * Lou_Cypher :Nickname is already in use\r\n")
                received.put(lines)
                # Register only once the test has queued its lines; otherwise a
                # slow test thread would send them after registration instead.
                queued.wait(5)
                conn.sendall(b":srv 001 Lou_Cypher_ :Welcome\r\n:srv 375 x :MOTD\r\nPING :tok\r\n")
                received.put([stream.readline().decode().strip() for _ in range(3)])  # queued + PONG
                conn.sendall(b":Other!u@h PRIVMSG #tribes :hello\r\n")
                received.put("sent")

        threading.Thread(target=upstream, daemon=True).start()

        class Handler:
            relay_nickname = "Lou_Cypher"

            def __init__(self):
                self.out = queue.Queue()
                self.lost = threading.Event()

            def send_raw(self, data):
                self.out.put(data.decode())

            def relay_lost(self, relay):
                self.lost.set()

        handler = Handler()
        relay = wonemu.IRCRelay(handler, f"{wonemu.HOST}:{listener.getsockname()[1]}",
                                wonemu.irc_nickname("Lou Cypher"), "4242")
        relay.relay("JOIN #tribes")
        relay.relay("PRIVMSG #tribes :hi")
        queued.set()
        self.assertEqual(received.get(timeout=5), ["NICK Lou_Cypher", "USER 4242 0 * :Tribes 2 player",
                                                   "NICK Lou_Cypher_"])
        # 001 arrives first, so the queued lines are flushed before the PING is answered.
        self.assertEqual(received.get(timeout=5), ["JOIN #tribes", "PRIVMSG #tribes :hi", "PONG :tok"])
        received.get(timeout=5)
        mirrored = [handler.out.get(timeout=5) for _ in range(2)]
        # The upstream nick change is reported, MOTD hidden, and chat mirrored.
        self.assertEqual(mirrored[0], ":Lou_Cypher!4242@localhost NICK :Lou_Cypher_\r\n")
        self.assertEqual(mirrored[1], ":Other!u@h PRIVMSG #tribes :hello\r\n")
        self.assertTrue(handler.lost.wait(5))  # the upstream closed its side
        listener.close()

    def test_irc_chat_commands_go_to_the_relay_and_the_rest_stays_local(self):
        class Relay:
            def __init__(self):
                self.lines = []

            def relay(self, line):
                self.lines.append(line)

            def close(self):
                pass

        class Request:
            def __init__(self):
                self.sent = []

            def sendall(self, data):
                self.sent.append(data.decode())

        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory))
            handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
            handler.server = SimpleNamespace(emulator=emulator)
            handler.request = Request()
            handler.relay = Relay()
            handler.dbproxy_chunks = {}
            handler.rejected_dbproxy_queries = set()
            session = {"username": "Player", "irc_authenticated": True, "irc_nickname": "Player",
                       "irc_ident": "1", "community": "native"}
            handler._handle_line("JOIN #tribes", session, 1, True)
            handler._handle_line("PRIVMSG #tribes :hello", session, 1, True)
            handler._handle_line("PING :keepalive", session, 1, True)
            handler._handle_line("dbqax 5 69 0 :1", session, 1, True)
            self.assertEqual(handler.relay.lines, ["JOIN #tribes", "PRIVMSG #tribes :hello"])
            self.assertIn(":wonemu.local PONG :keepalive\r\n", handler.request.sent)
            self.assertTrue(any("DBMSG 5" in line for line in handler.request.sent))
            emulator.log.close()
        self.assertEqual(wonemu.irc_nickname("[TN] Lou Cypher!"), "[TN]_Lou_Cypher")

    def test_session_backends_follow_irc_priority(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory))
            credentials = object()
            # Without a chat server, TribesNext IRC serves community data and chat stays local.
            self.assertEqual(emulator.session_backends("tribesnext", credentials),
                             ("tribesnext", ("native", True)))
            relaying = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory) / "c",
                                       backends=[{"kind": "tribesnext", "auth": True, "master": True,
                                                  "irc": "chat.example.net:6667"},
                                                 {"kind": "native", "auth": True, "irc": True}])
            self.assertEqual(relaying.session_backends("local"),
                             ("native", ("tribesnext", "chat.example.net:6667")))
            relaying.log.close()
            # A native login cannot use the TribesNext robot, but still gets its chat.
            self.assertEqual(emulator.session_backends("local")[0], "native")
            no_tn_irc = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory) / "b",
                                        backends=[{"kind": "tribesnext", "auth": True, "master": True},
                                                  {"kind": "native", "auth": True, "irc": True}])
            self.assertEqual(no_tn_irc.session_backends("tribesnext", credentials),
                             ("native", ("native", True)))
            self.assertEqual(no_tn_irc.auth_order, ("tribesnext", "native"))
            self.assertEqual(no_tn_irc.master_sources, [("tribesnext", wonemu.TRIBESNEXT_MASTER_URL)])
            emulator.log.close()
            no_tn_irc.log.close()

    def test_backends_parse_validate_and_map_legacy_master_sources(self):
        self.assertEqual(wonemu.parse_backends(wonemu.format_backends(wonemu.default_backends())),
                         wonemu.default_backends())
        backends = wonemu.parse_backends('[{"kind":"torque","master":"192.0.2.9:28000"},'
                                         '{"kind":"native","auth":true,"master":true,"irc":true}]')
        self.assertEqual(wonemu.backends_master_sources(backends),
                         [("torque", "192.0.2.9:28000"), ("lan", "")])
        for bad in ('[{"kind":"gopher"}]', '[{"kind":"torque","auth":true,"master":"a:1"}]',
                    '[{"kind":"native","auth":false}]', '[{"kind":"tribesnext","auth":true,"master":"ftp://x"}]',
                    '{"kind":"native"}'):
            with self.assertRaises(ValueError, msg=bad):
                wonemu.parse_backends(bad)
        legacy = wonemu.backends_from_master_sources([("lan", ""), ("torque", "192.0.2.9:28000")])
        self.assertFalse(legacy[0]["master"])
        self.assertTrue(legacy[0]["auth"] and legacy[0]["irc"])
        self.assertEqual(legacy[2], {"kind": "torque", "master": "192.0.2.9:28000"})
    def test_capture_log_rotates_one_generation_when_oversized(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "traffic.jsonl"
            path.write_text("old\n" * 10, encoding="utf-8")
            original_limit = wonemu.CaptureLog.MAX_BYTES
            wonemu.CaptureLog.MAX_BYTES = 8
            try:
                log = wonemu.CaptureLog(path)
                log.event("fresh")
                log.close()
            finally:
                wonemu.CaptureLog.MAX_BYTES = original_limit
            self.assertEqual(path.with_name("traffic.jsonl.1").read_text(), "old\n" * 10)
            self.assertEqual(json.loads(path.read_text())["event"], "fresh")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_auth_user_id_prefers_the_installed_tribes2_certificate_guid(self):
        with tempfile.TemporaryDirectory() as directory:
            private_store = Path(directory) / "private.store"
            private_store.write_text("test-user\tprivate-entry\n", encoding="ascii")
            private_store.with_name("public.store").write_text(
                "test-user\t7654321\t10001\tabcdef\tsignature\n", encoding="ascii")
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory), private_store=private_store)

            self.assertEqual(emulator.auth_user_id("TEST-USER"), 7654321)

    def test_auth1_reply_uses_the_supplied_tribes2_user_id(self):
        keys = {"root": {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                         "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)},
                "auth": {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                         "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}}
        session_key = bytes.fromhex("0102030405060708")
        reply = woncrypto.auth1_login_reply(keys, session_key, "test-user", now=1700000000,
                                            user_id=7654321)
        auth2 = woncrypto.auth1_login_material(reply)["auth2_certificate"]
        self.assertEqual(struct.unpack_from("<I", auth2, 16)[0], 7654321)

    def test_auth1_reply_carries_native_won_auth_info(self):
        keys = {"root": {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                         "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)},
                "auth": {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                         "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}}
        signature_bytes = 2 * ((SMALL_AUTH_Q.bit_length() + 7) // 8)
        session_key = bytes.fromhex("0102030405060708")
        tribes = [{"id": 77, "name": "Live Clan", "tag": "[LC]", "append": 1, "rank": 4,
                   "title": "Founder"},
                  {"id": 78, "name": "Titled", "tag": "TT", "append": 0, "rank": 12,
                   "title": "Overlord"}]
        reply = woncrypto.auth1_login_reply(keys, session_key, "Player", now=1700000000,
                                            user_id=7654321, tribes=tribes)
        auth2 = woncrypto.auth1_login_material(reply)["auth2_certificate"]
        self.assertTrue(woncrypto.verify(keys["auth"], auth2[:-signature_bytes],
                                         auth2[-signature_bytes:]))
        self.assertEqual(native_won_get_auth_info(auth2, signature_bytes),
                         "Player\t[LC]\t1\t7654321\n2\n"
                         "Live Clan\t[LC]\t1\t77\t4\t\n"
                         "Titled\tTT\t0\t78\t12\tOverlord\n")

        reply = woncrypto.auth1_login_reply(keys, session_key, "Loner", now=1700000000,
                                            user_id=7654321)
        auth2 = woncrypto.auth1_login_material(reply)["auth2_certificate"]
        self.assertEqual(native_won_get_auth_info(auth2, signature_bytes),
                         "Loner\t\t1\t7654321\n0\n")

    def test_auth_info_rejects_values_the_client_cannot_hold(self):
        tribe = {"id": 1, "name": "Clan", "tag": "C", "append": 0, "rank": 1, "title": ""}
        woncrypto.t2_auth_info_blob("w" * 31, [tribe] * woncrypto.T2_AUTH_INFO_MAX_TRIBES)
        with self.assertRaises(ValueError):
            woncrypto.t2_auth_info_blob("Player", [tribe] * 11)
        with self.assertRaises(ValueError):
            woncrypto.t2_auth_info_blob("w" * 32)
        with self.assertRaises(ValueError):
            woncrypto.t2_auth_info_blob("Player", [dict(tribe, tag="t" * 16)])
        with self.assertRaises(ValueError):
            woncrypto.t2_auth_info_blob("Player", [dict(tribe, name="n" * 64)])
        with self.assertRaises(ValueError):
            woncrypto.t2_auth_info_blob("Player", [dict(tribe, rank=10, title="t" * 41)])

    def test_generated_cd_keys_pass_the_clients_offline_check(self):
        keys = {woncrypto.generate_cd_key() for _ in range(200)}
        self.assertGreater(len(keys), 190)
        for key in keys:
            self.assertRegex(key, r"^([A-Z0-9]{4}-){4}[0-9]{4}$")
            self.assertTrue(woncrypto.cd_key_is_valid(key))
            # buildCDKey() uppercases; the client also skips spaces and tabs.
            self.assertTrue(woncrypto.cd_key_is_valid(key.lower().replace("-", " ")))
        # The check byte is 8 bits, so about 1 key in 256 also fits another product.
        self.assertLess(sum(woncrypto.cd_key_is_valid(key, product="Homeworld") for key in keys), 10)
        key = next(iter(keys))
        # The third group's vowel holds bits 30-31, inside check byte 3 only.
        vowel = key[11]
        for other in woncrypto.CD_KEY_ALPHABETS["V"].replace(vowel, ""):
            self.assertFalse(woncrypto.cd_key_is_valid(key[:11] + other + key[12:]))
        for bad in ("", key[:-1], key + "2", key.replace(key[0], "E", 1), key[:-1] + "1"):
            self.assertFalse(woncrypto.cd_key_is_valid(bad))

    def test_auth_info_tribes_come_from_the_login_provider(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory))
            emulator.community.ensure_profile("Player", 7654321)
            emulator.community.create_tribe("Local Clan", "[LC]", 1, 0, "", "Player", 7654321)
            local = emulator.auth_info_tribes("player")
            self.assertEqual([(tribe["name"], tribe["tag"], tribe["append"], tribe["rank"])
                              for tribe in local],
                             [("Local Clan", "[LC]", 1, 4)])

            credentials = object()
            emulator.tribesnext_user_id = lambda username: 123456
            emulator.tribesnext_robot.user_view = lambda supplied, guid: {"memberships": [
                {"id": "77", "name": "Live Clan", "rank": "3", "title": "Lead",
                 "tag": "[TN]", "append": "1"},
                {"id": "78", "name": "n" * 64, "rank": "1", "title": "", "tag": "",
                 "append": "0"}]}
            remote = emulator.auth_info_tribes("Player", "tribesnext", credentials)
            self.assertEqual(remote, [{"id": 77, "name": "Live Clan", "tag": "[TN]",
                                       "append": 1, "rank": 3, "title": "Lead"}])

            def unavailable(supplied, guid):
                raise tribesnext.RobotAPIError("offline")
            emulator.tribesnext_robot.user_view = unavailable
            self.assertEqual(emulator.auth_info_tribes("Player", "tribesnext", credentials), [])
            self.assertEqual(emulator.auth_info_tribes("Player", "tribesnext", None), [])

    def test_tribesnext_guid_is_resolved_from_public_store(self):
        with tempfile.TemporaryDirectory() as directory:
            private_store = Path(directory) / "private.store"
            private_store.write_text("player\tprivate-value\n", encoding="ascii")
            private_store.with_name("public.store").write_text(
                "Player\t123456\t10001\tabcdef\tsignature\n", encoding="ascii")
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory), private_store=private_store)
            self.assertEqual(emulator.tribesnext_user_id("PLAYER"), 123456)
            self.assertIsNone(emulator.tribesnext_user_id("missing"))

    def test_dbproxy_profile_and_membership_reads_use_tribesnext_response(self):
        with tempfile.TemporaryDirectory() as directory:
            # Native IRC unchecked: TribesNext is the only community backend.
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory), backends=TRIBESNEXT_ONLY_IRC)
            credentials = object()
            session = {"username": "Player", "irc_authenticated": True,
                       "tribesnext_credentials": credentials, "auth_provider": "tribesnext"}
            profile = {"guid": 123456, "name": "LiveName", "tag": "[TN]", "append": "1",
                       "creation": "1234567890", "website": "https://example.invalid",
                       "info": "authoritative text", "online": "1",
                       "memberships": [{"id": "77", "name": "Live Clan", "rank": "4",
                                        "title": "Founder", "tag": "[LC]", "append": "0"}]}
            emulator.tribesnext_user_id = lambda username: 123456
            emulator.tribesnext_robot.user_view = lambda supplied, guid: profile
            emulator.tribesnext_robot.user_search = lambda supplied, search: [
                {"index": "0", "name": "LiveName", "tag": "[TN]", "append": "1",
                 "guid": "123456"}]
            emulator.tribesnext_robot.clan_search = lambda supplied, search: [
                {"index": "0", "id": "77", "name": "Live Clan"}]
            remote_clan = {"id": "77", "name": "Live Clan", "tag": "[LC]", "append": "0",
                           "recruiting": "1", "picture": "team.png", "info": "live clan info",
                           "members": [{"name": "Member", "tag": "[LC]", "append": "0",
                                        "guid": "654321", "rank": "4", "title": "Leader",
                                        "online": "1"}]}
            emulator.tribesnext_robot.clan_view = lambda supplied, clan_id: remote_clan

            class Request:
                def __init__(self):
                    self.sent = []

                def sendall(self, payload):
                    self.sent.append(payload.decode("utf-8"))

            handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
            handler.server = SimpleNamespace(emulator=emulator)
            handler.request = Request()
            handler._handle_dbqax("10 23 0 :Player", session, 1)
            profile_reply = "".join(handler.request.sent)
            self.assertIn("LiveName", profile_reply)
            self.assertIn("authoritative text", profile_reply)
            self.assertIn("https://example.invalid", profile_reply)

            profile["creation"] = "invalid timestamp"
            handler.request.sent.clear()
            handler._handle_dbqax("18 23 0 :Player", session, 1)
            malformed_date_reply = "".join(handler.request.sent)
            self.assertIn("LiveName", malformed_date_reply)
            self.assertIn("DBMSG 18 1 :0", malformed_date_reply)
            profile["creation"] = "1234567890"

            handler.request.sent.clear()
            handler._handle_dbqax("11 C13 10 :Player", session, 1)
            membership_reply = "".join(handler.request.sent)
            self.assertIn("Live Clan", membership_reply)
            self.assertIn("77", membership_reply)
            self.assertIn("Founder", membership_reply)

            handler.request.sent.clear()
            handler._handle_dbqax("12 C3 10 :Live", session, 1)
            player_search = "".join(handler.request.sent)
            self.assertIn("LiveName", player_search)
            self.assertIn("123456", player_search)

            handler.request.sent.clear()
            handler._handle_dbqax("13 4 10 :Live", session, 1)
            clan_search = "".join(handler.request.sent)
            self.assertIn("Live Clan", clan_search)

            handler.request.sent.clear()
            def unavailable_clan_search(supplied, search):
                raise tribesnext.RobotAPIError("offline")

            emulator.tribesnext_robot.clan_search = unavailable_clan_search
            emulator.community.search_tribes = lambda *args, **kwargs: self.fail(
                "TribesNext clan search must not fall back to local records")
            handler._handle_dbqax("13 4 10 :Live", session, 1)
            unavailable_clan_search_reply = "".join(handler.request.sent)
            self.assertIn("TribesNext search unavailable", unavailable_clan_search_reply)
            self.assertNotIn("Live Clan", unavailable_clan_search_reply)
            emulator.tribesnext_robot.clan_search = lambda supplied, search: [
                {"index": "0", "id": "77", "name": "Live Clan"}]
            del emulator.community.search_tribes

            handler.request.sent.clear()
            handler._handle_dbqax("14 22 0 :Live Clan", session, 1)
            clan_profile = "".join(handler.request.sent)
            self.assertIn("live clan info", clan_profile)
            self.assertIn("team.png", clan_profile)

            handler.request.sent.clear()
            emulator.tribesnext_robot.clan_search = lambda supplied, search: []
            emulator.community.get_tribe = lambda *args, **kwargs: self.fail(
                "A missing TribesNext clan must not fall back to local records")
            handler._handle_dbqax("141 22 0 :Missing Clan", session, 1)
            missing_clan_reply = "".join(handler.request.sent)
            self.assertIn("Tribe not found", missing_clan_reply)
            del emulator.community.get_tribe
            emulator.tribesnext_robot.clan_search = lambda supplied, search: [
                {"index": "0", "id": "77", "name": "Live Clan"}]

            handler.request.sent.clear()
            handler._handle_dbqax("15 C6 10 :Live Clan", session, 1)
            clan_roster = "".join(handler.request.sent)
            self.assertIn("Member", clan_roster)
            self.assertIn("654321", clan_roster)
            self.assertIn("Leader", clan_roster)

            handler.request.sent.clear()
            handler._handle_dbqax("16 17 0 :local-only edit", session, 1)
            rejected_write = "".join(handler.request.sent)
            self.assertIn("TribesNext community operation unavailable", rejected_write)

            handler.request.sent.clear()
            def unavailable_profile(supplied, guid):
                raise tribesnext.RobotAPIError("offline")

            emulator.tribesnext_robot.user_view = unavailable_profile
            handler._handle_dbqax("17 23 0 :Player", session, 1)
            unavailable_profile = "".join(handler.request.sent)
            self.assertIn("TribesNext profile unavailable", unavailable_profile)
            self.assertNotIn("authoritative text", unavailable_profile)

    def test_dbproxy_history_buddies_and_picture_use_tribesnext_robot(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory))
            credentials = SimpleNamespace(certificate=SimpleNamespace(guid=123456))
            session = {"username": "Player", "irc_authenticated": True,
                       "tribesnext_credentials": credentials, "auth_provider": "tribesnext"}
            emulator.community.ensure_profile = lambda *args, **kwargs: self.fail(
                "TribesNext queries must not seed local profiles")
            emulator.tribesnext_user_id = lambda username: None
            robot = emulator.tribesnext_robot
            buddies = [{"name": "Friend", "tag": "[F]", "append": "0", "guid": "654321"}]
            changes = []
            robot.buddy_list = lambda supplied: list(buddies)
            robot.add_buddy = lambda supplied, guid: changes.append(("add", guid))
            robot.remove_buddy = lambda supplied, guid: changes.append(("remove", guid))
            robot.user_search = lambda supplied, search: [
                {"index": "0", "name": "Friendly", "tag": "", "append": "0", "guid": "777"},
                {"index": "1", "name": "Friend", "tag": "[F]", "append": "0", "guid": "654321"}]
            history_guids = []

            def user_history(supplied, guid):
                history_guids.append(guid)
                return [{"type": "JOIN", "time": "1200000000", "payload": "",
                         "template": "@player joined @clan",
                         "player": ["Player", "", "0", "123456"],
                         "clan": ["77", "Old Clan", "", "0"]},
                        {"type": "JOIN", "time": "1300000000", "payload": "",
                         "template": "@player joined @clan",
                         "player": ["Player", "", "0", "123456"],
                         "clan": ["78", "New Clan", "", "0"]}]

            robot.user_history = user_history
            viewed_guids = []
            robot.user_view = lambda supplied, guid: (
                viewed_guids.append(guid) or {"guid": guid, "name": "Player", "memberships": []})

            class Request:
                def __init__(self):
                    self.sent = []

                def sendall(self, payload):
                    self.sent.append(payload.decode("utf-8"))

            handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
            handler.server = SimpleNamespace(emulator=emulator)
            handler.request = Request()

            def query(text):
                handler.request.sent.clear()
                handler._handle_dbqax(text, session, 1)
                return "".join(handler.request.sent)

            history = query("20 C12 0 :Player")
            self.assertIn("DBMSG 20 1 :0\t\\S2\\S", history)
            self.assertLess(history.index("New Clan"), history.index("Old Clan"))
            self.assertEqual(history_guids, [123456])
            query("21 C12 0 :Friend")
            self.assertEqual(history_guids, [123456, 654321])
            self.assertIn("Player not found", query("22 C12 0 :Nobody"))

            buddy_list = query("23 C5 0 :Player")
            self.assertIn("Friend\t[F]\t0\t654321\t", buddy_list)
            self.assertIn(":0\t\\S1\\S", query("24 C5 0 :Player"))
            self.assertIn(":0\t\\S0\\S", query("25 C5 0 :Somebody Else"))

            self.assertIn("Friend", query("26 10 0 :Friend"))
            self.assertIn("Buddy removed", query("27 11 0 :friend"))
            self.assertIn("Buddy not found", query("28 11 0 :Stranger"))
            self.assertIn("Buddy list cleared", query("29 26 0 :clearBuddy"))
            self.assertEqual(changes, [("add", 654321), ("remove", 654321),
                                       ("remove", "654321")])

            self.assertIn("Updated", query("30 31 0 :texticons/twb/twb_Blaster.jpg"))
            self.assertEqual(emulator.community.tribesnext_picture(123456),
                             "texticons/twb/twb_Blaster.jpg")
            self.assertIn("twb_Blaster.jpg", query("31 23 0 :123456"))

            # A searched player is not in the local public store; the profile and
            # membership views resolve the name through the robot's player search.
            viewed_guids.clear()
            self.assertIn("DBMSG 40 1 :0\t", query("40 23 0 :Friend"))
            self.assertIn(":0\t\\S0\\S", query("41 C13 0 :Friend"))
            self.assertEqual(viewed_guids, [654321, 654321])
            self.assertIn("Player not found", query("42 23 0 :Nobody"))
            self.assertIn("Player not found", query("43 C13 0 :Nobody"))
            self.assertNotIn("unavailable", query("44 23 0 :Nobody"))

            def offline(*args):
                raise tribesnext.RobotAPIError("offline")

            robot.buddy_list = offline
            self.assertIn("TribesNext service unavailable", query("32 C5 0 :Player"))

    def test_dbproxy_tmail_and_block_list_use_tribesnext_mail_robot(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory))
            credentials = SimpleNamespace(certificate=SimpleNamespace(guid=123456))
            session = {"username": "Player", "irc_authenticated": True,
                       "tribesnext_credentials": credentials, "auth_provider": "tribesnext"}
            emulator.community.ensure_profile = lambda *args, **kwargs: self.fail(
                "TribesNext mail must not seed local profiles")
            emulator.community.inbox = lambda *args, **kwargs: self.fail(
                "TribesNext mail must not read the local mailbox")
            emulator.tribesnext_user_id = lambda username: None
            robot = emulator.tribesnext_robot
            requests = []

            def mail_get(supplied, method, **args):
                requests.append((method, args))
                if method == "box" and args["box"] == "inbox":
                    # Newest first, as the robot sorts by sent time.
                    return ["MSG\t12\tinbox\tfalse\tmessage\t1300000000",
                            "SBJ\t12\tNew\\tsubject", "SNDR\t12\tFriend\t[F]\t0\t654321",
                            "MSG\t11\tinbox\ttrue\tmessage\t1200000000",
                            "SBJ\t11\tOlder", "SNDR\t11\tFriend\t[F]\t0\t654321",
                            "MSG\t10\tinbox\ttrue\tmessage\t1100000000",
                            "SBJ\t10\tSeen", "SNDR\t10\tFriend\t[F]\t0\t654321"]
                if method == "box":
                    return [""]
                if method == "read":
                    message_id = args["id"]
                    return [f"MSG2\t{message_id}\tfalse\tmessage\t1300000000",
                            f"SBJ\t{message_id}\tSubject {message_id}",
                            f"SNDR\t{message_id}\tFriend\t[F]\t0\t654321",
                            f"TO\t{message_id}\t0\tPlayer\t\t0\t123456",
                            f"CC\t{message_id}\t0\tOther\t\t0\t777",
                            f"BDY\t{message_id}\tline one\\nline two"]
                if method == "viewlist":
                    return ["LIST\tignore\t0\tPest\t\t0\t999"]
                return [""]

            posts = []
            robot.mail_get = mail_get
            robot.mail_post = lambda supplied, method, **fields: (
                posts.append((method, fields)) or ["SUCCESS\t13"])
            robot.user_search = lambda supplied, search: [
                {"index": "0", "name": search, "tag": "", "append": "0",
                 "guid": {"Friend": "654321", "Other": "777", "Pest": "999"}.get(search, "1")}
                for _ in [0] if search in {"Friend", "Other", "Pest"}]

            class Request:
                def __init__(self):
                    self.sent = []

                def sendall(self, payload):
                    self.sent.append(payload.decode("utf-8"))

            handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
            handler.server = SimpleNamespace(emulator=emulator)
            handler.request = Request()

            def query(text):
                handler.request.sent.clear()
                handler._handle_dbqax(text, session, 1)
                return "".join(handler.request.sent)

            inbox = query("50 C1 0 :10")
            self.assertIn("DBMSG 50 1 :0\t\\S2\\S", inbox)
            self.assertNotIn("Subject 10", inbox)
            self.assertLess(inbox.index("Subject 11"), inbox.index("Subject 12"))
            self.assertIn("12\tFriend\t[F]\t0\t654321\tPlayer\t\t0\t123456\t", inbox)
            self.assertIn("\t0\t0\t0\tPlayer\tOther\tSubject 12\t2\tline one\\nline two", inbox)
            self.assertIn("\t0\t0\t1\tPlayer\tOther\tSubject 11\t", inbox)
            self.assertEqual(requests[0], ("box", {"box": "inbox", "first": 0,
                                                   "last": wonemu.TRIBESNEXT_MAIL_PAGE,
                                                   "since": 11}))
            self.assertEqual([args["id"] for method, args in requests if method == "read"],
                             [11, 12])
            self.assertIn("You have no deleted messages", query("51 C14 100 :getDeletedMail"))

            self.assertIn("Mail sent", query("52 5 0 :Friend, Other\t\tHello\tbody\ttext"))
            self.assertEqual(posts, [("send", {"subject": "Hello", "contents": "body\ttext",
                                               "to": "654321,777", "cc": "654321"})])
            self.assertIn("Player not found: Nobody", query("53 5 0 :Nobody\t\tHello\tbody"))
            self.assertIn("A To recipient is required", query("54 5 0 :\tFriend\tHello\tbody"))
            robot.mail_post = lambda *args, **kwargs: ["ERR\tMAIL\tINVALID_SBJ"]
            self.assertIn("A subject is required", query("55 5 0 :Friend\t\t\tbody"))

            requests.clear()
            self.assertIn("Updated", query("56 6 0 :12"))
            self.assertIn("Updated", query("57 7 0 :12"))
            self.assertIn("Updated", query("58 35 0 :12"))
            self.assertIn("Mail message not found", query("59 6 0 :bogus"))
            self.assertEqual(requests, [("delete", {"id": 12, "set": 1}),
                                        ("read", {"id": 12})])

            self.assertIn("Pest\t\t0\t999\t0", query("60 C2 0 :"))
            requests.clear()
            self.assertIn("Sender blocked", query("61 9 0 :Friend"))
            self.assertIn("Block removed", query("62 8 0 :pest"))
            self.assertIn("Blocked sender not found", query("63 8 0 :Friend"))
            self.assertEqual([(method, args.get("target")) for method, args in requests
                              if method != "viewlist"],
                             [("addlist", 654321), ("dellist", 999)])

    def test_tribesnext_invite_mail_links_and_answers_use_robot(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory))
            credentials = SimpleNamespace(certificate=SimpleNamespace(guid=123456))
            session = {"username": "Player", "irc_authenticated": True,
                       "tribesnext_credentials": credentials, "auth_provider": "tribesnext"}
            future = int(time.time()) + 86400
            message = {"id": 5, "type": "invite", "deleted": False, "time": "1300000000",
                       "subject": "Invitation to: Live Clan", "to": [], "cc": [],
                       "sender": ["Leader", "[LC]", "0", "654321"],
                       "body": json.dumps({"sguid": "654321", "clanid": 77,
                                           "expire": future})}
            header = {"id": 5, "read": False, "time": "1300000000", "type": "invite"}
            recipient = ("Player", "", "0", "123456")
            row = wonemu.tribesnext_mail_row(header, message, recipient)
            self.assertIn("[LC]Leader has invited you to join Live Clan.", row)
            self.assertIn("<a:acceptinvite\t77\tPlayer>Accept invitation</a>", row)
            self.assertIn("<a:rejectinvite\t77\tPlayer>Reject invitation</a>", row)
            message["body"] = json.dumps({"sguid": "654321", "clanid": 77, "expire": 1400000000})
            expired = wonemu.tribesnext_mail_row(header, message, recipient)
            self.assertIn("This invitation expired on", expired)
            self.assertNotIn("acceptinvite", expired)
            message["body"] = "not json"
            self.assertIn("\tnot json", wonemu.tribesnext_mail_row(header, message, recipient))

            robot = emulator.tribesnext_robot
            answers = []
            robot.browser_get = lambda supplied, method, **args: (
                answers.append((method, args)) or ["ACCEPTED\t77"])
            robot.clan_search = lambda supplied, search: [
                {"index": "0", "id": "78", "name": "Other Clan"}]

            class Request:
                def __init__(self):
                    self.sent = []

                def sendall(self, payload):
                    self.sent.append(payload.decode("utf-8"))

            handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
            handler.server = SimpleNamespace(emulator=emulator)
            handler.request = Request()

            def query(text):
                handler.request.sent.clear()
                handler._handle_dbqax(text, session, 1)
                return "".join(handler.request.sent)

            self.assertIn("DBMSG 70 1 :0\t\\S0\\S", query("70 C10 20 :Live Clan"))
            self.assertIn("Invitation updated", query("71 28 0 :accept\t77\tPlayer"))
            self.assertIn("Invitation updated", query("72 28 0 :reject\tother clan\tPlayer"))
            self.assertIn("Tribe not found", query("73 28 0 :accept\tMissing\tPlayer"))
            self.assertIn("cannot be retracted", query("74 28 0 :cancel\t77\tPlayer"))
            self.assertEqual(answers, [("uaccept", {"a0": 77}), ("ureject", {"a0": 78})])
            robot.browser_get = lambda *args, **kwargs: [
                "ERR\tBROWSER\tNo valid invitation found."]
            self.assertIn("No valid invitation found.", query("75 28 0 :accept\t77\tPlayer"))

    def test_tribesnext_auth_session_does_not_create_local_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory))
            emulator.community.ensure_profile = lambda *args, **kwargs: self.fail(
                "TribesNext authentication must not seed an unverified local profile")
            credentials = object()
            emulator.register_auth_session("127.0.0.1", "Player", b"session",
                                           b"auth1", {"x": 1}, credentials, "tribesnext")
            session = emulator.auth_session_for_peer("127.0.0.1")
            self.assertEqual(session["auth_provider"], "tribesnext")
            self.assertIs(session["tribesnext_credentials"], credentials)

    def test_tribesnext_auth_unlocks_stored_key_for_authoritative_profile_lookup(self):
        username = "WONEMUXPROBE"
        password = "probe_not_a_real_password"
        private_hex = "0ac1"
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        store_key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
        encrypted_private = woncrypto._rc4_drop2048(store_key, bytes.fromhex(private_hex)).hex()
        auth_key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4, "x": SMALL_AUTH_X,
                    "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        keys = {"root": auth_key, "auth": auth_key}

        with tempfile.TemporaryDirectory() as directory:
            private_store = Path(directory) / "private.store"
            private_store.write_text(f"{username}\t{nonce}:{encrypted_private}\n", encoding="ascii")
            private_store.with_name("public.store").write_text(
                f"{username}\t7654321\t11\tca1\t1\n", encoding="ascii")
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory), keys=keys, private_store=private_store)
            robot_requests = []

            def robot_request(endpoint, params=None, body=None, content_type=None):
                robot_requests.append((endpoint, params))
                if endpoint == "robot_login.php" and "nonce" in params:
                    challenge = int(params["nonce"] + "2", 16)
                    encrypted = pow(challenge, 17, 0xCA1)
                    return [f"CHALLENGE: {encrypted:x}"]
                if endpoint == "robot_login.php" and "response" in params:
                    self.assertEqual(params["response"], "2")
                    return ["UUID: robot-session"]
                if endpoint == "robot_browser.php":
                    self.assertEqual(params["uuid"], "robot-session")
                    self.assertEqual(params["method"], "uview")
                    self.assertEqual(params["a0"], "7654321")
                    return ["PLAYER\tNAME\t7654321\tVerifiedName",
                            "PLAYER\tDATE\t7654321\t1234567890",
                            "PLAYER\tSITE\t7654321\thttps://verified.invalid",
                            "PLAYER\tINFO\t7654321\tverified profile"]
                self.fail(f"Unexpected TribesNext robot request: {endpoint}")

            emulator.tribesnext_robot._request = robot_request
            emulator.start()
            try:
                session_key = bytes.fromhex("0102030405060708")
                with socket.create_connection((wonemu.HOST, emulator.ports["auth"]),
                                              timeout=2) as connection:
                    connection.sendall(wonemu.frame(bytes.fromhex("05 0c 00 01 00")))
                    wonemu.recv_frame(connection)
                    login = (bytes.fromhex("05 0c 00 03 00") +
                             struct.pack("<HH", 1, len(SMALL_AUTH_ENCRYPTED_KEY)) +
                             SMALL_AUTH_ENCRYPTED_KEY +
                             struct.pack("<H", len(SMALL_AUTH_ENCRYPTED_DATA)) +
                             SMALL_AUTH_ENCRYPTED_DATA)
                    connection.sendall(wonemu.frame(login))
                    challenge = wonemu.recv_frame(connection)
                    challenge_length, = struct.unpack_from("<H", challenge, 5)
                    self.assertEqual(len(woncrypto.decrypt_won_crypt_transform(
                        session_key, challenge[7:7 + challenge_length])), 16)
                    response_cipher = woncrypto.encrypt_won_crypt_transform(session_key, b"\0")
                    connection.sendall(wonemu.frame(
                        bytes.fromhex("05 0c 00 05 00") +
                        struct.pack("<H", len(response_cipher)) + response_cipher))
                    auth_reply = wonemu.recv_frame(connection)
                    self.assertEqual(struct.unpack_from("<BHHhB", auth_reply), (5, 12, 6, 0, 0))

                session = emulator.auth_session_for_peer("127.0.0.1")
                self.assertEqual(session["auth_provider"], "tribesnext")
                self.assertEqual(session["tribesnext_credentials"].certificate.guid, 7654321)
                self.assertEqual(emulator.community.get_profile(username), None)
                session["irc_authenticated"] = True

                class Request:
                    def __init__(self):
                        self.sent = []

                    def sendall(self, payload):
                        self.sent.append(payload.decode("utf-8"))

                handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
                handler.server = SimpleNamespace(emulator=emulator)
                handler.request = Request()
                handler._handle_dbqax("50 23 0 :WONEMUXPROBE", session, 1)
                reply = "".join(handler.request.sent)
                self.assertIn("VerifiedName", reply)
                self.assertIn("https://verified.invalid", reply)
                self.assertIn("verified profile", reply)
                # One profile view supplies AuthInfo memberships at login; the
                # other answers the Browser profile query.
                self.assertEqual([endpoint for endpoint, _ in robot_requests], [
                    "robot_login.php", "robot_login.php", "robot_browser.php",
                    "robot_browser.php"])
            finally:
                emulator.close()

    def test_community_store_migrates_legacy_mail_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "community.sqlite3"
            connection = sqlite3.connect(database)
            connection.execute("""
                CREATE TABLE mail (
                    mail_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    sender_id INTEGER NOT NULL,
                    recipient_id INTEGER NOT NULL,
                    is_cc INTEGER NOT NULL DEFAULT 0,
                    subject TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    is_read INTEGER NOT NULL DEFAULT 0,
                    is_deleted INTEGER NOT NULL DEFAULT 0
                )
            """)
            connection.execute("""
                CREATE TABLE invitations (
                    invite_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    tribe_id INTEGER NOT NULL,
                    user_id INTEGER NOT NULL,
                    status TEXT NOT NULL DEFAULT 'pending',
                    created_at INTEGER NOT NULL,
                    UNIQUE (tribe_id, user_id)
                )
            """)
            connection.commit()
            connection.close()

            community_store.CommunityStore(database)
            connection = sqlite3.connect(database)
            columns = {row[1] for row in connection.execute("PRAGMA table_info(mail)")}
            invitation_columns = {row[1] for row in
                                  connection.execute("PRAGMA table_info(invitations)")}
            connection.close()
            self.assertTrue({"to_list", "cc_list"}.issubset(columns))
            self.assertIn("inviter_id", invitation_columns)

    def test_community_store_migrates_legacy_forum_cursors(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "community.sqlite3"
            connection = sqlite3.connect(database)
            connection.executescript("""
                CREATE TABLE profiles (
                    user_id INTEGER PRIMARY KEY,
                    username TEXT NOT NULL UNIQUE COLLATE NOCASE,
                    display_name TEXT NOT NULL,
                    tag TEXT NOT NULL DEFAULT '',
                    appending INTEGER NOT NULL DEFAULT 0,
                    description TEXT NOT NULL DEFAULT '',
                    url TEXT NOT NULL DEFAULT '',
                    graphic TEXT NOT NULL DEFAULT '',
                    primary_tribe TEXT NOT NULL DEFAULT '',
                    registered_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                );
                CREATE TABLE forums (
                    forum_id INTEGER PRIMARY KEY,
                    name TEXT NOT NULL,
                    flags INTEGER NOT NULL DEFAULT 0,
                    tribe_id INTEGER NOT NULL DEFAULT 0
                );
                INSERT INTO forums VALUES (1, 'General', 0, 0);
                CREATE TABLE forum_topics (
                    topic_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    forum_id INTEGER NOT NULL,
                    subject TEXT NOT NULL,
                    author_id INTEGER NOT NULL,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    is_deleted INTEGER NOT NULL DEFAULT 0,
                    locked INTEGER NOT NULL DEFAULT 0,
                    lock_reason TEXT NOT NULL DEFAULT ''
                );
                CREATE TABLE forum_posts (
                    post_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    topic_id INTEGER NOT NULL,
                    parent_id INTEGER NOT NULL DEFAULT 0,
                    author_id INTEGER NOT NULL,
                    subject TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    is_deleted INTEGER NOT NULL DEFAULT 0
                );
                INSERT INTO profiles (user_id, username, display_name, registered_at, updated_at)
                    VALUES (101, 'legacy-poster', 'legacy-poster', 100, 100);
                INSERT INTO forum_topics
                    (topic_id, forum_id, subject, author_id, created_at, updated_at)
                    VALUES (1, 1, 'Legacy thread', 101, 100, 100);
                INSERT INTO forum_posts
                    (post_id, topic_id, parent_id, author_id, subject, body, created_at, updated_at)
                    VALUES (1, 1, 0, 101, 'Legacy thread', 'Legacy body', 100, 100);
            """)
            connection.commit()
            connection.close()

            store = community_store.CommunityStore(database)
            topics = store.list_forum_topics(1)
            posts = store.list_forum_posts(1)
            self.assertGreater(topics[0]["update_id"], 0)
            self.assertEqual(posts[0]["update_id"], topics[0]["update_id"])

    def test_community_store_local_account_enrolls_and_verifies_password(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            profile = store.reserve_local_account("Local Login", 12345)
            self.assertEqual(profile["username"], "Local Login")
            self.assertTrue(store.local_account_exists("local login"))
            self.assertEqual(store.verify_local_account("Local Login", "chosen-test-password"),
                             ("valid", True))
            self.assertEqual(store.verify_local_account("local login", "chosen-test-password"),
                             ("valid", False))
            self.assertEqual(store.verify_local_account("Local Login", "wrong-password"),
                             ("invalid", False))
            with self.assertRaisesRegex(ValueError, "already exists"):
                store.reserve_local_account("Local Login", 12345)

    def test_community_store_reserves_and_enrolls_local_password(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            profile = store.reserve_local_account("Local Login", 12345)
            self.assertEqual(profile["username"], "Local Login")
            self.assertTrue(store.local_account_exists("local login"))
            self.assertEqual(store.verify_local_account("Local Login", "chosen-test-password"),
                             ("valid", True))
            self.assertEqual(store.verify_local_account("local login", "chosen-test-password"),
                             ("valid", False))
            self.assertEqual(store.verify_local_account("Local Login", "wrong-password"),
                             ("invalid", False))
            with self.assertRaisesRegex(ValueError, "already exists"):
                store.reserve_local_account("Local Login", 12345)

    def test_community_store_persists_only_profile_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "community.sqlite3"
            store = community_store.CommunityStore(database)
            profile = store.ensure_profile("Local-Player", 12345)
            self.assertEqual(profile["username"], "Local-Player")
            self.assertEqual(profile["graphic"], community_store.DEFAULT_PROFILE_GRAPHIC)
            updated = store.update_profile(
                "local-player", 12345, description="A local profile", url="https://example.test")
            self.assertEqual(updated["description"], "A local profile")
            self.assertEqual(updated["url"], "https://example.test")
            reopened = community_store.CommunityStore(database)
            self.assertEqual(reopened.search_players("LOCAL"), [
                {"user_id": 12345, "username": "Local-Player", "display_name": "Local-Player"}
            ])

    def test_community_store_tribe_membership_and_invite_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            store.create_tribe("Local Tribe", "LT", True, True, "Description",
                               "founder", 101)
            self.assertEqual(store.membership("Local Tribe", "founder")["access"], 4)
            self.assertEqual(store.get_profile("founder")["primary_tribe"], "Local Tribe")
            store.ensure_profile("member", 102)
            store.invite("Local Tribe", "member", 102)
            invitations = store.list_invitations("Local Tribe")
            self.assertEqual((invitations[0]["invited_name"], invitations[0]["inviter_name"]),
                             ("member", "founder"))
            self.assertEqual(store.respond_to_invite("accept", "Local Tribe", "member", 102),
                             "accepted")
            with self.assertRaisesRegex(ValueError, "already a member"):
                store.invite("Local Tribe", "member", 102)
            store.request_invitation("Local Tribe", "requester", 103)
            request = next(row for row in store.list_invitations("Local Tribe")
                           if row["invited_name"] == "requester")
            self.assertEqual(request["status"], "requested")
            self.assertEqual(store.respond_to_invite("accept", "Local Tribe", "requester", 103,
                                                     "requested"), "accepted")
            store.update_membership("Local Tribe", "member", title="Captain", access=3)
            store.ensure_profile("invitee-two", 104)
            store.invite("Local Tribe", "invitee-two", 104, "member", 102)
            outgoing = next(row for row in store.list_invitations("Local Tribe")
                            if row["invited_name"] == "invitee-two")
            self.assertEqual(outgoing["inviter_name"], "member")
            rows = store.list_tribe_members("local tribe")
            member = next(row for row in rows if row["username"] == "member")
            self.assertEqual((member["title"], member["access"]), ("Captain", 3))
            store.update_tribe("Local Tribe", recruiting=False, description="Updated")
            tribe = store.get_tribe("LOCAL TRIBE")
            self.assertEqual((tribe["recruiting"], tribe["description"]), (0, "Updated"))
            self.assertEqual(store.search_tribes("local")[0]["member_count"], 3)
            self.assertTrue(store.remove_membership("Local Tribe", "member"))

    def test_community_store_preserves_tribe_name_and_tag_punctuation(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            store.create_tribe("#t2scripting", ".cs", 0, 1, "",
                               "founder", 12345)
            tribe = store.get_tribe("#t2scripting")
            self.assertEqual(tribe["name"], "#t2scripting")
            self.assertEqual(tribe["tag"], ".cs")

    def test_dbproxy_local_inbox_rows_use_stock_name_quad_order(self):
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                       Path(directory))
            emulator.community.ensure_profile("Sender", 101)
            emulator.community.ensure_profile("Player", 102)
            emulator.community.send_mail("Sender", 101, [("Player", False)], "Hello",
                                         "Body", "Player")

            class Request:
                def __init__(self):
                    self.sent = []

                def sendall(self, payload):
                    self.sent.append(payload.decode("utf-8"))

            handler = wonemu.IRCHandler.__new__(wonemu.IRCHandler)
            handler.server = SimpleNamespace(emulator=emulator)
            handler.request = Request()
            handler._handle_dbqax("5 C1 10 :0", {"username": "Player",
                                                 "irc_authenticated": True}, 1)
            reply = "".join(handler.request.sent)
            # getTextName reads name, tag, append, id for both quads.
            self.assertRegex(reply, r"\d+\tSender\t[^\t]*\t\d\t101\tPlayer\t[^\t]*\t\d\t102\t")

    def test_community_store_mail_send_and_inbox_cursor(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            store.ensure_profile("sender", 101)
            store.ensure_profile("recipient", 102)
            store.ensure_profile("copy", 103)
            self.assertEqual(store.send_mail(
                "sender", 101, [("recipient", False), ("copy", True)], "Hello",
                "Line one\nLine two", "recipient", "copy"), 2)
            messages = store.inbox("recipient")
            self.assertEqual(len(messages), 1)
            self.assertEqual((messages[0]["sender_name"], messages[0]["subject"],
                              messages[0]["body"]), ("sender", "Hello", "Line one\nLine two"))
            self.assertEqual((messages[0]["to_list"], messages[0]["cc_list"]),
                             ("recipient", "copy"))
            copy_message = store.inbox("copy")[0]
            self.assertEqual((copy_message["is_cc"], copy_message["to_list"],
                              copy_message["cc_list"]), (1, "recipient", "copy"))
            self.assertEqual(store.inbox("recipient", messages[0]["mail_id"]), [])
            self.assertTrue(store.update_mail("recipient", messages[0]["mail_id"], is_read=True))
            self.assertTrue(store.update_mail("recipient", messages[0]["mail_id"], is_deleted=True))
            self.assertEqual(store.inbox("recipient"), [])
            self.assertEqual(store.inbox("recipient", include_deleted=True, deleted_only=True)[0]["subject"],
                             "Hello")
            self.assertTrue(store.set_email_block("recipient", "sender"))
            self.assertEqual(store.send_mail("sender", 101, [("recipient", False)],
                                             "Blocked", "Should not arrive"), 0)
            block = store.email_block_list("recipient")[0]
            self.assertEqual((block["username"], block["blocked_count"]), ("sender", 0))
            self.assertTrue(store.remove_email_block("recipient", "sender"))

    def test_community_store_forum_topic_reply_edit_and_delete(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            store.ensure_profile("poster", 101)
            self.assertEqual(store.forum_list()[0]["name"], "General")
            tribe = store.create_tribe("Poster Tribe", "PT", True, True, "", "poster", 101)
            self.assertEqual(store.ensure_forum(-tribe["tribe_id"])["tribe_id"],
                             tribe["tribe_id"])
            topic = store.create_forum_post(1, 0, 0, "poster", 101,
                                            "Topic", "Opening post")
            reply = store.create_forum_post(1, topic["topic_id"], topic["post_id"],
                                            "poster", 101, "Re: Topic", "Reply body")
            store.set_forum_topic_locked(topic["topic_id"], True, "Maintenance")
            with self.assertRaisesRegex(ValueError, "locked"):
                store.create_forum_post(1, topic["topic_id"], topic["post_id"],
                                        "poster", 101, "Locked reply", "Not accepted")
            store.set_forum_topic_locked(topic["topic_id"], False)
            listed = store.list_forum_topics(1)
            self.assertEqual((listed[0]["topic_id"], listed[0]["post_count"]),
                             (topic["topic_id"], 2))
            posts = store.list_forum_posts(topic["topic_id"])
            self.assertEqual([post["body"] for post in posts], ["Opening post", "Reply body"])
            self.assertEqual([post["body"] for post in
                              store.list_forum_posts(topic["topic_id"], posts[0]["update_id"])],
                             ["Reply body"])
            cached_update = listed[0]["update_id"]
            self.assertTrue(store.edit_forum_post(topic["post_id"], "poster",
                                                  "Edited topic", "Updated opening"))
            self.assertEqual(store.list_forum_topics(1)[0]["subject"], "Edited topic")
            changed = store.list_forum_posts(topic["topic_id"], cached_update)
            self.assertEqual([(post["post_id"], post["body"]) for post in changed],
                             [(topic["post_id"], "Updated opening")])
            self.assertTrue(store.delete_forum_post(reply["post_id"], "poster"))
            self.assertEqual(store.list_forum_topics(1)[0]["post_count"], 1)
            moved = store.create_forum_post(1, 0, 0, "poster", 101,
                                            "Move me", "Topic body")
            store.move_forum_topic(moved["topic_id"], -tribe["tribe_id"])
            self.assertEqual(store.list_forum_topics(-tribe["tribe_id"])[0]["topic_id"],
                             moved["topic_id"])
            self.assertTrue(store.delete_forum_topic(moved["topic_id"]))
            self.assertEqual(store.list_forum_topics(-tribe["tribe_id"]), [])

    def test_community_store_news_post_edit_and_delete(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            store.ensure_profile("author", 101)
            article_id = store.post_news(105, "author", 101, "Headline", "Article body")
            articles, total = store.list_news(105)
            self.assertEqual((total, articles[0]["title"], articles[0]["body"]),
                             (1, "Headline", "Article body"))
            self.assertTrue(store.edit_news(article_id, "author", 35500,
                                            "Updated headline", "Updated body"))
            self.assertEqual(store.list_news(35500)[0][0]["title"], "Updated headline")
            self.assertTrue(store.delete_news(article_id, "author"))
            self.assertEqual(store.list_news(35500), ([], 0))

    def test_community_store_buddy_list_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            store.ensure_profile("owner", 101)
            store.ensure_profile("buddy", 102)
            store.add_buddy("owner", 101, "buddy", 102)
            store.add_buddy("owner", 101, "buddy", 102)
            self.assertEqual([row["username"] for row in store.list_buddies("OWNER")], ["buddy"])
            self.assertTrue(store.remove_buddy("owner", "buddy"))
            store.add_buddy("owner", 101, "buddy", 102)
            self.assertEqual(store.clear_buddies("owner"), 1)
            self.assertEqual(store.list_buddies("owner"), [])

    def test_won_irc_token_codec_matches_native_auth_frame(self):
        frame = bytes.fromhex("cb00000032000000")
        self.assertEqual(woncrypto.encode_won_token(frame), "b`000380000")
        self.assertEqual(woncrypto.decode_won_token("b`000380000"), frame)
        parsed = woncrypto.parse_won_irc_token("b`000380000")
        self.assertEqual(parsed, {"subtype": 0x32, "payload": b""})
        for length in range(10):
            sample = bytes(range(length))
            token = woncrypto.encode_won_token(sample)
            self.assertEqual(woncrypto.decode_won_token(token), sample)

    def test_won_irc_token_codec_rejects_invalid_alphabet_and_length(self):
        for token in ("bad token", "bad!", "0"):
            with self.subTest(token=token), self.assertRaises(ValueError):
                woncrypto.decode_won_token(token)
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            woncrypto.parse_won_irc_token(woncrypto.encode_won_token(b"not an IRC frame"))

    def test_won_irc_cb33_challenge_sections(self):
        challenge = bytes(range(16))
        key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
               "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        challenge_block = struct.pack("<H", len(challenge)) + challenge
        encrypted_challenge = woncrypto.encrypt_elgamal_challenge(key, challenge_block)
        context = bytes.fromhex("0102030405060708")
        payload = (struct.pack("<H", len(encrypted_challenge)) + encrypted_challenge +
                   struct.pack("<H", len(context)) + context)
        token = woncrypto.encode_won_token(struct.pack("<II", 0xcb, 0x33) + payload)

        self.assertEqual(woncrypto.decrypt_won_irc_challenge(token, key),
                         {"challenge": challenge, "challenge_extra": b"", "context": context})
        challenge_block += b"extra"
        encrypted_challenge = woncrypto.encrypt_elgamal_challenge(key, challenge_block)
        payload = (struct.pack("<H", len(encrypted_challenge)) + encrypted_challenge +
                   struct.pack("<H", len(context)) + context)
        token = woncrypto.encode_won_token(struct.pack("<II", 0xcb, 0x33) + payload)
        parsed = woncrypto.decrypt_won_irc_challenge(token, key)
        self.assertEqual(parsed["challenge"], challenge)
        self.assertEqual(parsed["challenge_extra"], b"extra")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            woncrypto.parse_won_irc_challenge(
                woncrypto.encode_won_token(struct.pack("<IIH", 0xcb, 0x33, 20)))

    def test_nr_digest_truncation_matches_auth_group_width(self):
        message = b"Tribes2 Auth1 certificate signature vector"
        self.assertEqual(woncrypto._nr_digest({"q": SMALL_AUTH_Q}, message),
                         int("08e33adea436e2bb49e8065749f02c4", 16))

    def test_recovered_request_and_endpoints(self):
        request = wonemu.decode_getdir(TRIBES2_QUERY[4:])
        self.assertEqual(request["path"], "/Tribes2")
        self.assertEqual(request["flags"], 0x06000022)
        wire = wonemu.getdir_response(request, wonemu.directory_records(wonemu.PORTS))
        self.assertEqual(struct.unpack_from("<I", wire)[0], len(wire))
        sequence, flags, records = parse_service_reply(wire[4:])
        self.assertEqual(sequence, 0x80)
        self.assertEqual(flags, 0x06000022)
        self.assertEqual(records, [
            {"name": "IRCServer", "address": ("127.0.0.1", 15107), "display_name": "0"},
            {"name": "MasterServer", "address": ("127.0.0.1", 15108), "display_name": "0"},
        ])

    def test_auth_reply_mask_matches_emitted_fields(self):
        request = wonemu.decode_getdir(AUTH_QUERY[4:])
        wire = wonemu.getdir_response(request, wonemu.directory_records(wonemu.PORTS))
        _, flags, records = parse_service_reply(wire[4:])
        self.assertEqual(flags, 0x06000032)
        self.assertEqual(records[0]["address"], ("127.0.0.1", 15105))

    def test_empty_directory_and_unknown_path(self):
        request = wonemu.decode_getdir(TRIBES2_QUERY[4:])
        self.assertEqual(parse_service_reply(wonemu.getdir_response(request, {"/Tribes2": []})[4:])[2], [])
        error = wonemu.getdir_response(request, {})
        self.assertEqual(error[4:], bytes.fromhex("05 02 00 03 00 ff ff"))

    def test_fragmented_directory_replies(self):
        request = wonemu.decode_getdir(TRIBES2_QUERY[4:])
        request["entities_per_reply"] = 1
        wire = wonemu.getdir_response(request, wonemu.directory_records(wonemu.PORTS))
        length, = struct.unpack_from("<I", wire)
        self.assertEqual(parse_service_reply(wire[4:length])[0], 0)
        self.assertEqual(parse_service_reply(wire[length + 4:])[0], 0x81)

    def test_reject_truncated_or_trailing_request(self):
        for payload in (TRIBES2_QUERY[4:-1], TRIBES2_QUERY[4:] + b"x"):
            with self.assertRaises(ValueError):
                wonemu.decode_getdir(payload)

    def test_elgamal_wire_order_and_big_integer_plaintext(self):
        parameters = woncrypto.verifier_parameters(Path.home() / "t2-linux/tribes2.dynamic")
        key = woncrypto.make_key(parameters)
        width = (key["p"].bit_length() + 7) // 8
        message = int.from_bytes(b"wonemu-test", "big")
        nonce = 7
        first = pow(key["g"], nonce, key["p"])
        second = message * pow(key["y"], nonce, key["p"]) % key["p"]
        encrypted = struct.pack("<I", 1) + first.to_bytes(width, "big") + second.to_bytes(width, "big")
        result = woncrypto.decrypt_elgamal(key, encrypted)
        self.assertEqual(result[-len(b"wonemu-test"):], b"wonemu-test")
        self.assertEqual(len(result), width)

    def test_elgamal_challenge_encoding_matches_native_chunk_layout(self):
        key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
               "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        width = (key["p"].bit_length() + 7) // 8
        chunk_width = width - 3
        for plaintext in (b"challenge", bytes(range(40))):
            ciphertext = woncrypto.encrypt_elgamal_challenge(key, plaintext)
            expected_count = (len(plaintext) + chunk_width - 1) // chunk_width
            self.assertEqual(struct.unpack_from("<I", ciphertext)[0], expected_count)
            decoded = woncrypto.decrypt_elgamal(key, ciphertext)
            recovered = bytearray()
            for offset in range(0, len(decoded), width):
                block = decoded[offset:offset + width]
                chunk_length = block[-1]
                self.assertLessEqual(chunk_length, chunk_width)
                recovered.extend(block[-1 - chunk_length:-1])
            self.assertEqual(bytes(recovered), plaintext)

    def test_won_irc_cb33_and_cb35_builders_use_auth1_identity(self):
        keys = {"root": {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                          "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)},
                "auth": {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                         "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}}
        session_key = bytes.fromhex("0102030405060708")
        client_key = woncrypto.make_key(keys["auth"])
        reply = woncrypto.auth1_login_reply(keys, session_key, "irc-test-account",
                                            now=1700000000, client_key=client_key)
        material = woncrypto.auth1_login_material(reply)
        material["client_key"]["x"] = client_key["x"]
        challenge = bytes.fromhex("1020304050607080")

        challenge_token = woncrypto.build_won_irc_cb33(reply, challenge)
        parsed = woncrypto.decrypt_won_irc_challenge(challenge_token, material["client_key"])
        self.assertEqual(parsed["challenge"], challenge)
        self.assertEqual(parsed["context"], material["auth2_certificate"])

        final_token = woncrypto.build_won_irc_cb35(reply, session_key)
        final = woncrypto.decode_won_token(final_token)
        self.assertEqual(struct.unpack_from("<IIHH", final), (0xcb, 0x35, 0, 36))
        decrypted = woncrypto.decrypt_elgamal(material["client_key"], final[12:48])
        width = (material["client_key"]["p"].bit_length() + 7) // 8
        chunk_length = decrypted[width - 1]
        chunk = decrypted[width - 1 - chunk_length:width - 1]
        self.assertEqual(chunk, struct.pack("<H", len(session_key)) + session_key)

        proof = struct.pack("<H", len(session_key)) + session_key + session_key
        encrypted_proof = woncrypto.encrypt_elgamal_challenge(client_key, proof)
        cb34 = woncrypto.encode_won_token(
            struct.pack("<IIH", 0xcb, 0x34, len(encrypted_proof)) + encrypted_proof)
        self.assertTrue(woncrypto.verify_won_irc_cb34(cb34, material["client_key"], session_key))
        self.assertFalse(woncrypto.verify_won_irc_cb34(
            cb34, material["client_key"], b"not the session key"))

        challenge = bytes.fromhex("aabbccddeeff0011")
        client_response = bytes.fromhex("1020304050607080")
        native_proof = (struct.pack("<H", len(challenge)) + challenge + client_response)
        encrypted_native_proof = woncrypto.encrypt_elgamal_challenge(client_key, native_proof)
        cb34 = woncrypto.encode_won_token(
            struct.pack("<IIH", 0xcb, 0x34, len(encrypted_native_proof)) + encrypted_native_proof)
        self.assertEqual(woncrypto.extract_won_irc_cb34_response(
            cb34, client_key, challenge), client_response)
        self.assertIsNone(woncrypto.extract_won_irc_cb34_response(
            cb34, client_key, b"a different challenge"))

    def test_keyblock_signature_and_der_public_key(self):
        parameters = woncrypto.verifier_parameters(Path.home() / "t2-linux/tribes2.dynamic")
        keys = {"root": woncrypto.make_key(parameters), "auth": woncrypto.make_key(parameters)}
        block = woncrypto.key_block(keys, now=1700000000)
        signature_size = 2 * ((parameters["q"].bit_length() + 7) // 8)
        body, signature = block[:-signature_size], block[-signature_size:]
        self.assertTrue(woncrypto.verify(keys["root"], body, signature))
        self.assertEqual(woncrypto.read_der_public(woncrypto.public_key(keys["auth"])),
                         {name: keys["auth"][name] for name in ("p", "q", "g", "y")})

    def test_auth_login_envelope_recovers_elgamal_material_without_logging_it(self):
        parameters = woncrypto.verifier_parameters(Path.home() / "t2-linux/tribes2.dynamic")
        key = woncrypto.make_key(parameters)
        width = (key["p"].bit_length() + 7) // 8
        clear = b"local-session-material"
        nonce = 11
        first = pow(key["g"], nonce, key["p"])
        second = int.from_bytes(clear, "big") * pow(key["y"], nonce, key["p"]) % key["p"]
        encrypted = struct.pack("<I", 1) + first.to_bytes(width, "big") + second.to_bytes(width, "big")
        inner = b"x" * 128
        payload = (bytes.fromhex("05 0c 00 03 00") + struct.pack("<HH", 1, len(encrypted)) + encrypted +
                   struct.pack("<H", len(inner)) + inner)
        result = wonemu.decode_login_envelope(payload, {"auth": key})
        self.assertEqual(result["key_block_id"], 1)
        self.assertEqual(result["elgamal_blocks"], 1)
        self.assertEqual(result["elgamal_plaintext_bytes"], width)
        self.assertEqual(result["encrypted_login_data_bytes"], 128)
        self.assertNotIn("password", result)

    def test_login_payload_accepts_opaque_client_extensions(self):
        session_key = bytes.fromhex("0102030405060708")
        extension = bytes.fromhex("a1b2c3d4e5")
        payload = (session_key[:2] + struct.pack("<HI", 1, 14) + wonemu.wide("LocalUser") +
                   wonemu.wide("local-password") + struct.pack("<HH", 0, 256) +
                   wonemu.wide("Tribes2Client") + extension)
        fields = wonemu.parse_login_payload(payload, session_key)
        self.assertEqual(fields["username"], "LocalUser")
        self.assertEqual(fields["password"], "local-password")
        self.assertEqual(fields["client"], "Tribes2Client")
        self.assertEqual(fields["extension_bytes"], len(extension))
        self.assertNotIn("extension", fields)

    def test_missing_tribesnext_account_is_recovered_before_login(self):
        key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4, "x": SMALL_AUTH_X,
               "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        payload = (bytes.fromhex("05 0c 00 03 00") + struct.pack("<HH", 1, len(SMALL_AUTH_ENCRYPTED_KEY)) +
                   SMALL_AUTH_ENCRYPTED_KEY + struct.pack("<H", len(SMALL_AUTH_ENCRYPTED_DATA)) +
                   SMALL_AUTH_ENCRYPTED_DATA)
        private_hex = "00112233445566778899aabbccddeeff"
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        rc4_key = hashlib.sha1(("probe_not_a_real_password" + nonce).encode("ascii")).hexdigest().encode("ascii")
        store_line = f"WONEMUXPROBE\t{nonce}:{woncrypto._rc4_drop2048(rc4_key, bytes.fromhex(private_hex)).hex()}\n"
        with tempfile.TemporaryDirectory() as directory:
            private_store = Path(directory) / "private.store"
            requests = []

            def recover(name, password):
                requests.append((name, password))
                private_store.write_text(store_line, encoding="ascii")
                return "refreshed"

            recovered = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, recover_tribesnext=recover)
            installed = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, recover_tribesnext=recover)
            private_store.unlink()
            failed = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store,
                recover_tribesnext=lambda name, password: "failed")

            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory),
                                       private_store=private_store)
            attempts = []
            emulator.refresh_tribesnext_account = (
                lambda name, password: attempts.append(password) or "failed")
            first = emulator.recover_tribesnext_account("Probe", "wrong")
            repeated = emulator.recover_tribesnext_account("PROBE", "wrong")
            corrected = emulator.recover_tribesnext_account("Probe", "right")
            emulator.log.close()
            log_text = (Path(directory) / "traffic.jsonl").read_text(encoding="utf-8")
        self.assertEqual(requests, [("WONEMUXPROBE", "probe_not_a_real_password")])
        self.assertEqual((recovered["account_validation"], recovered["credential_source"],
                          recovered["tribesnext_recovery"]), ("valid", "tribesnext", "refreshed"))
        # Once installed, logging in does not contact TribesNext again.
        self.assertEqual(installed["tribesnext_recovery"], "not_configured")
        self.assertNotEqual(failed["account_validation"], "valid")
        self.assertEqual(failed["tribesnext_recovery"], "failed")
        self.assertEqual((first, repeated, corrected), ("failed", "recently_failed", "failed"))
        self.assertEqual(attempts, ["wrong", "right"])
        self.assertNotIn("wrong", log_text)

    def test_real_client_small_group_login_decrypts_without_logging_credentials(self):
        # Deterministic Loki #25034 probe capture. The local compatibility auth
        # group keeps this legacy client's BigInteger multiply below its
        # oversized-c2 truncation edge; credentials remain encrypted in fixture.
        key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4, "x": SMALL_AUTH_X,
               "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        encrypted_key = SMALL_AUTH_ENCRYPTED_KEY
        encrypted_data = SMALL_AUTH_ENCRYPTED_DATA
        payload = (bytes.fromhex("05 0c 00 03 00") + struct.pack("<HH", 1, len(encrypted_key)) +
                   encrypted_key + struct.pack("<H", len(encrypted_data)) + encrypted_data)
        private_hex = "00112233445566778899aabbccddeeff"
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        rc4_key = hashlib.sha1(("probe_not_a_real_password" + nonce).encode("ascii")).hexdigest().encode("ascii")
        store_value = woncrypto._rc4_drop2048(rc4_key, bytes.fromhex(private_hex)).hex()
        with tempfile.TemporaryDirectory() as directory:
            private_store = Path(directory) / "private.store"
            private_store.write_text(f"WONEMUXPROBE\t{nonce}:{store_value}\n", encoding="ascii")
            result = wonemu.decode_login_envelope(payload, {"auth": key}, private_store)
            # A remote peer's login is decoded without the private store, so the
            # installed TribesNext key cannot authenticate it.
            remote_result = wonemu.decode_login_envelope(payload, {"auth": key}, None)
            account_store = community_store.CommunityStore(Path(directory) / "community.sqlite3")
            account_store.reserve_local_account(
                "WONEMUXPROBE", woncrypto.local_auth_user_id("WONEMUXPROBE"))
            # TribesNext is first by default; with native first, the reserved
            # local account enrolls this password.
            default_order_result = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, account_store)
            local_result = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, account_store,
                auth_order=("native", "tribesnext"))
            existing_local_result = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, account_store,
                auth_order=("native", "tribesnext"))
            native_only_result = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, None, auth_order=("native",))
            recovery_requests = []
            recovery_result = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, account_store,
                refresh_tribesnext=lambda name, password: (
                    recovery_requests.append((name, password)) or "refreshed"))
            separate_password_store = community_store.CommunityStore(
                Path(directory) / "separate-password.sqlite3")
            separate_password_store.reserve_local_account(
                "WONEMUXPROBE", woncrypto.local_auth_user_id("WONEMUXPROBE"))
            local_seed_status, local_seeded = separate_password_store.verify_local_account(
                "WONEMUXPROBE", "distinct-local-password")
            tribesnext_password_result = wonemu.decode_login_envelope(
                payload, {"auth": key}, private_store, separate_password_store)
        session_key, login_data = woncrypto.decrypt_won_login_data(key, encrypted_key, encrypted_data)
        fields = wonemu.parse_login_payload(login_data, session_key)
        self.assertEqual(woncrypto.encrypt_won_crypt_transform(session_key, login_data), encrypted_data)
        self.assertEqual(result["login_data_decryption"], "decoded")
        self.assertEqual(result["credential_source"], "tribesnext")
        self.assertNotEqual(remote_result["account_validation"], "valid")
        self.assertNotEqual(remote_result["credential_source"], "tribesnext")
        self.assertEqual(result["symmetric_key_bytes"], 8)
        self.assertEqual(result["login_plaintext_bytes"], 120)
        self.assertEqual(result["login_plaintext_sha256"],
                         "d502da47cf7c5ca6c3eafeb8dbf54a3d954545e8b696ae126d8231690b21d6d0")
        self.assertEqual(result["login_request_kind"], 14)
        self.assertEqual(result["username_utf16_bytes"], 24)
        self.assertEqual(result["password_utf16_bytes"], 50)
        self.assertEqual(result["client_identifier"], "Tribes2Client")
        self.assertEqual(result["account_validation"], "valid")
        self.assertEqual(default_order_result["credential_source"], "tribesnext")
        self.assertFalse(default_order_result["account_enrolled"])
        self.assertEqual(local_result["account_validation"], "valid")
        self.assertEqual(local_result["credential_source"], "local")
        self.assertTrue(local_result["account_enrolled"])
        # With TribesNext unchecked for auth, its key files are never consulted.
        self.assertEqual(native_only_result["account_validation"], "account_missing")
        self.assertEqual(native_only_result["tribesnext_recovery"], "not_used")
        self.assertEqual(existing_local_result["account_validation"], "valid")
        self.assertFalse(existing_local_result["account_enrolled"])
        self.assertEqual(recovery_result["tribesnext_recovery"], "refreshed")
        self.assertEqual(len(recovery_requests), 1)
        self.assertEqual(local_result["account_validation"], "valid")
        self.assertTrue(local_result["account_enrolled"])
        self.assertEqual(existing_local_result["account_validation"], "valid")
        self.assertFalse(existing_local_result["account_enrolled"])
        self.assertEqual(fields["username"], "WONEMUXPROBE")
        self.assertEqual(fields["password"], "probe_not_a_real_password")
        self.assertFalse({"username", "password", "plaintext"} & result.keys())
        self.assertEqual(local_seed_status, "valid")
        self.assertTrue(local_seeded)
        self.assertEqual(tribesnext_password_result["account_validation"], "valid")
        self.assertEqual(tribesnext_password_result["credential_source"], "tribesnext")
        self.assertFalse(tribesnext_password_result["account_enrolled"])

    def test_valid_private_store_login_gets_signed_auth1_success_reply(self):
        game_binary = Path.home() / "t2-linux/tribes2.dynamic"
        root_key = woncrypto.make_key(woncrypto.verifier_parameters(game_binary))
        auth_key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                    "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        password = "probe_not_a_real_password"
        private_hex = "00112233445566778899aabbccddeeff"
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        rc4_key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
        encrypted_private = woncrypto._rc4_drop2048(rc4_key, bytes.fromhex(private_hex))

        with tempfile.TemporaryDirectory() as directory:
            private_store = Path(directory) / "private.store"
            private_store.write_text(
                f"WONEMUXPROBE\t{nonce}:{encrypted_private.hex()}\n", encoding="ascii")
            emulator = wonemu.Emulator(
                {service: 0 for service in wonemu.PORTS}, Path(directory),
                keys={"root": root_key, "auth": auth_key}, private_store=private_store)
            emulator.start()
            try:
                with socket.create_connection((wonemu.HOST, emulator.ports["auth"]), timeout=2) as conn:
                    conn.sendall(wonemu.frame(bytes.fromhex("05 0c 00 01 00")))
                    self.assertEqual(struct.unpack_from("<BHHh", wonemu.recv_frame(conn)), (5, 12, 2, 0))
                    login = (bytes.fromhex("05 0c 00 03 00") + struct.pack("<HH", 1, len(SMALL_AUTH_ENCRYPTED_KEY)) +
                             SMALL_AUTH_ENCRYPTED_KEY + struct.pack("<H", len(SMALL_AUTH_ENCRYPTED_DATA)) +
                             SMALL_AUTH_ENCRYPTED_DATA)
                    conn.sendall(wonemu.frame(login))
                    challenge_frame = wonemu.recv_frame(conn)
                    self.assertEqual(struct.unpack_from("<BHH", challenge_frame), (5, 12, 4))
                    challenge_length, = struct.unpack_from("<H", challenge_frame, 5)
                    challenge = woncrypto.decrypt_won_crypt_transform(
                        bytes.fromhex("0102030405060708"), challenge_frame[7:7 + challenge_length])
                    self.assertEqual(len(challenge), 16)
                    response_cipher = woncrypto.encrypt_won_crypt_transform(
                        bytes.fromhex("0102030405060708"), b"\0")
                    response = (bytes.fromhex("05 0c 00 05 00") + struct.pack("<H", len(response_cipher)) +
                                response_cipher)
                    conn.sendall(wonemu.frame(response))
                    reply = wonemu.recv_frame(conn)
                    self.assertEqual(struct.unpack_from("<BHHhB", reply), (5, 12, 6, 0, 0))
                    self.assertEqual(reply[8], 3)
                    offset = 9
                    certificates = []
                    for expected_type in (1, 3, 6):
                        self.assertEqual(reply[offset], expected_type)
                        length, = struct.unpack_from("<H", reply, offset + 1)
                        start = offset + 3
                        if expected_type in (1, 6):
                            certificates.append(reply[start:start + length])
                        else:
                            key_block = reply[start:start + length]
                            root_signature_bytes = 2 * ((root_key["q"].bit_length() + 7) // 8)
                            self.assertTrue(woncrypto.verify(
                                root_key, key_block[:-root_signature_bytes],
                                key_block[-root_signature_bytes:]))
                        offset = start + length
                    signature_bytes = 2 * ((auth_key["q"].bit_length() + 7) // 8)
                    for family, certificate in zip((1, 2), certificates):
                        self.assertEqual(struct.unpack_from("<H", certificate)[0], family)
                        self.assertTrue(woncrypto.verify(
                            auth_key, certificate[:-signature_bytes], certificate[-signature_bytes:]))
                    encrypted_confirmation = woncrypto.decrypt_won_crypt_transform(
                        bytes.fromhex("0102030405060708"), reply[offset:])
                    self.assertEqual(encrypted_confirmation[0], 2)
                    self.assertEqual(encrypted_confirmation[1], 2)
                    private_block_length, = struct.unpack_from("<H", encrypted_confirmation, 2)
                    private_block = encrypted_confirmation[4:4 + private_block_length]
                    self.assertEqual(private_block[0], 0)
                    key_offset = 1
                    private_values = []
                    for _ in range(5):
                        self.assertEqual(private_block[key_offset], 2)
                        key_offset += 1
                        length = private_block[key_offset]
                        key_offset += 1
                        if length & 0x80:
                            size_bytes = length & 0x7f
                            length = int.from_bytes(private_block[key_offset:key_offset + size_bytes], "big")
                            key_offset += size_bytes
                        self.assertGreater(length, 0)
                        private_values.append(private_block[key_offset:key_offset + length])
                        key_offset += length
                    self.assertEqual(key_offset, len(private_block))
                    self.assertEqual(len(private_values), 5)
                    entry_offset = 4 + private_block_length
                    self.assertEqual(encrypted_confirmation[entry_offset], 4)
                    confirmation_length, = struct.unpack_from("<H", encrypted_confirmation, entry_offset + 1)
                    confirmation = encrypted_confirmation[
                        entry_offset + 3:entry_offset + 3 + confirmation_length]
                    self.assertEqual(confirmation[2:], bytes.fromhex("0102030405060708"))
                session = emulator.auth_session_for_peer("127.0.0.1")
                self.assertEqual(session["username"], "WONEMUXPROBE")
                self.assertEqual(session["auth_provider"], "tribesnext")
                self.assertIsNone(session["tribesnext_credentials"])
                self.assertEqual(session["session_key"], bytes.fromhex("0102030405060708"))
                self.assertEqual(pow(session["client_key"]["g"], session["client_key"]["x"],
                                     session["client_key"]["p"]), session["client_key"]["y"])
                events = [json.loads(line) for line in (Path(directory) / "traffic.jsonl").read_text().splitlines()]
                validation = next(event for event in events if event["event"] == "login_envelope")
                self.assertEqual(validation["account_validation"], "valid")
                challenge = next(event for event in events if event["event"] == "auth_challenge_response")
                self.assertTrue(challenge["expected_response"])
                completion = next(event for event in events
                                  if event["event"] == "sent" and event.get("message") == 6)
                self.assertEqual(completion["status"], 0)
                self.assertFalse(any("probe_not_a_real_password" in line
                                     for line in (Path(directory) / "traffic.jsonl").read_text().splitlines()))
            finally:
                emulator.close()

    def test_local_auth_profiles_preserve_root_and_migrate_auth_parameters(self):
        parameters = woncrypto.verifier_parameters(Path.home() / "t2-linux/tribes2.dynamic")
        with tempfile.TemporaryDirectory() as directory:
            keys = woncrypto.provision(Path(directory), Path.home() / "t2-linux/tribes2.dynamic", "compat")
            self.assertEqual(keys["root"]["p"], parameters["p"])
            self.assertEqual(keys["auth"]["p"], woncrypto.LOCAL_AUTH_PARAMETERS["p"])
            self.assertEqual(pow(keys["auth"]["g"], keys["auth"]["x"], keys["auth"]["p"]),
                             keys["auth"]["y"])
            root_public = woncrypto.public_key(keys["root"])
            migrated = woncrypto.provision(Path(directory), Path.home() / "t2-linux/tribes2.dynamic", "native")
            self.assertEqual(woncrypto.public_key(migrated["root"]), root_public)
            self.assertEqual(migrated["auth"]["p"], parameters["p"])

    def test_private_store_password_verification_matches_store_format(self):
        username = "local-test-account"
        password = "fixture-password"
        encrypted_private_key = bytes.fromhex("00112233445566778899aabbccddeeff")
        private_hex = encrypted_private_key.hex()
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        rc4_key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
        ciphertext = woncrypto._rc4_drop2048(rc4_key, encrypted_private_key)
        with tempfile.TemporaryDirectory() as directory:
            store = Path(directory) / "private.store"
            store.write_text(f"{username}\t{nonce}:{ciphertext.hex()}\n", encoding="ascii")
            self.assertEqual(woncrypto.verify_private_store_password(store, username, password), "valid")
            self.assertEqual(woncrypto.verify_private_store_password(store, username, "wrong"), "invalid")
            self.assertEqual(woncrypto.verify_private_store_password(store, "missing", password),
                             "account_missing")

    def test_auth1_login_enrolls_reserved_local_account_without_private_store(self):
        root_key = woncrypto.make_key(
            woncrypto.verifier_parameters(Path.home() / "t2-linux/tribes2.dynamic"))
        auth_key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                    "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        keys = {"root": root_key, "auth": auth_key}
        username = "WONEMUXPROBE"
        password = "probe_not_a_real_password"
        session_key, _ = woncrypto.decrypt_won_login_data(
            auth_key, SMALL_AUTH_ENCRYPTED_KEY, SMALL_AUTH_ENCRYPTED_DATA)

        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator(
                {service: 0 for service in wonemu.PORTS}, Path(directory), keys=keys,
                community_db=Path(directory) / "community.sqlite3")
            emulator.community.reserve_local_account(
                username, woncrypto.local_auth_user_id(username))
            emulator.start()
            try:
                with socket.create_connection((wonemu.HOST, emulator.ports["auth"]), timeout=2) as conn:
                    conn.sendall(wonemu.frame(bytes.fromhex("05 0c 00 01 00")))
                    self.assertEqual(struct.unpack_from("<BHHh", wonemu.recv_frame(conn)),
                                     (5, 12, 2, 0))
                    login = (bytes.fromhex("05 0c 00 03 00") +
                             struct.pack("<HH", 1, len(SMALL_AUTH_ENCRYPTED_KEY)) +
                             SMALL_AUTH_ENCRYPTED_KEY +
                             struct.pack("<H", len(SMALL_AUTH_ENCRYPTED_DATA)) +
                             SMALL_AUTH_ENCRYPTED_DATA)
                    conn.sendall(wonemu.frame(login))
                    challenge = wonemu.recv_frame(conn)
                    challenge_length, = struct.unpack_from("<H", challenge, 5)
                    self.assertEqual(len(woncrypto.decrypt_won_crypt_transform(
                        session_key, challenge[7:7 + challenge_length])), 16)
                    response_cipher = woncrypto.encrypt_won_crypt_transform(session_key, b"\0")
                    conn.sendall(wonemu.frame(
                        bytes.fromhex("05 0c 00 05 00") +
                        struct.pack("<H", len(response_cipher)) + response_cipher))
                    reply = wonemu.recv_frame(conn)
                self.assertEqual(struct.unpack_from("<BHHhB", reply), (5, 12, 6, 0, 0))
                self.assertIsNone(emulator.private_store)
                self.assertEqual(emulator.community.verify_local_account(username, password),
                                 ("valid", False))
                self.assertEqual(emulator.auth_session_for_peer("127.0.0.1")["username"], username)
                events = [json.loads(line) for line in
                          (Path(directory) / "traffic.jsonl").read_text().splitlines()]
                login_event = next(event for event in events if event["event"] == "login_envelope")
                self.assertTrue(login_event["account_enrolled"])
                self.assertNotIn(password, (Path(directory) / "traffic.jsonl").read_text())
            finally:
                emulator.close()


class NetworkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        # These tests log in over IRC; keep chat local so none reaches a real network.
        self.emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(self.temp.name),
                                        backends=NO_TRIBESNEXT_CHAT)
        self.emulator.start()

    def tearDown(self):
        self.emulator.close()
        self.temp.cleanup()

    def connect(self):
        return socket.create_connection((wonemu.HOST, self.emulator.ports["directory"]), timeout=2)

    def test_native_irc_challenge_round_trip(self):
        auth_key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                    "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        client_key = woncrypto.make_key(auth_key)
        keys = {"root": auth_key, "auth": auth_key}
        username = "irc-test-account"
        session_key = bytes.fromhex("0102030405060708")
        reply = woncrypto.auth1_login_reply(keys, session_key, username, now=1700000000,
                                            client_key=client_key, user_id=7654321)
        temp = tempfile.TemporaryDirectory()
        private_store = Path(temp.name) / "private.store"
        private_store.write_text(
            f"{username}\tignored-private-value\ntribe-friend\tother-private-value\n",
            encoding="ascii")
        private_store.with_name("public.store").write_text(
            f"{username}\t7654321\t10001\tabcdef\tsignature\n", encoding="ascii")
        emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS},
                                   Path(temp.name), keys=keys, private_store=private_store,
                                   backends=NO_TRIBESNEXT_CHAT)
        emulator.start()
        emulator.register_auth_session("127.0.0.1", username, session_key, reply, client_key)
        try:
            with socket.create_connection((wonemu.HOST, emulator.ports["irc"]), timeout=2) as conn:
                stream = conn.makefile("rb")
                conn.sendall(b"dbqa 200 :pre-auth certificate fragment\r\n")
                cert_token = woncrypto.encode_won_token(struct.pack("<II", 0xcb, 0x32))
                conn.sendall(f"CERT {cert_token}\r\n".encode("ascii"))
                challenge_line = stream.readline().decode("ascii").strip()
                challenge_token = challenge_line.split(" ", 2)[2]
                challenge = woncrypto.decrypt_won_irc_challenge(challenge_token, client_key)
                self.assertEqual(len(challenge["challenge"]), 8)
                self.assertEqual(challenge["context"],
                                 woncrypto.auth1_login_material(reply)["auth2_certificate"])

                proof = (struct.pack("<H", len(session_key)) + session_key * 2 +
                         b"legacy client proof extension")
                encrypted_response = woncrypto.encrypt_elgamal_challenge(client_key, proof)
                response_token = woncrypto.encode_won_token(
                    struct.pack("<IIH", 0xcb, 0x34, len(encrypted_response)) + encrypted_response)
                conn.sendall(f"CHALRESP {response_token}\r\n".encode("ascii"))
                final_line = stream.readline().decode("ascii").strip().split()
                self.assertEqual(final_line[1], "CHALRESP_REPLY")
                self.assertEqual(final_line[2], username)
                self.assertEqual(final_line[3], "7654321")

                final = woncrypto.parse_won_irc_token(final_line[4])
                self.assertEqual(final["subtype"], 0x35)
                status, encrypted_length = struct.unpack_from("<HH", final["payload"])
                self.assertEqual(status, 0)
                encrypted = final["payload"][4:4 + encrypted_length]
                decrypted = woncrypto.decrypt_elgamal(client_key, encrypted)
                chunk_length = decrypted[-1]
                chunk = decrypted[-1 - chunk_length:-1]
                self.assertEqual(chunk, struct.pack("<H", len(session_key)) + session_key)
                user_id = 7654321
                conn.sendall(f"dbqax 16 69 0 :{user_id}\t{user_id + 1}\r\n".encode("ascii"))
                presence = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 16 1 :0\t\\S10\\S", presence)
                conn.sendall(b"dbqax 17 23 0 :\r\n")
                dbmsg = stream.readline().decode("utf-8").strip()
                self.assertTrue(dbmsg.startswith(":wonemu.local DBMSG 17 1 :0\t\tirc-test-account\t"))
                self.assertRegex(dbmsg, r"\d{2}/\d{2}/\d{4}")
                self.assertTrue(dbmsg.endswith("\\S\\S"))
                conn.sendall(b"dbqax 18 C3 10 :irc\t0\t10\t0\r\n")
                search_reply = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 18 1 :0\t\\S1\\S0\tirc-test-account", search_reply)
                conn.sendall(b"dbqax 19 17 0 :A local description\r\n")
                update_reply = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 19 1 :0\tUpdated\\S\\S", update_reply)
                conn.sendall(b"dbqax 20 23 0 :irc-test-account\r\n")
                profile_reply = stream.readline().decode("utf-8").strip()
                self.assertIn("\\SA local description\\S", profile_reply)
                long_description = " " + "long-description-" * 35 + " "
                conn.sendall(f"dbqa 200 :{long_description[:400]}\r\n".encode())
                conn.sendall(f"dbqax 200 17 0 :{long_description[400:]}\r\n".encode())
                self.assertIn("DBMSG 200 1 :0\tUpdated", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 201 23 0 :irc-test-account\r\n")
                long_profile = read_dbmsg(stream)
                self.assertIn(long_description, long_profile)
                conn.sendall(b"dbqa 202 :stale canceled text\r\n")
                conn.sendall(b"dbqc 202\r\n")
                conn.sendall(b"dbqax 202 17 0 :fresh after cancellation\r\n")
                self.assertIn("DBMSG 202 1 :0\tUpdated", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 203 23 0 :irc-test-account\r\n")
                cancelled_profile = stream.readline().decode("utf-8").strip()
                self.assertIn("fresh after cancellation", cancelled_profile)
                self.assertNotIn("stale canceled text", cancelled_profile)
                conn.sendall(b"dbqax 21 16 0 :Local Tribe\tLT\t1\t1\t1\tLocal description\r\n")
                create_reply = stream.readline().decode("utf-8").strip()
                self.assertIn(":0\tCreated\\S\\S", create_reply)
                conn.sendall(b"dbqax 204 C10 0 :Local Tribe\r\n")
                tribe_options = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 204 1 :0\t\\S0\\S", tribe_options)
                conn.sendall(b"dbqax 22 22 0 :Local Tribe\r\n")
                tribe_profile = stream.readline().decode("utf-8").strip()
                self.assertIn(":0\t\t1\tLocal Tribe\tLT\t1\t1", tribe_profile)
                self.assertIn("\\SLocal description\\S", tribe_profile)
                conn.sendall(b"dbqax 68 C13 0 :irc-test-account\r\n")
                own_tribes = stream.readline().decode("utf-8").strip()
                self.assertIn("Local Tribe\tLT\t1\t4\t1\tFounder", own_tribes)
                conn.sendall(b"dbqax 23 C6 0 :Local Tribe\r\n")
                tribe_roster = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 23 1 :0\t\\S1\\Sirc-test-account", tribe_roster)
                self.assertTrue(tribe_roster.endswith("\t1\\S"))
                conn.sendall(b"dbqax 24 20 0 :Recruiting\tLocal Tribe\t0\r\n")
                toggle_reply = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 24 1 :0\tUpdated\\S\\S", toggle_reply)
                conn.sendall(b"dbqax 25 21 0 :Local Tribe\tirc-test-account\tCaptain\t3\r\n")
                member_update = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 25 1 :0\tMember profile updated\\S\\S", member_update)
                conn.sendall(b"dbqax 26 C6 0 :Local Tribe\r\n")
                updated_roster = stream.readline().decode("utf-8").strip()
                self.assertIn("irc-test-account\t\t\t", updated_roster)
                self.assertIn("Captain", updated_roster)
                self.assertTrue(updated_roster.endswith("\t1\\S"))
                conn.sendall(b"dbqax 27 27 0 :Local Tribe\ttribe-friend\r\n")
                invite_reply = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 27 1 :0\tInvitation sent\\S\\S", invite_reply)
                conn.sendall(b"dbqax 28 28 0 :cancel\tLocal Tribe\ttribe-friend\r\n")
                cancel_reply = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 28 1 :0\tInvitation updated\\S", cancel_reply)
                conn.sendall(b"dbqax 29 5 0 :irc-test-account\ttribe-friend\tLocal subject\tBody line one\\nBody line two\r\n")
                send_mail = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 29 1 :0\tMail sent\\S2\\S", send_mail)
                conn.sendall(b"dbqax 30 C1 10 :0\r\n")
                inbox = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 30 1 :0\t\\S1\\S1\tirc-test-account", inbox)
                self.assertIn("Local subject", inbox)
                self.assertIn("irc-test-account\ttribe-friend\tLocal subject", inbox)
                self.assertIn("Body line one\\nBody line two", inbox)
                conn.sendall(b"dbqax 31 7 0 :1\r\n")
                self.assertIn("DBMSG 31 1 :0\tUpdated\\S", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 32 6 0 :1\r\n")
                self.assertIn("DBMSG 32 1 :0\tUpdated\\S", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 33 C7 100 :\r\n")
                forum_list = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 33 1 :0\t\\S2\\S1\tGeneral", forum_list)
                self.assertIn("-1\tLocal Tribe", forum_list)
                conn.sendall(b"dbqax 34 12 0 :1\t0\t0\tForum topic\tOpening\\\\S post\r\n")
                self.assertIn("DBMSG 34 1 :0\tPost created", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 35 12 0 :1\t1\t1\tReply subject\tReply body\r\n")
                self.assertIn("DBMSG 35 1 :0\tPost created", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 36 C8 0 :1\r\n")
                topics = stream.readline().decode("utf-8").strip()
                self.assertIn("Forum topic", topics)
                topic_row = topics.split("\\S")[2]
                topic_update_id = topic_row.split("\t")[14]
                conn.sendall(b"dbqax 37 C9 0 :1\t0\r\n")
                posts = stream.readline().decode("utf-8").strip()
                self.assertIn("Opening", posts)
                self.assertIn(" post", posts)
                self.assertIn("Reply body", posts)
                self.assertEqual(posts.count("\\S"), 4)
                conn.sendall(b"dbqax 38 13 0 :1\tEdited topic\tUpdated opening\r\n")
                self.assertIn("DBMSG 38 1 :0\tPost updated", stream.readline().decode("utf-8"))
                conn.sendall(f"dbqax 192 C9 0 :1\t{topic_update_id}\r\n".encode("ascii"))
                updated_posts = read_dbmsg(stream)
                self.assertIn("Updated opening", updated_posts)
                conn.sendall(b"dbqax 39 14 0 :2\r\n")
                self.assertIn("DBMSG 39 1 :0\tPost removed", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 40 1 0 :105\tNews headline\tNews body\r\n")
                self.assertIn("DBMSG 40 1 :0\tArticle submitted",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 41 C100 0 :0\t0\t105\r\n")
                news = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 41 1 :0\t\t1\t0\\S1", news)
                self.assertIn("News headline", news)
                self.assertIn("News body", news)
                conn.sendall(b"dbqax 42 2 0 :1\t1\t1\t1\t35500\tEdited headline\tEdited body\r\n")
                self.assertIn("DBMSG 42 1 :0\tArticle updated",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 43 3 0 :1\t1\r\n")
                self.assertIn("DBMSG 43 1 :0\tArticle deleted",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 44 9 0 :tribe-friend\r\n")
                self.assertIn("DBMSG 44 1 :0\tSender blocked",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 45 C2 100 :\r\n")
                block_list = stream.readline().decode("utf-8").strip()
                self.assertIn("tribe-friend", block_list)
                conn.sendall(b"dbqax 46 8 0 :tribe-friend\r\n")
                self.assertIn("DBMSG 46 1 :0\tBlock removed",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 47 C14 100 :DeletedMail\r\n")
                deleted_mail = stream.readline().decode("utf-8").strip()
                self.assertIn("Local subject", deleted_mail)
                self.assertIn("\t0\t1\t1\tirc-test-account", deleted_mail)
                conn.sendall(b"dbqax 48 12 0 :-1\t0\t0\tTribal topic\tMember-only post\r\n")
                self.assertIn("DBMSG 48 1 :0\tPost created",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 49 C8 0 :-1\r\n")
                tribe_topics = stream.readline().decode("utf-8").strip()
                self.assertIn("Tribal topic", tribe_topics)
                conn.sendall(b"dbqax 80 66 0 :2\tScheduled maintenance\r\n")
                self.assertIn("Topic locked", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 81 12 0 :-1\t2\t3\tReply while locked\tNope\r\n")
                self.assertIn("Forum topic is locked", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 82 67 0 :2\r\n")
                self.assertIn("Topic unlocked", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 83 16 0 :Second Tribe\tST\t1\t1\t1\tA second local tribe\r\n")
                self.assertIn("DBMSG 83 1 :0\tCreated", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 84 68 0 :2\t-2\tSecond Tribe\r\n")
                self.assertIn("Topic moved", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 85 C8 0 :-1\r\n")
                original_forum = stream.readline().decode("utf-8").strip()
                self.assertNotIn("Tribal topic", original_forum)
                conn.sendall(b"dbqax 86 C8 0 :-2\r\n")
                second_forum = stream.readline().decode("utf-8").strip()
                self.assertIn("Tribal topic", second_forum)
                conn.sendall(b"dbqax 87 68 0 :2\t-1\tLocal Tribe\r\n")
                self.assertIn("Topic moved", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 88 12 0 :-1\t0\t0\tTemporary topic\tTemporary body\r\n")
                self.assertIn("Post created", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 89 62 0 :0\t3\t0\r\n")
                self.assertIn("Topic removed", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 90 C8 0 :-1\r\n")
                remaining_topics = stream.readline().decode("utf-8").strip()
                self.assertIn("Tribal topic", remaining_topics)
                self.assertNotIn("Temporary topic", remaining_topics)
                conn.sendall(b"dbqax 50 10 0 :tribe-friend\r\n")
                self.assertIn("DBMSG 50 1 :0\tBuddy added",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 51 C5 0 :\r\n")
                buddy_list = stream.readline().decode("utf-8").strip()
                self.assertIn("tribe-friend", buddy_list)
                conn.sendall(b"dbqax 52 11 0 :tribe-friend\r\n")
                self.assertIn("DBMSG 52 1 :0\tBuddy removed",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 53 10 0 :tribe-friend\r\n")
                stream.readline()
                conn.sendall(b"dbqax 55 27 0 :Local Tribe\ttribe-friend\r\n")
                self.assertIn("DBMSG 55 1 :0\tInvitation sent",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 56 C11 0 :Local Tribe\r\n")
                invites = stream.readline().decode("utf-8").strip()
                self.assertIn("DBMSG 56 1 :0\t\\S1", invites)
                self.assertIn("tribe-friend", invites)
                conn.sendall(b"dbqax 57 28 0 :cancel\tLocal Tribe\ttribe-friend\r\n")
                self.assertIn("Invitation updated", stream.readline().decode("utf-8"))
                friend_session_key = bytes.fromhex("1112131415161718")
                friend_client_key = woncrypto.make_key(auth_key)
                friend_reply = woncrypto.auth1_login_reply(
                    keys, friend_session_key, "tribe-friend", now=1700000000,
                    client_key=friend_client_key)
                emulator.register_auth_session("127.0.0.2", "tribe-friend", friend_session_key,
                                               friend_reply, friend_client_key)
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as friend_conn:
                    friend_conn.settimeout(2)
                    friend_conn.bind(("127.0.0.2", 0))
                    friend_conn.connect((wonemu.HOST, emulator.ports["irc"]))
                    friend_stream = friend_conn.makefile("rb")
                    cert_token = woncrypto.encode_won_token(struct.pack("<II", 0xcb, 0x32))
                    friend_conn.sendall(f"CERT {cert_token}\r\n".encode("ascii"))
                    friend_challenge = friend_stream.readline().decode("ascii").strip().split(" ", 2)[2]
                    woncrypto.decrypt_won_irc_challenge(friend_challenge, friend_client_key)
                    friend_proof = struct.pack("<H", len(friend_session_key)) + friend_session_key * 2
                    friend_cipher = woncrypto.encrypt_elgamal_challenge(friend_client_key, friend_proof)
                    friend_token = woncrypto.encode_won_token(
                        struct.pack("<IIH", 0xcb, 0x34, len(friend_cipher)) + friend_cipher)
                    friend_conn.sendall(f"CHALRESP {friend_token}\r\n".encode("ascii"))
                    self.assertIn("CHALRESP_REPLY", friend_stream.readline().decode("ascii"))
                    friend_conn.sendall(b"dbqax 62 10 0 :irc-test-account\r\n")
                    self.assertIn("Buddy added", friend_stream.readline().decode("utf-8"))
                    friend_conn.sendall(b"dbqax 63 C5 0 :irc-test-account\r\n")
                    viewed_buddies = friend_stream.readline().decode("utf-8").strip()
                    self.assertIn("tribe-friend", viewed_buddies)
                    friend_conn.sendall(b"dbqax 64 C5 0 :\r\n")
                    friend_buddies = friend_stream.readline().decode("utf-8").strip()
                    self.assertIn("irc-test-account", friend_buddies)
                    friend_conn.sendall(b"dbqax 65 C1 10 :0\r\n")
                    friend_inbox = friend_stream.readline().decode("utf-8").strip()
                    self.assertIn("Local subject", friend_inbox)
                    self.assertIn("\t1\t0\t0\tirc-test-account\ttribe-friend", friend_inbox)
                    friend_conn.sendall(b"dbqax 66 34 0 :Local Tribe\r\n")
                    self.assertIn("Invitation request sent",
                                  friend_stream.readline().decode("utf-8"))
                    friend_conn.sendall(b"dbqax 67 C8 0 :-1\r\n")
                    self.assertIn("Tribe membership required",
                                  friend_stream.readline().decode("utf-8"))
                    friend_stream.close()
                conn.sendall(b"dbqax 54 26 0 :clearBuddy\r\n")
                self.assertIn("DBMSG 54 1 :0\tBuddy list cleared",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 58 C11 0 :Local Tribe\r\n")
                request_list = stream.readline().decode("utf-8").strip()
                self.assertIn("tribe-friend", request_list)
                self.assertIn("\t1\t0\\S", request_list)
                conn.sendall(b"dbqax 59 28 0 :accept\tLocal Tribe\ttribe-friend\r\n")
                self.assertIn("Invitation updated", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 73 21 0 :Local Tribe\ttribe-friend\tOfficer\t2\r\n")
                self.assertIn("Member profile updated", stream.readline().decode("utf-8"))
                member_reply = woncrypto.auth1_login_reply(
                    keys, friend_session_key, "tribe-friend", now=1700000001,
                    client_key=friend_client_key)
                emulator.register_auth_session("127.0.0.2", "tribe-friend", friend_session_key,
                                               member_reply, friend_client_key)
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as member_conn:
                    member_conn.settimeout(2)
                    member_conn.bind(("127.0.0.2", 0))
                    member_conn.connect((wonemu.HOST, emulator.ports["irc"]))
                    member_stream = member_conn.makefile("rb")
                    member_conn.sendall(f"CERT {cert_token}\r\n".encode("ascii"))
                    member_challenge = member_stream.readline().decode("ascii").strip().split(" ", 2)[2]
                    woncrypto.decrypt_won_irc_challenge(member_challenge, friend_client_key)
                    member_conn.sendall(f"CHALRESP {friend_token}\r\n".encode("ascii"))
                    self.assertIn("CHALRESP_REPLY", member_stream.readline().decode("ascii"))
                    member_conn.sendall(b"dbqax 79 19 0 :irc-test-account\tLocal Tribe\t0\r\n")
                    self.assertIn("equal- or higher-ranked",
                                  member_stream.readline().decode("utf-8"))
                    member_conn.sendall(b"dbqax 80 66 0 :2\tMember cannot moderate\r\n")
                    self.assertIn("Tribe moderator access required",
                                  member_stream.readline().decode("utf-8"))
                    member_stream.close()
                conn.sendall(b"dbqax 72 27 0 :Local Tribe\ttribe-friend\r\n")
                self.assertIn("already a member", stream.readline().decode("utf-8"))
                for index in range(24):
                    emulator.community.post_news(
                        35500, username, user_id, f"Page article {index}", "Page body")
                conn.sendall(b"dbqax 74 C100 0 :0\t0\t35500\r\n")
                first_page = read_dbmsg(stream)
                self.assertEqual(first_page.count("\\S"), 25)
                self.assertIn("Page article 23", first_page)
                conn.sendall(b"dbqax 75 C0 1 :1\t24\r\n")
                second_page = stream.readline().decode("utf-8").strip()
                self.assertIn("Page article 0", second_page)
                self.assertEqual(second_page.count("\\S"), 3)
                conn.sendall(b"dbqax 70 C13 0 :tribe-friend\r\n")
                friend_tribes = stream.readline().decode("utf-8").strip()
                self.assertIn("Local Tribe\tLT\t1\t2\t1\tOfficer", friend_tribes)
                conn.sendall(b"dbqax 71 C12 0 :tribe-friend\r\n")
                history = stream.readline().decode("utf-8").strip()
                self.assertIn("Local community profile first seen on", history)
                friend_id = woncrypto.local_auth_user_id("tribe-friend")
                conn.sendall(f"dbqax 76 C12 0 :{friend_id}\r\n".encode("ascii"))
                history_by_id = stream.readline().decode("utf-8").strip()
                self.assertIn("Local community profile first seen on", history_by_id)
                conn.sendall(b"dbqax 77 C12 0 :999999999\r\n")
                history_from_profile_context = stream.readline().decode("utf-8").strip()
                self.assertIn("Local community profile first seen on", history_from_profile_context)
                conn.sendall(b"dbqax 78 C13 0 :999999999\r\n")
                own_tribes_by_id = stream.readline().decode("utf-8").strip()
                self.assertIn("Local Tribe\tLT\t1\t3\t1\tCaptain", own_tribes_by_id)
                conn.sendall(b"dbqax 62 C15 0 :WEBLINK\r\n")
                web_links = stream.readline().decode("utf-8").strip()
                self.assertIn("Tribes 2 Home Page", web_links)
                self.assertIn("T2 Technical Information", web_links)
                conn.sendall(b"dbqax 63 0 0 :\r\n")
                motd = stream.readline().decode("utf-8").strip()
                self.assertIn("Welcome to the local Tribes 2 community server", motd)
                conn.sendall(b"dbqax 64 24 0 :Local Tribe\r\n")
                self.assertIn("Membership removed", stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 65 13 0 :3\tFormer member edit\tNo longer allowed\r\n")
                self.assertIn("Tribe membership required",
                              stream.readline().decode("utf-8"))
                conn.sendall(b"dbqax 66 14 0 :3\r\n")
                self.assertIn("Tribe membership required",
                              stream.readline().decode("utf-8"))
                stream.close()
            for _ in range(50):
                if not emulator.online_user_ids():
                    break
                time.sleep(0.01)
            self.assertEqual(emulator.online_user_ids(), set())
        finally:
            emulator.close()
            temp.cleanup()

    def test_reserved_local_account_authenticates_without_private_store(self):
        parameters = woncrypto.verifier_parameters(Path.home() / "t2-linux/tribes2.dynamic")
        root_key = woncrypto.make_key(parameters)
        auth_key = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                    "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}
        keys = {"root": root_key, "auth": auth_key}
        username = "WONEMUXPROBE"
        password = "probe_not_a_real_password"
        session_key, _ = woncrypto.decrypt_won_login_data(
            auth_key, SMALL_AUTH_ENCRYPTED_KEY, SMALL_AUTH_ENCRYPTED_DATA)

        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator(
                {service: 0 for service in wonemu.PORTS}, Path(directory), keys=keys,
                community_db=Path(directory) / "community.sqlite3")
            emulator.community.reserve_local_account(
                username, woncrypto.local_auth_user_id(username))
            emulator.start()
            try:
                with socket.create_connection((wonemu.HOST, emulator.ports["auth"]), timeout=2) as conn:
                    conn.sendall(wonemu.frame(bytes.fromhex("05 0c 00 01 00")))
                    self.assertEqual(struct.unpack_from("<BHHh", wonemu.recv_frame(conn)),
                                     (5, 12, 2, 0))
                    login = (bytes.fromhex("05 0c 00 03 00") +
                             struct.pack("<HH", 1, len(SMALL_AUTH_ENCRYPTED_KEY)) +
                             SMALL_AUTH_ENCRYPTED_KEY +
                             struct.pack("<H", len(SMALL_AUTH_ENCRYPTED_DATA)) +
                             SMALL_AUTH_ENCRYPTED_DATA)
                    conn.sendall(wonemu.frame(login))
                    challenge = wonemu.recv_frame(conn)
                    challenge_length, = struct.unpack_from("<H", challenge, 5)
                    self.assertEqual(len(woncrypto.decrypt_won_crypt_transform(
                        session_key, challenge[7:7 + challenge_length])), 16)
                    response_cipher = woncrypto.encrypt_won_crypt_transform(session_key, b"\0")
                    conn.sendall(wonemu.frame(
                        bytes.fromhex("05 0c 00 05 00") +
                        struct.pack("<H", len(response_cipher)) + response_cipher))
                    reply = wonemu.recv_frame(conn)
                self.assertEqual(struct.unpack_from("<BHHhB", reply), (5, 12, 6, 0, 0))
                self.assertEqual(emulator.community.verify_local_account(username, password),
                                 ("valid", False))
                self.assertIsNone(emulator.private_store)
                session = emulator.auth_session_for_peer("127.0.0.1")
                self.assertEqual(session["username"], username)
            finally:
                emulator.close()

    def test_split_tcp_reads_and_multiple_frames(self):
        with self.connect() as conn:
            conn.sendall(TRIBES2_QUERY[:2])
            conn.sendall(TRIBES2_QUERY[2:10])
            conn.sendall(TRIBES2_QUERY[10:] + AUTH_QUERY)
            self.assertEqual(len(parse_service_reply(wonemu.recv_frame(conn))[2]), 2)
            self.assertEqual(len(parse_service_reply(wonemu.recv_frame(conn))[2]), 1)

    def test_dbproxy_user_id_lookup_uses_private_store_without_logging_identity(self):
        temp = tempfile.TemporaryDirectory()
        store = Path(temp.name) / "private.store"
        store.write_text("local-user\tignored-private-value\n", encoding="ascii")
        store.with_name("public.store").write_text(
            "local-user\t7654321\t10001\tabcdef\tsignature\n", encoding="ascii")
        emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(temp.name),
                                   private_store=store)
        emulator.start()
        try:
            request = (struct.pack("<BHHH", 5, 14, 1002, 16) + wonemu.wide("LOCAL-USER"))
            with socket.create_connection((wonemu.HOST, emulator.ports["dbproxy"]), timeout=2) as conn:
                conn.sendall(wonemu.frame(request))
                reply = wonemu.recv_frame(conn)
                missing_request = (struct.pack("<BHHH", 5, 14, 1002, 16) + wonemu.wide("missing-user"))
                conn.sendall(wonemu.frame(missing_request))
                missing_reply = wonemu.recv_frame(conn)
            header, service, message, submessage, status, error_length = struct.unpack_from(
                "<BHHHhH", reply)
            self.assertEqual((header, service, message, submessage, status, error_length),
                             (5, 14, 1002, 17, 0, 0))
            self.assertEqual(reply[11], 0x80)
            user_id, = struct.unpack_from("<I", reply, 12)
            self.assertEqual(user_id, 7654321)
            missing_status, missing_error_length = struct.unpack_from("<hH", missing_reply, 7)
            self.assertEqual(missing_status, -1)
            self.assertEqual(missing_reply[11:11 + missing_error_length], b"User not found")
            self.assertEqual(missing_reply[11 + missing_error_length], 0x80)

            events = [json.loads(line) for line in (Path(temp.name) / "traffic.jsonl").read_text().splitlines()]
            lookup = next(event for event in events if event["event"] == "dbproxy_account_lookup")
            self.assertTrue(lookup["account_found"])
            log_text = (Path(temp.name) / "traffic.jsonl").read_text()
            self.assertFalse(any(name in log_text for name in ("local-user", "missing-user")))
        finally:
            emulator.close()
            temp.cleanup()

    def test_torque_master_heartbeat_info_and_filtered_list(self):
        temp = tempfile.TemporaryDirectory()
        emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(temp.name),
                                   master_backend="local")
        emulator.start()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.bind((wonemu.HOST, 0))
                client.settimeout(2)
                client.sendto(struct.pack("<BBHH", 22, 0, 0, 0),
                              (wonemu.HOST, emulator.ports["master"]))
                info_request, _ = client.recvfrom(4096)
                packet, flags, session, key = struct.unpack_from("<BBHH", info_request)
                self.assertEqual((packet, flags), (10, 0))

                info_response = (struct.pack("<BBHH", 12, 0, session, key) +
                                 wonemu.torque_string("Tribes 2") + wonemu.torque_string("Arena") +
                                 struct.pack("<BIIBBIB", 32, 0x2, 25034, 0x03, 0, 2200, 0))
                client.sendto(info_response, (wonemu.HOST, emulator.ports["master"]))
                for _ in range(30):
                    if emulator.master_servers:
                        break
                    time.sleep(0.01)
                self.assertEqual(len(emulator.master_servers), 1)

                query = (struct.pack("<BBHHB", 6, 0, 0x1234, 0x5678, 0xff) +
                         wonemu.torque_string("Tribes 2") + wonemu.torque_string("Arena") +
                         struct.pack("<BBIIBBHB", 0, 100, 0, 0, 0, 0, 0, 0))
                client.sendto(query, (wonemu.HOST, emulator.ports["master"]))
                reply, _ = client.recvfrom(4096)
                self.assertEqual(struct.unpack_from("<BBHHBBH", reply), (8, 0, 0x1234, 0x5678, 0, 1, 1))
                self.assertEqual(reply[10:14], socket.inet_aton(wonemu.HOST))
                self.assertEqual(struct.unpack_from("<H", reply, 14)[0], client.getsockname()[1])

                unmatched = query.replace(wonemu.torque_string("Tribes 2"),
                                          wonemu.torque_string("No Such Game"), 1)
                client.sendto(unmatched, (wonemu.HOST, emulator.ports["master"]))
                empty_reply, _ = client.recvfrom(4096)
                self.assertEqual(struct.unpack_from("<BBHHBBH", empty_reply),
                                 (8, 0, 0x1234, 0x5678, 0, 1, 0))
        finally:
            emulator.close()
            temp.cleanup()

    def test_torque_master_translates_tribesnext_http_list_and_types(self):
        from unittest.mock import patch

        temp = tempfile.TemporaryDirectory()
        emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(temp.name),
                                   master_backend="tribesnext")
        query = {"game_type": "Classic CTF", "mission_type": "Map One", "min_players": 2,
                 "max_players": 20, "max_bots": 4, "regions": 0xffffffff, "version": 0,
                 "min_cpu": 0, "info_flags": 3, "buddies": [17, 23]}
        try:
            with patch.object(emulator, "fetch_tribesnext", return_value="192.0.2.4:28002\n") as fetch:
                servers = emulator.tribesnext_servers(query)
            fetch.assert_called_once_with("/list/Classic%20CTF/Map%20One/2/20/4/3%2017%2023", None)
            self.assertEqual(servers, [(socket.inet_aton("192.0.2.4"), 28002)])

            default_query = {"game_type": "Any", "mission_type": "Any", "min_players": 0,
                             "max_players": 255, "max_bots": 16, "regions": 0xffffffff,
                             "version": 0, "min_cpu": 0, "info_flags": 0, "buddies": []}
            with patch.object(emulator, "fetch_tribesnext", return_value="") as fetch:
                emulator.tribesnext_servers(default_query)
            fetch.assert_called_once_with("/list", None)

            with patch.object(emulator, "fetch_tribesnext",
                              return_value="66.42.121.64:28001\n"):
                fission = emulator.tribesnext_servers(default_query)
            self.assertEqual(fission, [(bytes((66, 42, 121, 64)), 28001)])

            with patch.object(emulator, "fetch_tribesnext",
                              return_value="0 CLASSIC\n0 TRIBES2\n1 CTF\n"):
                self.assertEqual(emulator.tribesnext_types(), (["CLASSIC", "TRIBES2"], ["CTF"]))
            with patch.object(emulator, "fetch_tribesnext", return_value="2\n") as heartbeat:
                emulator.publish_tribesnext_heartbeat(("203.0.113.7", 28002))
            heartbeat.assert_called_once_with("/add/28002/203.0.113.7", None)
        finally:
            emulator.log.close()
            temp.cleanup()

    def test_master_sources_parse_in_order_and_reject_bad_entries(self):
        self.assertEqual(wonemu.parse_master_sources("tribesnext, lan,torque=192.0.2.9:28000,lan"), [
            ("tribesnext", wonemu.TRIBESNEXT_MASTER_URL), ("lan", ""), ("torque", "192.0.2.9:28000")])
        self.assertEqual(wonemu.parse_master_sources("tribesnext=https://example.invalid/m/"),
                         [("tribesnext", "https://example.invalid/m")])
        self.assertEqual(wonemu.master_backend_sources("hybrid"),
                         [("tribesnext", wonemu.TRIBESNEXT_MASTER_URL), ("lan", "")])
        for spec in ("gopher=x", "torque=nohost", "torque=host:99999", "tribesnext=ftp://x"):
            with self.assertRaises(ValueError, msg=spec):
                wonemu.parse_master_sources(spec)
        self.assertEqual(wonemu.format_master_sources([("tribesnext", "http://a"), ("lan", "")]),
                         "tribesnext=http://a,lan")

    def test_directory_advertises_the_configured_address(self):
        records = wonemu.directory_records(wonemu.PORTS, "192.0.2.50")
        entity = records["/Tribes2"][1]
        encoded = wonemu.encode_entity(entity, 0x04000000)
        self.assertEqual(encoded, bytes([6]) + struct.pack("!H", wonemu.PORTS["master"]) +
                         socket.inet_aton("192.0.2.50"))
        with tempfile.TemporaryDirectory() as directory:
            emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(directory),
                                       advertise_address="192.0.2.51")
            self.assertEqual(emulator.records["/TitanServers/Auth"][0]["address"], "192.0.2.51")
            emulator.log.close()

    def test_this_machine_means_loopback_or_the_sockets_own_address(self):
        class FakeConnection:
            def __init__(self, local):
                self.local = local

            def getsockname(self):
                return (self.local, 15105)

        lan = FakeConnection("192.0.2.10")
        self.assertTrue(wonemu.from_this_machine(lan, ("127.0.0.1", 40000)))
        self.assertTrue(wonemu.from_this_machine(lan, ("192.0.2.10", 40000)))
        self.assertFalse(wonemu.from_this_machine(lan, ("192.0.2.11", 40000)))

    def test_torque_source_pointing_at_itself_is_refused(self):
        with tempfile.TemporaryDirectory() as directory, self.assertRaisesRegex(ValueError, "own master"):
            wonemu.Emulator(dict(wonemu.PORTS), Path(directory),
                            master_sources=[("torque", f"127.0.0.1:{wonemu.PORTS['master']}")])

    def test_torque_source_reads_another_masters_list_and_types(self):
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            upstream = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(first_dir),
                                       master_sources=[("lan", "")])
            upstream.start()
            try:
                upstream.master_servers[("192.0.2.30", 28000)] = {
                    "endpoint": (socket.inet_aton("192.0.2.30"), 28000), "game_type": "Classic",
                    "mission_type": "CTF", "max_players": 32, "regions": 0, "version": 0,
                    "info_flags": 0, "num_bots": 0, "cpu_speed": 0, "player_count": 3,
                    "players": [], "last_heartbeat": time.monotonic()}
                address = f"{wonemu.HOST}:{upstream.ports['master']}"
                downstream = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(second_dir),
                                             master_sources=[("torque", address), ("lan", "")])
                query = {"game_type": "", "mission_type": "", "min_players": 0, "max_players": 0,
                         "max_bots": 0, "regions": 0, "version": 0, "info_flags": 0, "min_cpu": 0,
                         "buddies": []}
                self.assertEqual(downstream.master_list(query), [(socket.inet_aton("192.0.2.30"), 28000)])
                self.assertEqual(downstream.master_game_types(), (["Classic"], ["CTF"]))
                downstream.log.close()
            finally:
                upstream.close()

    def test_torque_master_empty_types_info_and_paginated_lists(self):
        from unittest.mock import patch

        temp = tempfile.TemporaryDirectory()
        emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(temp.name),
                                   master_backend="tribesnext")
        servers = "".join(f"192.0.2.{(index % 250) + 1}:{28000 + index}\n" for index in range(250))
        def upstream(path, base_url=None):
            if path == "/listtypes":
                return "0 CTF\n1 Arena\n"
            if path == "/list":
                return servers
            raise AssertionError(f"Unexpected TribesNext master path: {path}")

        emulator.start()
        try:
            with (patch.object(emulator, "fetch_tribesnext", side_effect=upstream),
                  socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client):
                    client.bind((wonemu.HOST, 0))
                    client.settimeout(2)
                    client.sendto(struct.pack("<BBHH", 2, 0, 0x1020, 0x3040),
                                  (wonemu.HOST, emulator.ports["master"]))
                    types, _ = client.recvfrom(4096)
                    self.assertEqual(struct.unpack_from("<BBHHB", types), (4, 0, 0x1020, 0x3040, 1))
                    reader = wonemu.Reader(types)
                    reader.take(6)
                    self.assertEqual(reader.number("<B"), 1)
                    self.assertEqual(wonemu.read_torque_string(reader), "CTF")
                    self.assertEqual(reader.number("<B"), 1)
                    self.assertEqual(wonemu.read_torque_string(reader), "Arena")
                    self.assertEqual(reader.offset, len(types))

                    client.sendto(struct.pack("<BBHH", 24, 0, 0x1122, 0x3344),
                                  (wonemu.HOST, emulator.ports["master"]))
                    info, _ = client.recvfrom(4096)
                    self.assertEqual(struct.unpack_from("<BBHH", info), (26, 0, 0x1122, 0x3344))
                    info_reader = wonemu.Reader(info)
                    info_reader.take(6)
                    self.assertEqual(wonemu.read_torque_string(info_reader), "TribesNext / T2Launch Master")
                    self.assertEqual(wonemu.read_torque_string(info_reader), "0")
                    self.assertEqual(info_reader.number("<H"), 250)

                    request = (struct.pack("<BBHHB", 6, 0, 0x1234, 0x5678, 0xff) +
                               wonemu.torque_string("Any") + wonemu.torque_string("Any") +
                               struct.pack("<BBIIBBHB", 0, 100, 0, 0, 0, 0, 0, 0))
                    client.sendto(request, (wonemu.HOST, emulator.ports["master"]))
                    replies = [client.recvfrom(4096)[0], client.recvfrom(4096)[0]]
            headers = [struct.unpack_from("<BBHHBBH", reply) for reply in replies]
            self.assertEqual([header[4:7] for header in headers], [(0, 2, 248), (1, 2, 2)])
            self.assertEqual(sum(header[6] for header in headers), 250)
        finally:
            emulator.close()
            temp.cleanup()

    def test_readiness_identity_and_capture_stub(self):
        status = wonemu.health(self.emulator.ports["directory"])
        self.assertEqual(status["identity"], wonemu.IDENTITY.decode())
        self.assertEqual(status["implementation_sha256"], wonemu.IMPLEMENTATION_SHA256)
        with socket.create_connection((wonemu.HOST, self.emulator.ports["auth"]), timeout=2) as conn:
            conn.sendall(b"\x08\x00\x00\x00probe")
            conn.shutdown(socket.SHUT_WR)
            self.assertEqual(conn.recv(1), b"")
        captured = []
        for _ in range(20):
            captured = [json.loads(line) for line in (Path(self.temp.name) / "traffic.jsonl").read_text().splitlines()]
            if any(event["event"] == "capture_only" for event in captured):
                break
            time.sleep(0.01)
        # Frames are logged by size and hash only; their bytes never reach the log.
        sent = b"\x08\x00\x00\x00probe"
        self.assertTrue(any(event["event"] == "capture_only" and event.get("bytes") == len(sent) and
                            event.get("sha256") == hashlib.sha256(sent).hexdigest()
                            for event in captured))
        log_text = (Path(self.temp.name) / "traffic.jsonl").read_text()
        self.assertNotIn(b"probe".hex(), log_text)
        self.assertNotIn("_hex", log_text)

    def test_invalid_frame_length_closes_connection(self):
        with self.connect() as conn:
            conn.sendall(struct.pack("<I", 0xffffffff))
            self.assertEqual(conn.recv(1), b"")

    def test_auth_emulator_returns_native_rejection_without_fake_success(self):
        parameters = woncrypto.verifier_parameters(Path.home() / "t2-linux/tribes2.dynamic")
        auth_parameters = woncrypto.LOCAL_AUTH_PARAMETERS
        keys = {"root": woncrypto.make_key(parameters),
                "auth": woncrypto.make_key(auth_parameters)}
        temp = tempfile.TemporaryDirectory()
        emulator = wonemu.Emulator({service: 0 for service in wonemu.PORTS}, Path(temp.name), keys=keys)
        emulator.start()
        try:
            with socket.create_connection((wonemu.HOST, emulator.ports["auth"]), timeout=2) as conn:
                conn.sendall(wonemu.frame(bytes.fromhex("05 0c 00 01 00")))
                response = wonemu.recv_frame(conn)
                self.assertEqual(struct.unpack_from("<BHHh", response), (5, 12, 2, 0))
                invalid_login = (bytes.fromhex("05 0c 00 03 00") + struct.pack("<HHH", 1, 0, 0))
                conn.sendall(wonemu.frame(invalid_login))
                self.assertEqual(wonemu.recv_frame(conn), struct.pack("<BHHh", 5, 12, 6, -1))
        finally:
            emulator.close()
            temp.cleanup()


class StockAccountDialogTests(unittest.TestCase):
    """DBProxy exchanges captured from the stock #25034 login screen."""

    AUTH_KEY = {"p": SMALL_AUTH_P, "q": SMALL_AUTH_Q, "g": 4,
                "x": SMALL_AUTH_X, "y": pow(4, SMALL_AUTH_X, SMALL_AUTH_P)}

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.keys = {"root": self.AUTH_KEY, "auth": self.AUTH_KEY}
        self.emulator = wonemu.Emulator(
            {service: 0 for service in wonemu.PORTS}, Path(self.temp.name), keys=self.keys,
            backends=[{"kind": "native", "auth": True, "master": False, "irc": False}])
        self.emulator.start()

    def tearDown(self):
        self.emulator.close()
        self.temp.cleanup()

    def dbproxy(self):
        return socket.create_connection((wonemu.HOST, self.emulator.ports["dbproxy"]), timeout=5)

    @staticmethod
    def exchange(conn, payload):
        conn.sendall(wonemu.frame(payload))
        return wonemu.recv_frame(conn)

    @staticmethod
    def reply_status(reply, sub):
        header, service, message, reply_sub, status, length = struct.unpack_from("<BHHHhH", reply)
        assert (header, service, message, reply_sub) == (5, 14, 1002, sub), reply[:7].hex()
        text = reply[11:11 + length].decode("ascii")
        assert reply[11 + length] == 0x80
        return status, text, reply[12 + length:]

    @staticmethod
    def create_message(login, password, email, cd_key, warrior, send_info):
        product = (struct.pack("<H", 0x62) + bytes([len(cd_key)]) + cd_key.encode() +
                   bytes([len(warrior)]) + warrior.encode())
        return (bytes.fromhex("05 0e00 ea03 0200") + wonemu.wide(login) + wonemu.wide(password) +
                struct.pack("<H", len(email)) + email.encode() + struct.pack("<H", 2) +
                struct.pack("<HHHB", 1, 3, 1, int(send_info)) +
                struct.pack("<HH", 2, len(product)) + product)

    def create(self, message):
        """Key request, wrapped session key, then the encrypted request, as the client sends them."""
        with self.dbproxy() as conn:
            reply = self.exchange(conn, bytes.fromhex("05 0e00 0200 0100"))
            self.assertEqual(struct.unpack_from("<BHHHhHB", reply), (5, 14, 3, 0, 0, 0, 0x80))
            length, = struct.unpack_from("<H", reply, 12)
            server_key = woncrypto.read_der_public(reply[14:14 + length])
            session_key = bytes.fromhex("1122334455667788")
            wrapped = woncrypto.encrypt_elgamal_challenge(server_key, session_key)
            conn.sendall(wonemu.frame(struct.pack("<BHHHH", 5, 14, 4, 1, len(wrapped)) + wrapped))
            return self.exchange(conn, struct.pack("<BHHH", 5, 14, 5, 1) +
                                 woncrypto.encrypt_won_crypt_transform(session_key, message))

    def peer_login(self, username, password):
        """Auth1 session, then the 0x32/0x33/0x34/0x35 peer exchange; returns the session key."""
        client_key = woncrypto.make_key(self.AUTH_KEY)
        reply = woncrypto.auth1_login_reply(self.keys, bytes(8), username, client_key=client_key)
        self.emulator.register_auth_session(wonemu.HOST, username, bytes(8), reply, client_key)
        with self.dbproxy() as conn:
            challenge_message = self.exchange(
                conn, struct.pack("<II", 0xcb, 0x32) + bytes.fromhex("0201 0000 0000"))
            challenge = woncrypto.decrypt_won_irc_challenge(
                woncrypto.encode_won_token(challenge_message), client_key)
            certificate = challenge["context"]
            signature_bytes = 2 * ((self.AUTH_KEY["q"].bit_length() + 7) // 8)
            self.assertTrue(woncrypto.verify(self.AUTH_KEY, certificate[:-signature_bytes],
                                             certificate[-signature_bytes:]))
            key_length, = struct.unpack_from("<H", certificate, 20)
            server_key = woncrypto.read_der_public(certificate[22:22 + key_length])
            secret = bytes.fromhex("a1a2a3a4a5a6a7a8")
            proof = woncrypto.encrypt_elgamal_challenge(
                server_key, struct.pack("<H", len(challenge["challenge"])) + challenge["challenge"] + secret)
            complete = self.exchange(conn, struct.pack("<IIH", 0xcb, 0x34, len(proof)) + proof)
            self.assertEqual(struct.unpack_from("<II", complete), (0xcb, 0x35))
        return challenge["challenge"]

    def peer_request(self, key, sequence, message):
        # A new connection each time: the client reuses the session across them.
        with self.dbproxy() as conn:
            reply = self.exchange(conn, woncrypto.encrypt_won_peer_message(key, 0, sequence, message))
        _session, reply_sequence, plain = woncrypto.decrypt_won_peer_message(key, reply)
        self.assertEqual(reply_sequence, sequence)
        return plain

    def test_create_account_registers_a_native_account(self):
        key = woncrypto.generate_cd_key()
        reply = self.create(self.create_message(
            "NEWPROBE", "probe-pass-1", "newprobe@example.invalid", key, "NewProbe", True))
        self.assertEqual(self.reply_status(reply, 3), (0, "", b""))
        community = self.emulator.community
        self.assertEqual(community.verify_local_account("newprobe", "probe-pass-1"), ("valid", False))
        info = community.account_info("NEWPROBE")
        # The login name arrives uppercased; the warrior name keeps its case.
        self.assertEqual((info["username"], info["provider"], info["email"], info["send_info"]),
                         ("NewProbe", "native", "newprobe@example.invalid", 1))
        again = self.create(self.create_message(
            "NEWPROBE", "other-pass", "x@example.invalid", key, "NewProbe", False))
        self.assertEqual(self.reply_status(again, 3)[0], wonemu.DBPROXY_USER_EXISTS)
        bad_key = self.create(self.create_message(
            "OTHER", "probe-pass-1", "o@example.invalid", "AAAA-AAAA-AAAA-AAAA-AAAA", "Other", False))
        self.assertEqual(self.reply_status(bad_key, 3)[0], wonemu.DBPROXY_INVALID_CDKEY)
        self.emulator.log.close()
        log_text = (Path(self.temp.name) / "traffic.jsonl").read_text()
        for secret in ("probe-pass-1", "newprobe@example.invalid", key):
            self.assertNotIn(secret, log_text)

    def test_edit_account_shows_and_updates_the_logged_in_account(self):
        community = self.emulator.community
        community.create_local_account("Editor", 4242, "old-pass")
        community.set_account_info("Editor", "native", "old@example.invalid", True)
        key = self.peer_login("Editor", "old-pass")
        info = self.peer_request(key, 1, bytes.fromhex("05 0e00 ea03 0600 0100 0100 0200 0100"))
        status, _text, body = self.reply_status(info, 7)
        self.assertEqual(status, 0)
        self.assertEqual(body, bytes([19]) + b"old@example.invalid" +
                         struct.pack("<HHHHB", 1, 1, 3, 1, 1))
        update = (bytes.fromhex("05 0e00 ea03 0400") + wonemu.wide("new-pass") +
                  struct.pack("<H", 19) + b"new@example.invalid" + struct.pack("<HHHHB", 1, 1, 3, 1, 0))
        self.assertEqual(self.reply_status(self.peer_request(key, 2, update), 5)[0], 0)
        self.assertEqual(community.verify_local_account("Editor", "new-pass"), ("valid", False))
        self.assertEqual(community.verify_local_account("Editor", "old-pass"), ("invalid", False))
        self.assertEqual(community.account_info("Editor")["email"], "new@example.invalid")
        self.assertEqual(community.account_info("Editor")["send_info"], 0)
        # Replayed or reordered messages are refused.
        with self.dbproxy() as conn:
            conn.sendall(wonemu.frame(woncrypto.encrypt_won_peer_message(key, 0, 2, update)))
            self.assertIsNone(wonemu.recv_frame(conn))

    def test_email_fetches_report_codes_the_stock_script_knows(self):
        self.emulator.community.create_local_account("Known", 77, "pass-known")
        with self.dbproxy() as conn:
            known = self.exchange(conn, bytes.fromhex("05 0e00 ea03 0800") + wonemu.wide("KNOWN") + bytes(2))
            unknown = self.exchange(conn, bytes.fromhex("05 0e00 ea03 0800") + wonemu.wide("NOBODY") + bytes(2))
            by_email = self.exchange(conn, bytes.fromhex("05 0e00 ea03 0e00") +
                                     struct.pack("<H", 9) + b"a@b.c.def" + bytes(2))
        self.assertEqual(self.reply_status(known, 9)[0], wonemu.DBPROXY_NO_MODULE)
        self.assertEqual(self.reply_status(unknown, 9)[0], wonemu.DBPROXY_USER_DOES_NOT_EXIST)
        self.assertEqual(self.reply_status(by_email, 15)[0], wonemu.DBPROXY_NO_MODULE)

    def test_tribesnext_creation_maps_server_answers_and_stays_on_this_machine(self):
        emulator = self.emulator
        emulator.auth_order = ("tribesnext", "native")
        emulator.private_store = Path(self.temp.name) / "private.store"
        emulator.public_store = Path(self.temp.name) / "public.store"
        fields = {"login": "TNPROBE", "password": "probe-pass-1", "email": "t@example.invalid",
                  "cd_key": woncrypto.generate_cd_key(), "warrior": "TnProbe", "send_info": False}
        calls = []
        original = tribesnext.create_account
        try:
            tribesnext.create_account = lambda *args, **kwargs: (_ for _ in ()).throw(
                tribesnext.AccountServerError("Account name is already taken", "TAKEN"))
            self.assertEqual(emulator.create_stock_account(fields, True)[0], wonemu.DBPROXY_USER_EXISTS)
            tribesnext.create_account = lambda *args, **kwargs: (_ for _ in ()).throw(OSError("down"))
            self.assertEqual(emulator.create_stock_account(fields, True), (wonemu.DBPROXY_DB_ERROR, "OSError"))
            tribesnext.create_account = lambda name, password, *args, **kwargs: calls.append(name)
            self.assertEqual(emulator.create_stock_account(fields, True), (0, ""))
            # Another machine never gets a TribesNext account; it gets a native one.
            remote = dict(fields, login="REMOTE", warrior="Remote")
            self.assertEqual(emulator.create_stock_account(remote, False), (0, ""))
        finally:
            tribesnext.create_account = original
        self.assertEqual(calls, ["TnProbe"])
        self.assertEqual(emulator.community.account_info("TnProbe")["provider"], "tribesnext")
        self.assertEqual(emulator.community.account_info("Remote")["provider"], "native")
        emulator.account_creation = False
        self.assertEqual(emulator.create_stock_account(dict(fields, login="LATE", warrior="Late"), True)[0],
                         wonemu.DBPROXY_SHOW_TEXT)

    def test_tribesnext_password_cannot_change_from_edit_account(self):
        private_hex = "00112233445566778899aabbccddeeff"
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        rc4_key = hashlib.sha1(("tn-pass" + nonce).encode("ascii")).hexdigest().encode("ascii")
        store = Path(self.temp.name) / "private.store"
        store.write_text(f"TnUser\t{nonce}:{woncrypto._rc4_drop2048(rc4_key, bytes.fromhex(private_hex)).hex()}\n",
                         encoding="ascii")
        self.emulator.private_store = store
        peer = {"username": "TnUser", "auth_provider": "tribesnext"}
        changed = {"password": "different", "email": "x@example.invalid", "send_info": True}
        same = dict(changed, password="tn-pass")
        self.assertEqual(self.emulator.update_stock_account(peer, changed)[0], wonemu.DBPROXY_SHOW_TEXT)
        self.assertIsNone(self.emulator.community.account_info("TnUser"))
        self.assertEqual(self.emulator.update_stock_account(peer, same), (0, ""))
        self.assertEqual(self.emulator.community.account_info("TnUser")["email"], "x@example.invalid")


if __name__ == "__main__":
    unittest.main()
