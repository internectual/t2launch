"""TribesNext credential recovery and T2CSRI game-server authentication."""

from __future__ import annotations

import argparse
import getpass
import hashlib
import http.client
import ipaddress
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from woncrypto import _rc4_drop2048, verify_private_store_password

MAX_CERT_BYTES = 20_000
MAX_RSA_HEX_CHARS = 2048
AUTH_LOOKUP_URL = "http://www.tribesnext.com/auth"
ROBOT_BASE_URL = "http://tribesnext.thyth.com/tn/robot/"
ROBOT_SESSION_REFRESH_SECONDS = 10 * 60
ROBOT_HTTP_MAX_BYTES = 2_000_000
_HEX = re.compile(r"^[0-9a-fA-F]+$")


@dataclass(frozen=True)
class AccountCertificate:
    username: str
    guid: int
    exponent: int
    modulus: int
    signature: int
    wire_text: str


@dataclass(frozen=True)
class AccountCredentials:
    certificate: AccountCertificate
    private_exponent: int


class RobotAPIError(ValueError):
    """A TribesNext robot endpoint returned an unusable response."""


def _robot_decimal(value, label):
    if not value.isdecimal():
        raise RobotAPIError(f"Malformed TribesNext {label}")
    return value


def _robot_unescape(value):
    """Decode the robot API's backslash, tab, and newline escapes."""
    output = []
    index = 0
    while index < len(value):
        if value[index] == "\\" and index + 1 < len(value):
            escaped = value[index + 1]
            output.append({"\\": "\\", "t": "\t", "n": "\n"}.get(escaped, "\\" + escaped))
            index += 2
        else:
            output.append(value[index])
            index += 1
    return "".join(output)


def parse_robot_profile(lines):
    """Parse the TribesNext robot ``uview`` response into profile fields."""
    profile = {"memberships": []}
    for line in lines:
        fields = line.split("\t")
        if len(fields) >= 3 and fields[0] == "ERR":
            raise RobotAPIError(" ".join(fields[2:]) or "TribesNext profile lookup failed")
        if len(fields) < 3 or fields[0] != "PLAYER":
            continue
        kind = fields[1]
        if kind == "CLAN" and len(fields) >= 10:
            profile["memberships"].append({
                "id": _robot_decimal(fields[4], "clan ID"), "name": fields[5],
                "rank": _robot_decimal(fields[6], "clan rank"),
                "title": _robot_unescape(fields[7]), "tag": fields[8], "append": fields[9],
            })
        elif kind in {"NAME", "TAG", "DATE", "SITE", "INFO", "ONLINE"} and len(fields) >= 4:
            key = {"NAME": "name", "TAG": "tag", "DATE": "creation", "SITE": "website",
                   "INFO": "info", "ONLINE": "online"}[kind]
            profile[key] = _robot_unescape(fields[3])
            if kind == "TAG" and len(fields) > 4:
                profile["append"] = fields[4]
    if not any(key in profile for key in ("name", "website", "info", "creation")):
        raise RobotAPIError("TribesNext robot returned no usable profile")
    return profile


def parse_robot_search(lines, record_type):
    """Parse ``usearch`` or ``csearch`` rows without accepting malformed records."""
    matches = []
    expected = "SEARCH" if record_type == "user" else "CSEARCH"
    for line in lines:
        fields = line.split("\t")
        if len(fields) >= 3 and fields[0] == "ERR":
            raise RobotAPIError(" ".join(fields[2:]) or "TribesNext search failed")
        if fields[0] != expected:
            continue
        if record_type == "user" and len(fields) >= 7:
            matches.append({"index": fields[2], "name": fields[3], "tag": fields[4],
                            "append": fields[5],
                            "guid": _robot_decimal(fields[6], "player GUID")})
        elif record_type == "clan" and len(fields) >= 5:
            matches.append({"index": fields[2],
                            "id": _robot_decimal(fields[3], "clan ID"), "name": fields[4]})
    return matches


