import unittest
import hashlib
import socket
import tempfile
from pathlib import Path
import threading

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


if __name__ == "__main__":
    unittest.main()
