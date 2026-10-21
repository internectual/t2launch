import hashlib
import socket
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

import tribesnext
from woncrypto import _rc4_drop2048


class CertificateTests(unittest.TestCase):
    def test_parses_wire_fields_without_normalizing_signed_text(self):
        cert = tribesnext.parse_certificate("Name\t00042\t03\t00a1\t0f")
        self.assertEqual(cert.guid, 42)
        self.assertEqual(cert.exponent, 3)
        self.assertEqual(cert.wire_text, "Name\t00042\t03\t00a1\t0f")

    def test_rejects_malformed_certificate(self):
        with self.assertRaises(ValueError):
            tribesnext.parse_certificate("Name\t42\t3\txyz\t1")
        with self.assertRaises(ValueError):
            tribesnext.parse_certificate("Name\t-1\t3\ta1\t1")

    def test_issuer_signature_is_preserved_for_game_server_verification(self):
        cert = tribesnext.parse_certificate("Name\t42\t11\tcA1\tdeadbeef")
        self.assertEqual(cert.wire_text, "Name\t42\t11\tcA1\tdeadbeef")


class AccountStoreTests(unittest.TestCase):
    def test_existing_store_unlock_checks_password_and_public_private_match(self):
        username = "TestAccount"
        password = "local-only-test-password"
        private_hex = "0ac1"  # RSA d=2753 for p=61, q=53, e=17.
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
        encrypted = _rc4_drop2048(key, bytes.fromhex(private_hex)).hex()
        with tempfile.TemporaryDirectory() as directory:
            public_path = Path(directory) / "public.store"
            private_path = Path(directory) / "private.store"
            private_path.write_text(f"{username}\t{nonce}:{encrypted}\n", encoding="ascii")
            public_path.write_text(f"{username}\t42\t11\tcA1\t1\n", encoding="ascii")
            credentials = tribesnext.unlock_account(public_path, private_path,
                                                   username, password)
            self.assertEqual(credentials.private_exponent, 0xAC1)
            with self.assertRaises(ValueError):
                tribesnext.unlock_account(public_path, private_path, username, "wrong")


class ChallengeTests(unittest.TestCase):
    def setUp(self):
        p, q, exponent = 2305843009213693951, 2147483647, 65537
        modulus = p * q
        private_exponent = pow(exponent, -1, (p - 1) * (q - 1))
        cert = tribesnext.AccountCertificate("Name", 42, exponent, modulus, 1,
                                             f"Name\t42\t{exponent:x}\t{modulus:x}\t1")
        self.credentials = tribesnext.AccountCredentials(cert, private_exponent)