def parse_robot_clan(lines):
    """Parse a TribesNext robot ``cview`` response including its roster."""
    clan = {"members": []}
    for line in lines:
        fields = line.split("\t")
        if len(fields) >= 3 and fields[0] == "ERR":
            raise RobotAPIError(" ".join(fields[2:]) or "TribesNext clan lookup failed")
        if len(fields) < 3 or fields[0] != "CLAN":
            continue
        kind = fields[1]
        if kind == "MEMB" and len(fields) >= 11:
            clan["members"].append({
                "name": fields[4], "tag": fields[5], "append": fields[6],
                "guid": _robot_decimal(fields[7], "player GUID"),
                "rank": _robot_decimal(fields[8], "clan rank"),
                "title": _robot_unescape(fields[9]), "online": fields[10],
            })
        elif kind in {"NAME", "TAG", "RECR", "SITE", "DATE", "PICT", "ACTIVE", "INFO"} and len(fields) >= 4:
            key = {"NAME": "name", "TAG": "tag", "RECR": "recruiting", "SITE": "website",
                   "DATE": "creation", "PICT": "picture", "ACTIVE": "active",
                   "INFO": "info"}[kind]
            clan[key] = _robot_unescape(fields[3])
            if kind == "TAG" and len(fields) > 4:
                clan["append"] = fields[4]
    if not clan.get("name"):
        raise RobotAPIError("TribesNext robot returned no usable clan")
    return clan


