"""TribesNext credential recovery and T2CSRI game-server authentication."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import ipaddress
from pathlib import Path
import re
import socket
import tempfile
import time
import urllib.request

from woncrypto import _rc4_drop2048, verify_private_store_password

MAX_CERT_BYTES = 20_000
MAX_RSA_HEX_CHARS = 2048
AUTH_LOOKUP_URL = "http://www.tribesnext.com/auth"
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
            except socket.timeout:
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