class RobotClientTests(unittest.TestCase):
    def setUp(self):
        modulus = (1 << 1024) - 109
        cert = tribesnext.AccountCertificate(
            "Name", 42, 1, modulus, 1,
            f"Name\t42\t1\t{modulus:x}\t1")
        self.credentials = tribesnext.AccountCredentials(cert, 1)

    def test_robot_login_challenge_and_browser_get(self):
        client = tribesnext.RobotClient()
        nonce = "1" + "2" * 127
        response = "abc123"
        encrypted = format(int(nonce + response, 16), "x")
        requests = []

        def request(endpoint, params=None, **kwargs):
            requests.append((endpoint, params, kwargs))
            if "nonce" in (params or {}):
                return ["", f"CHALLENGE: {encrypted}"]
            if "response" in (params or {}):
                return ["UUID: session-token"]
            return ["profile"]

        client._request = request
        with patch.object(client, "_new_nonce", return_value=nonce):
            self.assertEqual(client.browser_get(self.credentials, "profile", page=2),
                             ["profile"])
        self.assertEqual(requests[0][0], "robot_login.php")
        self.assertEqual(requests[0][1], {"guid": "42", "nonce": nonce})
        self.assertEqual(requests[1][1], {"guid": "42", "response": response})
        self.assertEqual(requests[2][0], "robot_browser.php")
        self.assertEqual(requests[2][1], {
            "guid": "42", "uuid": "session-token", "method": "profile", "page": "2"})

    def test_robot_browser_post_uses_multipart_fields_and_reuses_session(self):
        client = tribesnext.RobotClient()
        client._sessions[42] = {"uuid": "session-token", "refreshed_at": 10**20}
        calls = []

        def request(endpoint, params=None, **kwargs):
            calls.append((endpoint, params, kwargs))
            return ["ok"]

        client._request = request
        self.assertEqual(client.browser_post(self.credentials, "message", body="hello"), ["ok"])
        endpoint, params, kwargs = calls[0]
        self.assertEqual(endpoint, "robot_browser.php")
        self.assertIsNone(params)
        self.assertIn(b'name="guid"', kwargs["body"])
        self.assertIn(b'name="uuid"', kwargs["body"])
        self.assertIn(b'name="method"', kwargs["body"])
        self.assertIn(b'name="body"', kwargs["body"])
        self.assertIn(b"hello", kwargs["body"])
        self.assertTrue(kwargs["content_type"].startswith("multipart/form-data; boundary="))

    def test_robot_rejects_error_and_nonce_mismatch(self):
        client = tribesnext.RobotClient()
        with self.assertRaisesRegex(tribesnext.RobotAPIError, "denied"):
            client._line_value(["ERR: denied"], "UUID: ")

        nonce = "1" + "2" * 127
        client._request = lambda *args, **kwargs: ["CHALLENGE: 2"]
        with (patch.object(client, "_new_nonce", return_value=nonce),
              self.assertRaisesRegex(tribesnext.RobotAPIError, "did not echo")):
            client._start_session(self.credentials)

    def test_user_view_maps_official_robot_profile_and_membership_rows(self):
        profile = tribesnext.parse_robot_profile([
            "PLAYER\tNAME\t42\tLouCypher",
            "PLAYER\tTAG\t42\t[ABC]\t1",
            "PLAYER\tDATE\t42\t1234567890",
            "PLAYER\tSITE\t42\thttps://example.invalid",
            "PLAYER\tINFO\t42\tline one\\nline two\\tmore",
            "PLAYER\tONLINE\t42\t1",
            "PLAYER\tCLAN\t42\t0\t77\tExample Clan\t4\tFounder\\tRank\t[ABC]\t1",
        ])
        self.assertEqual(profile, {
            "memberships": [{"id": "77", "name": "Example Clan", "rank": "4",
                             "title": "Founder\tRank", "tag": "[ABC]", "append": "1"}],
            "name": "LouCypher", "tag": "[ABC]", "append": "1", "creation": "1234567890",
            "website": "https://example.invalid", "info": "line one\nline two\tmore",
            "online": "1",
        })

    def test_user_view_uses_official_uview_argument_and_rejects_missing_profile(self):
        client = tribesnext.RobotClient()
        observed = []
        client.browser_get = lambda credentials, method, **args: (
            observed.append((method, args)) or ["PLAYER\tNAME\t42\tName"])
        self.assertEqual(client.user_view(self.credentials, 42)["name"], "Name")
        self.assertEqual(observed, [("uview", {"a0": 42})])
        with self.assertRaisesRegex(tribesnext.RobotAPIError, "no usable profile"):
            tribesnext.parse_robot_profile(["TIMEOUT"])

    def test_user_history_parses_phist_rows_and_expands_templates(self):
        events = tribesnext.parse_robot_history([
            "",
            ("PHIST\t42\t1\tRANK\t1300000000\t4\\tFounder\t@player ranked @payload;0"
             " (@payload;1)\tLou\t[ABC]\t1\t42\t77\tExample Clan\t[ABC]\t1"),
            ("PHIST\t42\t0\tJOIN\t1200000000\t0\t@player joined @clan, admin @payload^"
             "\tLou\t[ABC]\t0\t42\t77\tExample Clan\t[ABC]\t0"),
        ])
        self.assertEqual([event["type"] for event in events], ["JOIN", "RANK"])
        self.assertEqual(tribesnext.expand_history_event(events[0]),
                         "[ABC]Lou joined Example Clan, admin NO")
        self.assertEqual(tribesnext.expand_history_event(events[1]),
                         "Lou[ABC] ranked 4 (Founder)")
        with self.assertRaisesRegex(tribesnext.RobotAPIError, "UNAUTHENTICATED"):
            tribesnext.parse_robot_history(["ERR\tBROWSER\tUNAUTHENTICATED"])

    def test_buddy_list_uses_mail_robot_and_parses_list_rows(self):
        client = tribesnext.RobotClient()
        client._sessions[42] = {"uuid": "session-token", "refreshed_at": 10**20}
        calls = []

        def request(endpoint, params=None, **kwargs):
            calls.append((endpoint, params))
            if params["method"] == "viewlist":
                return ["", "LIST\tbuddy\t1\tSecond\t\t0\t99",
                        "LIST\tignore\t0\tIgnored\t\t0\t7", "LIST\tbuddy\t0\tFirst\t[T]\t1\t98"]
            return [""]

        client._request = request
        self.assertEqual(client.buddy_list(self.credentials), [
            {"name": "First", "tag": "[T]", "append": "1", "guid": "98"},
            {"name": "Second", "tag": "", "append": "0", "guid": "99"}])
        client.add_buddy(self.credentials, 98)
        client.remove_buddy(self.credentials, 99)
        self.assertEqual([(endpoint, params["method"], params.get("target"))
                          for endpoint, params in calls], [
            ("robot_mail.php", "viewlist", None), ("robot_mail.php", "addlist", "98"),
            ("robot_mail.php", "dellist", "99")])
        self.assertTrue(all(params["list"] == "buddy" for _, params in calls))
        client._request = lambda *args, **kwargs: ["ERR\tMAIL\tUNAUTHENTICATED"]
        with self.assertRaisesRegex(tribesnext.RobotAPIError, "UNAUTHENTICATED"):
            client.add_buddy(self.credentials, 98)

    def test_mail_robot_box_read_and_send_parse_official_rows(self):
        self.assertEqual(tribesnext.parse_robot_mailbox([
            "", "MSG\t7\tinbox\tfalse\tinvite\t1300000000", "SBJ\t7\tJoin\\tus",
            "SNDR\t7\tLou\t[ABC]\t1\t42", "SBJ\t99\torphan"]), [
            {"id": 7, "box": "inbox", "read": False, "type": "invite", "time": "1300000000",
             "subject": "Join\tus", "sender": ["Lou", "[ABC]", "1", "42"]}])
        message = tribesnext.parse_robot_message([
            "MSG2\t7\ttrue\tmessage\t1300000000", "SBJ\t7\tHi", "SNDR\t7\tLou\t\t0\t42",
            "TO\t7\t1\tSecond\t\t0\t44", "TO\t7\t0\tFirst\t\t0\t43", "BDY\t7\ta\\nb\\\\c"])
        self.assertEqual(message["body"], "a\nb\\c")
        self.assertTrue(message["deleted"])
        self.assertEqual([player[0] for player in message["to"]], ["First", "Second"])
        with self.assertRaisesRegex(tribesnext.RobotAPIError, "READ"):
            tribesnext.parse_robot_message(["ERR\tMAIL\tREAD\t7"])
        self.assertEqual(tribesnext.parse_robot_mail_send(["", "SUCCESS\t8"]), 8)
        with self.assertRaisesRegex(tribesnext.RobotAPIError, "Invalid recipient"):
            tribesnext.parse_robot_mail_send(["ERR\tMAIL\tINVALID_RECIP"])

        client = tribesnext.RobotClient()
        sent = []
        client.mail_post = lambda credentials, method, **fields: (
            sent.append(fields) or ["SUCCESS\t9"])
        self.assertEqual(client.send_mail(self.credentials, "S", "B", [43, 44], []), 9)
        self.assertEqual(sent[0]["to"], "43,44")
        self.assertEqual(sent[0]["cc"], "43")

    def test_user_and_clan_search_rows_parse_official_fields(self):
        self.assertEqual(tribesnext.parse_robot_search([
            "SEARCH\tLou\t0\tLouCypher\t[ABC]\t1\t42"], "user"),
            [{"index": "0", "name": "LouCypher", "tag": "[ABC]",
              "append": "1", "guid": "42"}])
        self.assertEqual(tribesnext.parse_robot_search([
            "CSEARCH\tExample\t0\t77\tExample Clan"], "clan"),
            [{"index": "0", "id": "77", "name": "Example Clan"}])
        with self.assertRaisesRegex(tribesnext.RobotAPIError, "player GUID"):
            tribesnext.parse_robot_search(
                ["SEARCH\tLou\t0\tLouCypher\t[ABC]\t1\tinvalid"], "user")

    def test_clan_view_maps_official_detail_and_roster_rows(self):
        clan = tribesnext.parse_robot_clan([
            "CLAN\tNAME\t77\tExample Clan",
            "CLAN\tTAG\t77\t[ABC]\t1",
            "CLAN\tRECR\t77\t1",
            "CLAN\tSITE\t77\thttps://example.invalid",
            "CLAN\tDATE\t77\t1234567890",
            "CLAN\tPICT\t77\tteam.png",
            "CLAN\tACTIVE\t77\t1",
            "CLAN\tINFO\t77\tClan\\ninfo",
            "CLAN\tMEMB\t77\t0\tName\t[TN]\t0\t42\t4\tFounder\\tRank\t1",
        ])
        self.assertEqual(clan, {
            "members": [{"name": "Name", "tag": "[TN]", "append": "0", "guid": "42",
                         "rank": "4", "title": "Founder\tRank", "online": "1"}],
            "name": "Example Clan", "tag": "[ABC]", "append": "1", "recruiting": "1",
            "website": "https://example.invalid", "creation": "1234567890",
            "picture": "team.png", "active": "1", "info": "Clan\ninfo",
        })

    def test_client_challenge_encodes_nonce_and_server_address(self):
        self.assertEqual(tribesnext.make_client_challenge("192.0.2.1", 0x123),
                         "123c0000201")

    def test_ipv4_suffix_uses_two_digit_hex_octets(self):
        self.assertEqual(tribesnext.ipv4_hex("64.177.124.10"), "40b17c0a")
        self.assertEqual(tribesnext.ipv4_hex("66.42.121.64"), "422a7940")

    def test_decrypts_and_validates_server_challenge(self):
        client = tribesnext.make_client_challenge("0.0.0.1", 1)
        server_half = "200000002"
        message = int(client + server_half, 16)
        ciphertext = format(pow(message, self.credentials.certificate.exponent,
                                self.credentials.certificate.modulus), "x")
        result = tribesnext.decrypt_server_challenge(self.credentials, ciphertext, client)
        self.assertEqual(result, server_half)

    def test_rejects_challenge_with_wrong_nonce(self):
        client = tribesnext.make_client_challenge("0.0.0.1", 1)
        server_half = "200000002"
        message = int(client + server_half, 16)
        ciphertext = format(pow(message, self.credentials.certificate.exponent,
                                self.credentials.certificate.modulus), "x")
        with self.assertRaises(ValueError):
            tribesnext.decrypt_server_challenge(self.credentials, ciphertext, "f" + client[1:])