class RobotClient:
    """Backend-side client for the existing TribesNext robot HTTP interface.

    This runs in wonemu, not inside Tribes 2. Account keys come from the saved
    public/private stores; the API session is a short-lived UUID and is never
    written to disk.
    """

    def __init__(self, base_url: str = ROBOT_BASE_URL, timeout: float = 5.0):
        self.base = urllib.parse.urlsplit(base_url.rstrip("/") + "/")
        if self.base.scheme not in {"http", "https"} or not self.base.hostname:
            raise ValueError("Invalid TribesNext robot base URL")
        self.timeout = float(timeout)
        self._sessions = {}
        self._lock = threading.RLock()

    def _request(self, endpoint, params=None, body=None, content_type=None):
        params = params or {}
        path = self.base.path.rstrip("/") + "/" + endpoint
        if params:
            path += "?" + urllib.parse.urlencode(params)
        connection_type = (http.client.HTTPSConnection if self.base.scheme == "https"
                           else http.client.HTTPConnection)
        connection = connection_type(self.base.hostname, self.base.port, timeout=self.timeout)
        headers = {"User-Agent": "Tribes 2", "Connection": "close"}
        method = "GET"
        if body is not None:
            method = "POST"
            headers["Content-Type"] = content_type or "application/octet-stream"
            headers["Content-Length"] = str(len(body))
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            if response.status != 200:
                raise RobotAPIError(f"TribesNext robot endpoint returned HTTP {response.status}")
            payload = response.read(ROBOT_HTTP_MAX_BYTES + 1)
            if len(payload) > ROBOT_HTTP_MAX_BYTES:
                raise RobotAPIError("TribesNext robot response exceeds size limit")
            try:
                return payload.decode("utf-8", "strict").splitlines()
            except UnicodeDecodeError:
                return payload.decode("latin-1").splitlines()
        except (OSError, http.client.HTTPException) as exc:
            raise RobotAPIError("Unable to reach TribesNext robot service") from exc
        finally:
            connection.close()

    @staticmethod
    def _line_value(lines, prefix):
        for line in lines:
            if line.startswith(prefix):
                return line[len(prefix):].strip()
            if line.startswith("ERR:"):
                raise RobotAPIError(line[4:].strip() or "TribesNext robot login failed")
        return ""

    @staticmethod
    def _new_nonce(credentials):
        modulus_chars = len(credentials.certificate.wire_text.split("\t")[3])
        nonce_chars = modulus_chars // 2
        if nonce_chars < 1 or nonce_chars > 512:
            raise RobotAPIError("Invalid TribesNext account modulus length")
        return "1" + secrets.token_hex((nonce_chars + 1) // 2)[:nonce_chars - 1]

    def _start_session(self, credentials):
        certificate = credentials.certificate
        nonce = self._new_nonce(credentials)
        challenge_lines = self._request("robot_login.php", {
            "guid": str(certificate.guid), "nonce": nonce,
        })
        encrypted_challenge = self._line_value(challenge_lines, "CHALLENGE: ")
        if not encrypted_challenge or not _HEX.fullmatch(encrypted_challenge):
            raise RobotAPIError("Malformed TribesNext robot challenge")
        encrypted = int(encrypted_challenge, 16)
        if encrypted >= certificate.modulus:
            raise RobotAPIError("TribesNext robot challenge exceeds account modulus")
        plaintext = format(pow(encrypted, credentials.private_exponent,
                               certificate.modulus), "x")
        if not plaintext.lower().startswith(nonce.lower()):
            raise RobotAPIError("TribesNext robot challenge did not echo its nonce")
        response = plaintext[len(nonce):]
        if not response:
            raise RobotAPIError("TribesNext robot challenge response is empty")
        uuid_lines = self._request("robot_login.php", {
            "guid": str(certificate.guid), "response": response,
        })
        uuid = self._line_value(uuid_lines, "UUID: ")
        if not uuid:
            raise RobotAPIError("TribesNext robot did not return a session UUID")
        session = {"uuid": uuid, "refreshed_at": time.monotonic()}
        self._sessions[certificate.guid] = session
        return session

    def _ensure_session(self, credentials):
        certificate = credentials.certificate
        session = self._sessions.get(certificate.guid)
        if session is None:
            return self._start_session(credentials)
        if time.monotonic() - session["refreshed_at"] < ROBOT_SESSION_REFRESH_SECONDS:
            return session
        lines = self._request("robot_login.php", {
            "guid": str(certificate.guid), "uuid": session["uuid"],
        })
        if any(line.startswith("REFRESHED") for line in lines):
            session["refreshed_at"] = time.monotonic()
            return session
        self._sessions.pop(certificate.guid, None)
        return self._start_session(credentials)

    def browser_get(self, credentials, method, **args):
        with self._lock:
            session = self._ensure_session(credentials)
            params = {"guid": str(credentials.certificate.guid), "uuid": session["uuid"],
                      "method": str(method)}
            params.update({key: str(value) for key, value in args.items() if value is not None})
            return self._request("robot_browser.php", params)

    def browser_post(self, credentials, method, **fields):
        with self._lock:
            session = self._ensure_session(credentials)
            boundary = "---------------------------" + secrets.token_hex(12)
            values = {"guid": str(credentials.certificate.guid), "uuid": session["uuid"],
                      "method": str(method)}
            values.update({key: str(value) for key, value in fields.items() if value is not None})
            chunks = []
            for key, value in values.items():
                chunks.append((f"--{boundary}\r\n"
                               f"Content-Disposition: form-data; name=\"{key}\"\r\n\r\n"
                               f"{value}\r\n").encode())
            chunks.append(f"--{boundary}--\r\n".encode("ascii"))
            return self._request(
                "robot_browser.php", body=b"".join(chunks),
                content_type=f"multipart/form-data; boundary={boundary}")

    def user_view(self, credentials, guid):
        lines = self.browser_get(credentials, "uview", a0=guid)
        return parse_robot_profile(lines)

    def clan_view(self, credentials, clan_id):
        """Return clan details and roster from the official robot API."""
        return parse_robot_clan(self.browser_get(credentials, "cview", a0=clan_id))

    def user_search(self, credentials, name):
        """Return player results from the official name-prefix search."""
        return parse_robot_search(self.browser_get(credentials, "usearch", a0=name), "user")

    def clan_search(self, credentials, name):
        """Return clan results from the official name-prefix search."""
        return parse_robot_search(self.browser_get(credentials, "csearch", a0=name), "clan")


def _hex_integer(value: str, label: str) -> int:
    if not value or len(value) > MAX_RSA_HEX_CHARS or not _HEX.fullmatch(value):
        raise ValueError(f"Invalid certificate {label}")
    return int(value, 16)


def parse_certificate(value: str | bytes) -> AccountCertificate:
    """Parse a five-field certificate without rewriting its wire representation."""
    if isinstance(value, bytes):
        try:
            value = value.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError("Certificate is not ASCII") from exc
    if len(value.encode("ascii", "strict")) > MAX_CERT_BYTES:
        raise ValueError("Certificate exceeds TribesNext size limit")
    fields = value.strip("\r\n").split("\t")
    if len(fields) != 5:
        raise ValueError("Certificate must contain five tab-separated fields")
    username, guid_text, exponent_text, modulus_text, signature_text = fields
    if not username or any(ord(char) < 32 or ord(char) > 125 for char in username):
        raise ValueError("Invalid certificate username")
    if not guid_text.isascii() or not guid_text.isdigit():
        raise ValueError("Invalid certificate GUID")
    cert = AccountCertificate(
        username=username,
        guid=int(guid_text),
        exponent=_hex_integer(exponent_text, "exponent"),
        modulus=_hex_integer(modulus_text, "modulus"),
        signature=_hex_integer(signature_text, "signature"),
        wire_text="\t".join(fields),
    )
    if cert.exponent < 3 or cert.modulus <= 0 or cert.signature <= 0:
        raise ValueError("Certificate RSA values are out of range")
    return cert


def unlock_account(public_store, private_store, username: str, password: str) -> AccountCredentials:
    """Unlock an installed account key; trust the certificate issuer to the game server."""
    public_path, private_path = Path(public_store), Path(private_store)
    if not public_path.is_file() or not private_path.is_file():
        raise ValueError("TribesNext account stores are missing")
    certificate_text = _find_store_line(public_path, username)
    encrypted = _find_store_value(private_path, username)
    certificate = parse_certificate(certificate_text)
    status = verify_private_store_password(private_path, certificate.username, password)
    if status != "valid":
        raise ValueError(f"Unable to unlock existing TribesNext account ({status})")
    private_text = _decode_store_value(encrypted)
    if private_text.count(":") != 1:
        raise ValueError("Malformed TribesNext private-key entry")
    nonce, private_hex = private_text.split(":", 1)
    key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
    private_exponent = int(_rc4_drop2048(key, bytes.fromhex(private_hex)).hex(), 16)
    if private_exponent <= 0:
        raise ValueError("Unlocked TribesNext private exponent is empty")
    probe = 2
    if pow(pow(probe, certificate.exponent, certificate.modulus),
           private_exponent, certificate.modulus) != probe:
        raise ValueError("Unlocked TribesNext key does not match its certificate")
    return AccountCredentials(certificate, private_exponent)


def _find_store_value(path: Path, username: str, full_line: bool = False) -> str:
    for line in path.read_text(encoding="ascii").splitlines():
        fields = line.split("\t", 1)
        if len(fields) == 2 and fields[0].casefold() == username.casefold():
            return line.strip() if full_line else fields[1].strip()
    raise ValueError(f"No TribesNext store entry for account {username}")


def _find_store_line(path: Path, username: str) -> str:
    return _find_store_value(path, username, full_line=True)


def _decode_store_value(value: str) -> str:
    if ":" in value:
        return value
    import base64
    try:
        return base64.b64decode(value, validate=True).decode("ascii")
    except (ValueError, UnicodeError) as exc:
        raise ValueError("Malformed encoded TribesNext store entry") from exc


def ipv4_hex(address: str) -> str:
    return ipaddress.IPv4Address(address).packed.hex()


def make_client_challenge(server_ip: str, random64: int) -> str:
    """Match Ruby rand(...).to_s(16): unpadded 64-bit nonce plus IPv4 hex."""
    if not 0 <= random64 < 1 << 64:
        raise ValueError("Client challenge nonce must be a 64-bit unsigned integer")
    return f"{random64:x}{ipv4_hex(server_ip)}"


def decrypt_server_challenge(credentials: AccountCredentials, encrypted_hex: str,
                             client_challenge: str) -> str:
    """Decrypt challenge, verify the echoed client nonce, and return server half."""
    encrypted = _hex_integer(encrypted_hex, "server challenge")
    cert = credentials.certificate
    if encrypted >= cert.modulus:
        raise ValueError("Encrypted server challenge exceeds account modulus")
    plaintext = format(pow(encrypted, credentials.private_exponent, cert.modulus), "x")
    if not plaintext.lower().startswith(client_challenge.lower()):
        raise ValueError("Server challenge did not echo the client nonce")
    server_half = plaintext[len(client_challenge):]
    if not server_half or not _HEX.fullmatch(server_half):
        raise ValueError("Malformed server challenge")
    return server_half


def recovery_request(username: str, password: str, utc_seconds: int | None = None) -> bytes:
    """Build the password-derived RECOVER request used by T2CSRI and t2-mapper."""
    if not username or any(char in username for char in "\t\r\n"):
        raise ValueError("Invalid TribesNext username")
    if any(char in password for char in "\r\n"):
        raise ValueError("Invalid TribesNext password")
    utc = str(int(time.time()) if utc_seconds is None else int(utc_seconds))
    lower_name = username.lower()
    auth_stored = hashlib.sha1(("3.14159265" + lower_name + password).encode("utf-8")).hexdigest()
    time_nonce = hashlib.sha1((utc + lower_name).encode("ascii")).hexdigest()
    request_hash = hashlib.sha1((auth_stored + time_nonce).encode("ascii")).hexdigest()
    return f"RECOVER\t{username}\t{utc}\t{request_hash}\n".encode("ascii")


def discover_auth_server(timeout: float = 10.0) -> tuple[str, int]:
    """Resolve the official TribesNext auth endpoint through its HTTP lookup."""
    request = urllib.request.Request(AUTH_LOOKUP_URL, headers={"User-Agent": "Tribes 2"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = response.read(65536).decode("ascii", "strict")
    for line in body.splitlines():
        address = line.strip().split("\t", 1)[0]
        if ":" not in address:
            continue
        host, port_text = address.rsplit(":", 1)
        if host and port_text.isdigit() and 0 < int(port_text) <= 65535:
            return host, int(port_text)
    raise ValueError("Official TribesNext auth lookup returned no endpoint")


def parse_recovery_response(response: bytes, expected_username: str) -> tuple[str, str]:
    """Parse official CERT/EXP response and return the certificate and encrypted key."""
    try:
        text = response.decode("ascii").strip()
    except UnicodeDecodeError as exc:
        raise ValueError("Auth server response is not ASCII") from exc
    if text.upper() in {"NOTFOUND", "INVALIDPASSWORD", "RECOVERERROR"}:
        raise ValueError(text.upper())
    lines = text.splitlines()
    if len(lines) < 2 or not lines[0].startswith("CERT:") or not lines[1].startswith("EXP:"):
        raise ValueError("Malformed TribesNext account recovery response")
    certificate_text = lines[0][5:].strip()
    encrypted = lines[1][4:].strip()
    certificate = parse_certificate(certificate_text)
    if certificate.username.casefold() != expected_username.casefold():
        raise ValueError("Recovered certificate account does not match request")
    if not encrypted or len(encrypted) > MAX_CERT_BYTES or any(c in encrypted for c in "\r\n\t"):
        raise ValueError("Malformed encrypted account key")
    return certificate.wire_text, encrypted


def _auth_request(endpoint: tuple[str, int], payload: bytes, timeout: float) -> bytes:
    received = bytearray()
    with socket.create_connection(endpoint, timeout=timeout) as conn:
        conn.settimeout(timeout)
        conn.sendall(payload)
        conn.settimeout(min(timeout, 0.7))
        while len(received) < MAX_CERT_BYTES * 2:
            try:
                data = conn.recv(4096)
            except TimeoutError:
                break
            if not data:
                break
            received.extend(data)
    return bytes(received)


def _validate_new_account_fields(username: str, password: str) -> None:
    if (not username.isascii() or not 4 <= len(username) <= 16 or
            any(ord(char) < 33 or ord(char) > 126 or char in "\t\r\n" for char in username)):
        raise ValueError("Account names must be 4-16 printable ASCII characters")
    if (not password.isascii() or not 6 <= len(password) <= 255 or
            any(ord(char) < 32 or ord(char) > 126 for char in password)):
        raise ValueError("Passwords must be 6-255 printable ASCII characters")


def generate_account_key(rsa_helper=None, bits: int = 1024,
                         timeout: float = 180.0) -> tuple[int, int, int]:
    """Create a supported TribesNext RSA key using the GMP-backed helper."""
    if bits not in {512, 768, 1024}:
        raise ValueError("TribesNext account keys must be 512, 768, or 1024 bits")
    helper = Path(rsa_helper) if rsa_helper else (
        Path(__file__).resolve().parent / "games" / "tribes2" / "rsa_helper")
    if not helper.is_file():
        raise FileNotFoundError(
            f"RSA helper not found at {helper}; build the Tribes 2 support overlay first")
    result = subprocess.run([str(helper), "--keygen", str(bits)],
                            capture_output=True, text=True, timeout=timeout, check=False)
    if result.returncode != 0:
        raise RuntimeError("RSA key generation failed")
    fields = result.stdout.strip().split("\t")
    if len(fields) != 3 or any(not _HEX.fullmatch(field) for field in fields):
        raise RuntimeError("RSA helper returned malformed key material")
    modulus, exponent, private_exponent = (int(field, 16) for field in fields)
    if (modulus.bit_length() != bits or exponent not in {3, 5, 7} or
            private_exponent <= 0 or
            pow(pow(2, exponent, modulus), private_exponent, modulus) != 2):
        raise RuntimeError("RSA helper returned an invalid key")
    return exponent, modulus, private_exponent


def _private_store_value(private_exponent_hex: str, password: str) -> str:
    nonce = hashlib.sha1(private_exponent_hex.encode("ascii")).hexdigest()
    key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
    encrypted = _rc4_drop2048(key, bytes.fromhex(private_exponent_hex)).hex()
    return f"{nonce}:{encrypted}"


def _account_name_in_store(path: Path, username: str) -> bool:
    if not path.is_file():
        return False
    return any(line.split("\t", 1)[0].casefold() == username.casefold()
               for line in path.read_text(encoding="ascii").splitlines())


def create_account(username: str, password: str, public_store, private_store,
                   rsa_helper=None, key_bits: int = 1024,
                   endpoint: tuple[str, int] | None = None,
                   timeout: float = 15.0) -> AccountCredentials:
    """Register a new account through the official T2CSRI SIGN protocol.

    The password is used only to derive the server's password hash and encrypt
    the local private exponent. It is never sent to the auth server or logged.
    """
    _validate_new_account_fields(username, password)
    public_path, private_path = Path(public_store), Path(private_store)
    if _account_name_in_store(public_path, username) or _account_name_in_store(private_path, username):
        raise ValueError("An account with that name is already installed locally")
    endpoint = endpoint or discover_auth_server(timeout=min(timeout, 10.0))

    availability = _auth_request(endpoint, f"NAME\t{username}\n".encode("ascii"), timeout)
    try:
        name_status = availability.decode("ascii").strip()
    except UnicodeDecodeError as exc:
        raise ValueError("Authentication server returned a non-ASCII name status") from exc
    if name_status != "SUCCESS":
        errors = {"TOOSHORT": "Account name is too short",
                  "TOOLONG": "Account name is too long",
                  "INVALID": "Account name is not accepted",
                  "TAKEN": "Account name is already taken"}
        raise ValueError(errors.get(name_status, "Authentication server name check failed"))

    exponent, modulus, private_exponent = generate_account_key(rsa_helper, key_bits)
    exponent_hex = format(exponent, "x")
    modulus_hex = format(modulus, "x")
    private_hex = format(private_exponent, "x")
    if len(private_hex) % 2:
        private_hex = "0" + private_hex
    encrypted_private = _private_store_value(private_hex, password)
    auth_hash = hashlib.sha1(
        ("3.14159265" + username.lower() + password).encode("ascii")).hexdigest()
    signed_fields = (f"{username}\t{exponent_hex}\t{modulus_hex}\t"
                     f"{encrypted_private}\t{auth_hash}")
    request_signature = format(
        pow(int(hashlib.sha1(signed_fields.encode("ascii")).hexdigest(), 16),
            private_exponent, modulus), "x")
    response = _auth_request(
        endpoint, f"SIGN\t{signed_fields}\t{request_signature}\n".encode("ascii"), timeout)
    try:
        certificate = parse_certificate(response)
    except (UnicodeError, ValueError) as exc:
        status = response.decode("ascii", "replace").strip().upper()
        errors = {"REJECTED": "Authentication server rejected this registration",
                  "INVALIDNAME": "Authentication server rejected the account name",
                  "SIGNERROR": "Authentication server rejected the signed request"}
        raise ValueError(errors.get(status, "Malformed account registration response")) from exc
    if (certificate.username.casefold() != username.casefold() or
            certificate.exponent != exponent or certificate.modulus != modulus):
        raise ValueError("Authentication server returned a certificate for a different key")
    credentials = AccountCredentials(certificate, private_exponent)

    with tempfile.TemporaryDirectory(prefix="t2launch-tn-create-") as temp_dir:
        temp_public, temp_private = Path(temp_dir) / "public.store", Path(temp_dir) / "private.store"
        _store_account(temp_public, username, certificate.wire_text, keyed=False)
        _store_account(temp_private, username, encrypted_private, keyed=True)
        unlock_account(temp_public, temp_private, username, password)
    _store_account(public_path, username, certificate.wire_text, keyed=False)
    _store_account(private_path, username, encrypted_private, keyed=True)
    return credentials


def recover_account(username: str, password: str, public_store, private_store,
                    endpoint: tuple[str, int] | None = None,
                    timeout: float = 15.0) -> AccountCredentials:
    """Recover an existing account and persist stores; never creates accounts."""
    endpoint = endpoint or discover_auth_server(timeout=min(timeout, 10.0))
    request = recovery_request(username, password)
    received = bytearray()
    with socket.create_connection(endpoint, timeout=timeout) as conn:
        conn.settimeout(0.7)
        conn.sendall(request)
        while len(received) < MAX_CERT_BYTES * 2:
            try:
                data = conn.recv(4096)
            except TimeoutError:
                break
            if not data:
                break
            received.extend(data)
    certificate, encrypted = parse_recovery_response(bytes(received), username)
    with tempfile.TemporaryDirectory(prefix="t2launch-tn-") as temporary_dir:
        temporary_public = Path(temporary_dir) / "public.store"
        temporary_private = Path(temporary_dir) / "private.store"
        _store_account(temporary_public, username, certificate, keyed=False)
        _store_account(temporary_private, username, encrypted, keyed=True)
        credentials = unlock_account(temporary_public, temporary_private, username, password)
    _store_account(Path(public_store), username, certificate, keyed=False)
    _store_account(Path(private_store), username, encrypted, keyed=True)
    return credentials


def _store_account(path: Path, username: str, value: str, keyed: bool) -> None:
    """Atomically update one store entry with owner-only permissions."""
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = path.read_text(encoding="ascii").splitlines() if path.is_file() else []
    lines = [line for line in lines
             if line.split("\t", 1)[0].casefold() != username.casefold()]
    lines.append(f"{username}\t{value}" if keyed else value)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text("\n".join(lines) + "\n", encoding="ascii")
    temporary.chmod(0o600)
    temporary.replace(path)


def _main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="TribesNext account utilities")
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create-account", help="register a new TribesNext account")
    create.add_argument("username")
    create.add_argument("--game-dir", type=Path,
                        default=Path.home() / "t2-linux",
                        help="game directory containing base/ (default: ~/t2-linux)")
    create.add_argument("--public-store", type=Path)
    create.add_argument("--private-store", type=Path)
    create.add_argument("--rsa-helper", type=Path,
                        help="GMP-backed rsa_helper generated by docker/asgard-build")
    create.add_argument("--key-bits", type=int, choices=(512, 768, 1024), default=1024)
    args = parser.parse_args(argv)
    password = getpass.getpass("New TribesNext password: ")
    confirmation = getpass.getpass("Confirm password: ")
    if password != confirmation:
        parser.error("Passwords do not match")
    public_store = args.public_store or args.game_dir / "base" / "public.store"
    private_store = args.private_store or args.game_dir / "base" / "private.store"
    try:
        credentials = create_account(
            args.username, password, public_store, private_store,
            rsa_helper=args.rsa_helper, key_bits=args.key_bits)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Account creation failed: {exc}", file=sys.stderr)
        return 1
    print(f"Created TribesNext account {credentials.certificate.username} "
          f"(GUID {credentials.certificate.guid}).")
    print(f"Credentials saved to {public_store} and {private_store}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