class RecoveryTests(unittest.TestCase):
    def test_recovery_request_matches_t2csri_formula(self):
        payload = tribesnext.recovery_request("LouCypher", "secret", 1234567890)
        fields = payload.decode("ascii").rstrip("\n").split("\t")
        auth_stored = hashlib.sha1(b"3.14159265loucyphersecret").hexdigest()
        time_nonce = hashlib.sha1(b"1234567890loucypher").hexdigest()
        self.assertEqual(fields[:3], ["RECOVER", "LouCypher", "1234567890"])
        self.assertEqual(fields[3], hashlib.sha1((auth_stored + time_nonce).encode()).hexdigest())

    def test_parses_recovery_response_without_trusting_source_tree_signer(self):
        certificate = "Name\t42\t11\tcA1\tdeadbeef"
        got = tribesnext.parse_recovery_response(
            f"CERT: {certificate}\r\nEXP: {('a' * 40)}\r\n".encode("ascii"), "Name")
        self.assertEqual(got, (certificate, "a" * 40))
        with self.assertRaisesRegex(ValueError, "INVALIDPASSWORD"):
            tribesnext.parse_recovery_response(b"INVALIDPASSWORD", "Name")

    def test_recovery_updates_existing_store_files_and_unlocks_without_new_account(self):
        username, password = "TestAccount", "recovery-test-password"
        private_hex = "0ac1"
        nonce = hashlib.sha1(private_hex.encode("ascii")).hexdigest()
        key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
        encrypted = _rc4_drop2048(key, bytes.fromhex(private_hex)).hex()
        certificate = f"{username}\t42\t11\tcA1\tdeadbeef"
        response = f"CERT: {certificate}\r\nEXP: {nonce}:{encrypted}\r\n".encode("ascii")
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        received = []

        def serve_once():
            conn, _ = server.accept()
            with conn:
                conn.settimeout(2)
                received.append(conn.recv(4096))
                conn.sendall(response)
            server.close()

        thread = threading.Thread(target=serve_once)
        thread.start()
        with tempfile.TemporaryDirectory() as directory:
            public_path = Path(directory) / "public.store"
            private_path = Path(directory) / "private.store"
            credentials = tribesnext.recover_account(
                username, password, public_path, private_path, server.getsockname())
            self.assertEqual(credentials.private_exponent, 0xAC1)
            self.assertEqual(public_path.read_text("ascii").strip(), certificate)
            self.assertEqual(private_path.read_text("ascii").strip(),
                             f"{username}\t{nonce}:{encrypted}")
            self.assertEqual(public_path.stat().st_mode & 0o777, 0o600)
        thread.join(timeout=2)
        self.assertFalse(thread.is_alive())
        self.assertTrue(received[0].startswith(b"RECOVER\tTestAccount\t"))


class AccountCreationTests(unittest.TestCase):
    def test_registration_signs_protocol_request_and_persists_unlockable_stores(self):
        username, password = "NewAccount", "account-create-password"
        exponent, modulus, private_exponent = 17, 0xCA1, 0xAC1
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        requests = []

        def serve_registration():
            for response in (b"SUCCESS\n", None):
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(2)
                    payload = bytearray()
                    while not payload.endswith(b"\n"):
                        block = conn.recv(4096)
                        if not block:
                            break
                        payload.extend(block)
                    request = bytes(payload)
                    requests.append(request)
                    if response is not None:
                        conn.sendall(response)
                    else:
                        fields = request.decode("ascii").strip().split("\t")
                        self.assertEqual(fields[0], "SIGN")
                        signed = "\t".join(fields[1:-1])
                        signature = int(fields[-1], 16)
                        expected = int(hashlib.sha1(signed.encode("ascii")).hexdigest(), 16)
                        self.assertEqual(pow(signature, exponent, modulus), expected % modulus)
                        self.assertEqual(fields[1], username)
                        self.assertEqual(fields[2], format(exponent, "x"))
                        self.assertIn(":", fields[4])
                        self.assertEqual(fields[5], hashlib.sha1(
                            ("3.14159265" + username.lower() + password).encode("ascii")
                        ).hexdigest())
                        conn.sendall(f"{username}\t42\t{exponent:x}\t{modulus:x}\t1\n".encode())
            listener.close()

        thread = threading.Thread(target=serve_registration)
        thread.start()
        with tempfile.TemporaryDirectory() as directory:
            public_path = Path(directory) / "public.store"
            private_path = Path(directory) / "private.store"
            with patch.object(tribesnext, "generate_account_key",
                              return_value=(exponent, modulus, private_exponent)):
                credentials = tribesnext.create_account(
                    username, password, public_path, private_path,
                    endpoint=listener.getsockname(), timeout=2)
            self.assertEqual(credentials.private_exponent, private_exponent)
            self.assertEqual(credentials.certificate.guid, 42)
            self.assertEqual(public_path.read_text("ascii").strip(),
                             f"{username}\t42\t{exponent:x}\t{modulus:x}\t1")
            self.assertEqual(tribesnext.unlock_account(
                public_path, private_path, username, password).private_exponent,
                private_exponent)
            self.assertEqual(public_path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(private_path.stat().st_mode & 0o777, 0o600)
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        self.assertEqual(requests[0], f"NAME\t{username}\n".encode("ascii"))
        self.assertTrue(requests[1].startswith(f"SIGN\t{username}\t".encode("ascii")))

    def test_name_rejection_does_not_generate_or_write_credentials(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)

        def reject_name():
            conn, _ = listener.accept()
            with conn:
                conn.recv(4096)
                conn.sendall(b"TAKEN\n")
            listener.close()

        thread = threading.Thread(target=reject_name)
        thread.start()
        with tempfile.TemporaryDirectory() as directory:
            public_path = Path(directory) / "public.store"
            private_path = Path(directory) / "private.store"
            with patch.object(tribesnext, "generate_account_key") as generate:
                with self.assertRaisesRegex(ValueError, "already taken"):
                    tribesnext.create_account(
                        "TakenName", "account-create-password", public_path, private_path,
                        endpoint=listener.getsockname(), timeout=2)
                generate.assert_not_called()
            self.assertFalse(public_path.exists())
            self.assertFalse(private_path.exists())
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())


if __name__ == "__main__":
    unittest.main()
