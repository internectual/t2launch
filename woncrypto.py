"""WON public-key encoding and NR/MD5 signatures recovered from Loki #25034.

Public keys are DER SEQUENCE(p, q, g, y). Key-block signatures verify as
r == (g**s * y**r mod p + MD5(message)) mod q, serialized r || s big-endian.
"""
import base64
import ctypes
import ctypes.util
import hashlib
import json
import os
import secrets
import struct
import time
from pathlib import Path

# The embedded WON verifier slot in the client (tribes2.dynamic) and the
# dedicated server (tribes2d.dynamic); both hold the same key.
ROOT_ADDRESSES = (0x0849A860, 0x0848F520)
ROOT_SLOT_SIZE = 817
LOCAL_AUTH_PARAMETERS = {
    # A local compatibility group avoids the Loki client's oversized c2
    # intermediate being truncated by its fixed-width 2048-bit serializer.
    "p": 280113052720621736320677874810934783027,
    "q": 140056526360310868160338937405467391513,
    "g": 4,
}


def der_length(length):
    if length < 128:
        return bytes([length])
    data = length.to_bytes((length.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(data)]) + data


def der_integer(value):
    data = value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big")
    if data[0] & 0x80:
        data = b"\0" + data
    return b"\x02" + der_length(len(data)) + data


def public_key(key):
    data = b"".join(der_integer(key[name]) for name in ("p", "q", "g", "y"))
    return b"\x30" + der_length(len(data)) + data


def private_key(key):
    data = b"".join(der_integer(key[name]) for name in ("p", "q", "g", "y", "x"))
    return b"\x30" + der_length(len(data)) + data


def private_key_components(key):
    """Encode the native Auth1 private-key BigInteger sequence."""
    def component(value):
        raw = value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big")
        if len(raw) < 0x80:
            length = bytes([len(raw)])
        else:
            encoded_length = len(raw).to_bytes((len(raw).bit_length() + 7) // 8, "big")
            length = bytes([0x80 | len(encoded_length)]) + encoded_length
        return b"\x02" + length + raw

    return b"\0" + b"".join(component(key[name]) for name in ("p", "q", "g", "y", "x"))


def encode_won_token(data):
    """Encode binary IRC auth data using the client's ASCII-shifted Base64."""
    alphabet = bytes(range(0x30, 0x70))
    data = bytes(data)
    encoded = bytearray()
    full_length = len(data) - (len(data) % 3)
    for offset in range(0, full_length, 3):
        first, second, third = data[offset:offset + 3]
        encoded.extend((alphabet[first >> 2],
                        alphabet[((first & 0x03) << 4) | (second >> 4)],
                        alphabet[((second & 0x0f) << 2) | (third >> 6)],
                        alphabet[third & 0x3f]))
    remaining = data[full_length:]
    if len(remaining) == 1:
        first = remaining[0]
        encoded.extend((alphabet[first >> 2], alphabet[(first & 0x03) << 4]))
    elif len(remaining) == 2:
        first, second = remaining
        encoded.extend((alphabet[first >> 2],
                        alphabet[((first & 0x03) << 4) | (second >> 4)],
                        alphabet[(second & 0x0f) << 2]))
    return encoded.decode("ascii")


def decode_won_token(token):
    """Decode the client's unpadded, ASCII-shifted Base64 IRC token."""
    try:
        encoded = token.encode("ascii") if isinstance(token, str) else bytes(token)
    except (UnicodeEncodeError, TypeError, ValueError) as exc:
        raise ValueError("Invalid WON token") from exc
    if any(value < 0x30 or value > 0x6f for value in encoded) or len(encoded) % 4 == 1:
        raise ValueError("Invalid WON token")

    values = [value - 0x30 for value in encoded]
    decoded = bytearray()
    full_length = len(values) - (len(values) % 4)
    for offset in range(0, full_length, 4):
        first, second, third, fourth = values[offset:offset + 4]
        decoded.extend((((first << 2) | (second >> 4)) & 0xff,
                        ((second << 4) | (third >> 2)) & 0xff,
                        ((third << 6) | fourth) & 0xff))
    remaining = values[full_length:]
    if len(remaining) >= 2:
        decoded.append(((remaining[0] << 2) | (remaining[1] >> 4)) & 0xff)
    if len(remaining) == 3:
        decoded.append(((remaining[1] << 4) | (remaining[2] >> 2)) & 0xff)
    return bytes(decoded)


def parse_won_irc_token(token):
    """Decode a native IRC auth token and expose its type and opaque payload."""
    message = decode_won_token(token)
    if len(message) < 8:
        raise ValueError("Truncated WON IRC auth message")
    message_type, subtype = struct.unpack_from("<II", message)
    if message_type != 0xcb:
        raise ValueError("Unexpected WON IRC auth message type")
    return {"subtype": subtype, "payload": message[8:]}


def parse_won_irc_challenge(token):
    """Parse the encrypted challenge section and opaque context section in CB33."""
    message = parse_won_irc_token(token)
    if message["subtype"] != 0x33:
        raise ValueError("Expected a WON IRC CB33 challenge")

    payload = message["payload"]
    sections = []
    offset = 0
    for _ in range(2):
        if offset + 2 > len(payload):
            raise ValueError("Truncated WON IRC challenge section length")
        length, = struct.unpack_from("<H", payload, offset)
        offset += 2
        if offset + length > len(payload):
            raise ValueError("Truncated WON IRC challenge section")
        sections.append(payload[offset:offset + length])
        offset += length
    if offset != len(payload):
        raise ValueError("Trailing data in WON IRC challenge")

    if len(sections[0]) < 4:
        raise ValueError("Truncated WON IRC ElGamal challenge section")
    return {"encrypted_challenge": sections[0], "context": sections[1]}


def decrypt_won_irc_challenge(token, key):
    """Decrypt a CB33 ElGamal section and return its challenge and context."""
    sections = parse_won_irc_challenge(token)
    plaintext = decrypt_elgamal_challenge_chunks(key, sections["encrypted_challenge"])
    if len(plaintext) < 2:
        raise ValueError("Truncated WON IRC decrypted challenge")
    challenge_length, = struct.unpack_from("<H", plaintext)
    if challenge_length > len(plaintext) - 2:
        raise ValueError("Invalid WON IRC decrypted challenge length")
    return {"challenge": plaintext[2:2 + challenge_length],
            "challenge_extra": plaintext[2 + challenge_length:],
            "context": sections["context"]}


def decrypt_elgamal_challenge_chunks(key, ciphertext):
    """Decrypt ElGamal chunks that carry a trailing one-byte chunk length."""
    decrypted = decrypt_elgamal(key, ciphertext)
    width = (key["p"].bit_length() + 7) // 8
    plaintext = bytearray()
    for offset in range(0, len(decrypted), width):
        block = decrypted[offset:offset + width]
        chunk_length = block[-1]
        if not chunk_length or chunk_length > min(width - 3, 0xff):
            raise ValueError("Invalid WON IRC ElGamal chunk length")
        plaintext.extend(block[-1 - chunk_length:-1])
    return bytes(plaintext)


def decode_won_irc_cb34_proof(token, key):
    """Decrypt the opaque proof carried by a native CB34 token."""
    message = parse_won_irc_token(token)
    if message["subtype"] != 0x34:
        raise ValueError("Expected a WON IRC CB34 response")
    payload = message["payload"]
    if len(payload) < 2:
        raise ValueError("Truncated WON IRC CB34 response")
    cipher_length, = struct.unpack_from("<H", payload)
    if cipher_length != len(payload) - 2:
        raise ValueError("Invalid WON IRC CB34 ciphertext length")
    return decrypt_elgamal_challenge_chunks(key, payload[2:])


def verify_won_irc_cb34(token, client_key, session_key):
    """Validate the native CB34 proof against its Auth1 client session key."""
    plaintext = decode_won_irc_cb34_proof(token, client_key)
    session_key = bytes(session_key)
    expected = struct.pack("<H", len(session_key)) + session_key + session_key
    return plaintext == expected


def extract_won_irc_cb34_response(token, key, expected_challenge):
    """Extract the client-generated CB34 response secret when challenge-bound."""
    plaintext = decode_won_irc_cb34_proof(token, key)
    if len(plaintext) < 2:
        return None
    challenge_length, = struct.unpack_from("<H", plaintext)
    if challenge_length > len(plaintext) - 2:
        return None
    challenge = plaintext[2:2 + challenge_length]
    response = plaintext[2 + challenge_length:]
    if challenge != bytes(expected_challenge) or not 1 <= len(response) <= 56:
        return None
    return response


def read_der_public(data):
    offset = 0
    def item(tag):
        nonlocal offset
        if offset >= len(data) or data[offset] != tag:
            raise ValueError("Unexpected DER tag")
        offset += 1
        length = data[offset]
        offset += 1
        if length & 0x80:
            count = length & 0x7f
            if not 1 <= count <= 2:
                raise ValueError("Unsupported DER length")
            length = int.from_bytes(data[offset:offset + count], "big")
            offset += count
        if offset + length > len(data):
            raise ValueError("Truncated DER")
        return length
    length = item(0x30)
    end = offset + length
    key = {}
    for name in ("p", "q", "g", "y"):
        length = item(2)
        key[name] = int.from_bytes(data[offset:offset + length], "big")
        offset += length
    if offset != end:
        raise ValueError("Unexpected public-key fields")
    return key


def verifier_parameters(binary):
    data = Path(binary).read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected a Loki ELF32 little-endian game binary")
    phoff, = struct.unpack_from("<I", data, 28)
    size, count = struct.unpack_from("<HH", data, 42)
    for root in ROOT_ADDRESSES:
        for index in range(count):
            kind, offset, address, _, length, _, _, _ = struct.unpack_from("<8I", data, phoff + size * index)
            if kind != 1 or not address <= root or root + ROOT_SLOT_SIZE > address + length:
                continue
            start = offset + root - address
            if data[start:start + 4] != b"\x30\x82\x03\x2c":
                continue
            key = read_der_public(data[start:start + ROOT_SLOT_SIZE])
            if key["p"].bit_length() != 2048 or key["q"].bit_length() < 128:
                raise ValueError("Unsupported embedded WON verifier parameters")
            return key
    raise ValueError("Embedded WON verifier not found")


def make_key(parameters):
    key = {name: parameters[name] for name in ("p", "q", "g")}
    key["x"] = secrets.randbelow(key["q"] - 1) + 1
    key["y"] = pow(key["g"], key["x"], key["p"])
    return key


def provision(directory, binary, auth_profile="compat"):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    directory.chmod(0o700)
    path = directory / "keys.json"
    parameters = verifier_parameters(binary)
    if auth_profile == "compat":
        auth_parameters = LOCAL_AUTH_PARAMETERS
    elif auth_profile == "native":
        auth_parameters = parameters
    else:
        raise ValueError(f"Unknown WON auth key profile: {auth_profile}")
    if path.exists():
        path.chmod(0o600)
        stored = json.loads(path.read_text())
        keys = {role: {name: int(value, 16) for name, value in values.items()} for role, values in stored.items()}
        if any(keys["root"][name] != parameters[name] for name in ("p", "q", "g")):
            raise ValueError("Stored root key uses different client parameters")
        if any(keys["auth"][name] != auth_parameters[name] for name in ("p", "q", "g")):
            keys["auth"] = make_key(auth_parameters)
            path.write_text(json.dumps({role: {name: format(value, "x") for name, value in key.items()}
                                        for role, key in keys.items()}, indent=2) + "\n")
            path.chmod(0o600)
    else:
        keys = {"root": make_key(parameters), "auth": make_key(auth_parameters)}
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "w") as stream:
            json.dump({role: {name: format(value, "x") for name, value in key.items()} for role, key in keys.items()}, stream, indent=2)
            stream.write("\n")
    root = public_key(keys["root"])
    if len(root) > ROOT_SLOT_SIZE:
        raise ValueError("Emulator verifier exceeds embedded key storage")
    pub_path = directory / "verifier.der"
    pub_path.write_bytes(root.ljust(ROOT_SLOT_SIZE, b"\0"))
    pub_path.chmod(0o600)
    return keys


def _nr_digest(key, message):
    digest = hashlib.md5(message).digest()
    q_bits = key["q"].bit_length()
    if len(digest) * 8 < q_bits:
        return int.from_bytes(digest, "big")
    q_bytes = (q_bits + 7) // 8
    return int.from_bytes(digest[:q_bytes], "big") >> (q_bytes * 8 - q_bits + 1)


def sign(key, message):
    h = _nr_digest(key, message)
    while True:
        k = secrets.randbelow(key["q"] - 1) + 1
        r = (pow(key["g"], k, key["p"]) + h) % key["q"]
        if r:
            break
    s = (k - key["x"] * r) % key["q"]
    width = (key["q"].bit_length() + 7) // 8
    return r.to_bytes(width, "big") + s.to_bytes(width, "big")


def verify(key, message, signature):
    width = (key["q"].bit_length() + 7) // 8
    if len(signature) != 2 * width:
        return False
    r = int.from_bytes(signature[:width], "big")
    s = int.from_bytes(signature[width:], "big")
    if not 0 < r < key["q"] or not 0 <= s < key["q"]:
        return False
    h = _nr_digest(key, message)
    return (pow(key["g"], s, key["p"]) * pow(key["y"], r, key["p"]) % key["p"] + h) % key["q"] == r


def key_block(keys, now=None):
    now = int(time.time()) if now is None else now
    encoded = public_key(keys["auth"])
    # AuthBase(family, issue, expiry), block id, count, length-prefixed key.
    body = struct.pack("<HIIHHH", 1, now, now + 86400, 1, 1, len(encoded)) + encoded
    return body + sign(keys["root"], body)


# wonemu's own accounts take GUIDs from 1,000,000,000 to 2^31 - 1. TribesNext
# account GUIDs are numbered from 1 and are in the low millions, so the two
# never meet, and GUIDs stay positive for scripts that read them as signed.
# The community store keeps each account's GUID and skips one already taken.
LOCAL_USER_ID_BASE = 1_000_000_000
LOCAL_USER_ID_LIMIT = 0x7fffffff


def local_auth_user_id(username):
    """The GUID a new wonemu account named username asks for first."""
    digest = hashlib.sha256(str(username).casefold().encode("utf-8")).digest()
    return LOCAL_USER_ID_BASE + int.from_bytes(digest[:4], "little") % (
        LOCAL_USER_ID_LIMIT - LOCAL_USER_ID_BASE + 1)


def legacy_local_auth_user_id(username):
    """The name-hash GUID wonemu gave its accounts before they had their own range."""
    digest = hashlib.sha256(str(username).casefold().encode("utf-8")).digest()
    return int.from_bytes(digest[:4], "little") or 1


def private_store_contains(path, username):
    """Check account-name presence without reading or exposing its private key."""
    path = Path(path) if path is not None else None
    if path is None or not path.is_file():
        return False
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError):
        return False
    return any("\t" in line and line.split("\t", 1)[0].casefold() == username.casefold()
               for line in lines)


def private_store_usernames(path):
    """Return account names only; never return encrypted private-store values."""
    path = Path(path) if path is not None else None
    if path is None or not path.is_file():
        return ()
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError):
        return ()
    return tuple(line.split("\t", 1)[0] for line in lines
                 if "\t" in line and line.split("\t", 1)[0])


# WON ClientCDKey, checked offline by the client before account creation
# (Loki #25034: parse 0x836bbf0, check 0x836baac). Each pattern letter encodes
# bits LSB-first into 8 bytes: C a consonant (4 bits), V a vowel (2), N a
# digit 2-9 (3). Byte 3 must equal bits 4-11 of a CRC-16 (poly 0x8005, MSB
# first, initial 0) over the product name, bytes 0-2, and bytes 4-7.
CD_KEY_PATTERN = "CVCNCVCNCVCNCVCNNNNN"
CD_KEY_ALPHABETS = {"C": "BCDFGJLMNPRSTWXZ", "V": "AEUY", "N": "23456789"}
CD_KEY_BITS = {"C": 4, "V": 2, "N": 3}
CD_KEY_SEPARATORS = "- \t"


def _crc16_8005(data, crc=0):
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def _cd_key_check_byte(raw, product):
    crc = _crc16_8005(product.encode("ascii"))
    crc = _crc16_8005(raw[:3] + raw[4:], crc)
    return (crc >> 4) & 0xFF


def _decode_cd_key(key):
    text = "".join(char for char in str(key) if char not in CD_KEY_SEPARATORS).upper()
    if len(text) != len(CD_KEY_PATTERN):
        return None
    value = position = 0
    for char, kind in zip(text, CD_KEY_PATTERN):
        index = CD_KEY_ALPHABETS[kind].find(char)
        if index < 0:
            return None
        value |= index << position
        position += CD_KEY_BITS[kind]
    return value.to_bytes(8, "little")


def cd_key_is_valid(key, product="Tribes2"):
    """Apply the client's offline CD key check; no server ever verifies keys."""
    raw = _decode_cd_key(key)
    return raw is not None and raw[3] == _cd_key_check_byte(raw, product)


def generate_cd_key(product="Tribes2"):
    """Return a random key in the stock dialog's XXXX-XXXX-XXXX-XXXX-XXXX form."""
    raw = bytearray(secrets.token_bytes(8))
    raw[3] = _cd_key_check_byte(bytes(raw), product)
    value = int.from_bytes(raw, "little")
    chars = []
    for kind in CD_KEY_PATTERN:
        bits = CD_KEY_BITS[kind]
        chars.append(CD_KEY_ALPHABETS[kind][value & ((1 << bits) - 1)])
        value >>= bits
    text = "".join(chars)
    return "-".join(text[i:i + 4] for i in range(0, len(text), 4))


# Limits enforced by the client's AuthInfo parser (Loki #25034, 0x8348fcc).
# The parser has no tribe-count check; its stack record holds ten tribes.
T2_AUTH_INFO_MAX_TRIBES = 10
T2_AUTH_INFO_MAX_TAG = 15
T2_AUTH_INFO_MAX_TRIBE_NAME = 63
T2_AUTH_INFO_MAX_TITLE = 40
T2_AUTH_INFO_MAX_WARRIOR = 31


def _auth_info_string(value, limit, label):
    encoded = str(value).encode("latin-1")
    if len(encoded) > limit:
        raise ValueError(f"AuthInfo {label} is too long")
    return struct.pack("<H", len(encoded)) + encoded


def t2_auth_info_blob(warrior, tribes=()):
    """Encode the Tribes 2 community data that WONGetAuthInfo() decodes.

    Each tribe is a mapping with id, name, tag, append, rank and title. The
    client reads a title only for ranks above 9, so lower ranks omit it.
    """
    tribes = list(tribes)
    if len(tribes) > T2_AUTH_INFO_MAX_TRIBES:
        raise ValueError("AuthInfo has too many tribes")
    blob = struct.pack("<B", len(tribes))
    for tribe in tribes:
        rank = int(tribe.get("rank", 0))
        blob += (struct.pack("<I", int(tribe["id"])) +
                 _auth_info_string(tribe.get("tag", ""), T2_AUTH_INFO_MAX_TAG, "tribe tag") +
                 struct.pack("<B", int(bool(int(tribe.get("append", 0))))) +
                 _auth_info_string(tribe["name"], T2_AUTH_INFO_MAX_TRIBE_NAME, "tribe name") +
                 struct.pack("<I", rank))
        if rank > 9:
            blob += _auth_info_string(tribe.get("title", ""), T2_AUTH_INFO_MAX_TITLE,
                                      "tribe title")
    return blob + _auth_info_string(warrior, T2_AUTH_INFO_MAX_WARRIOR, "warrior name")


def auth1_certificate_for(auth_key, key, user_id, now, expires, community_id=1, trust_level=1):
    """Auth1Certificate: family/times, user/community/trust, ElGamal public key."""
    encoded_key = public_key(key)
    body = (struct.pack("<HIIIIHH", 1, now, expires, user_id, community_id,
                        trust_level, len(encoded_key)) + encoded_key)
    return body + sign(auth_key, body)


def auth1_login_reply(keys, session_key, username, now=None, client_key=None,
                      return_client_key=False, user_id=None, tribes=()):
    """Build a signed, local-realm Auth1 success reply for a verified account.

    The response contains a family-1 certificate, a family-2 certificate, and
    the encrypted session-key confirmation required by the Auth1 client. The
    family-2 certificate also carries the Tribes 2 AuthInfo community data.
    """
    auth_key = keys["auth"]
    if not session_key or len(session_key) > 56:
        raise ValueError("Invalid Auth1 session key")
    username = str(username)
    name = username.encode("utf-16le")
    if len(name) // 2 > 0xffff:
        raise ValueError("Auth1 username is too long")

    now = int(time.time()) if now is None else int(now)
    expires = now + 86400
    user_id = local_auth_user_id(username) if user_id is None else int(user_id)
    if not 0 < user_id <= 0xffffffff:
        raise ValueError("Invalid Auth1 user ID")
    community_id = 1
    trust_level = 1
    client_key = make_key(auth_key) if client_key is None else dict(client_key)
    if (any(name not in client_key for name in ("p", "q", "g", "x", "y")) or
            pow(client_key["g"], client_key["x"], client_key["p"]) != client_key["y"]):
        raise ValueError("Invalid Auth1 client key")
    encoded_key = public_key(client_key)

    auth1_certificate = auth1_certificate_for(auth_key, client_key, user_id, now, expires,
                                              community_id, trust_level)
    certificate_signer = auth_key

    # Auth2Certificate data: type 0 is the account identity; type 1 is the
    # per-community blob that WONGetAuthInfo() reads from the first community.
    auth_data = (struct.pack("<IH", user_id, len(encoded_key)) + encoded_key +
                 struct.pack("<H", len(name) // 2) + name +
                 struct.pack("<BIH", 1, community_id, trust_level))
    auth_info = t2_auth_info_blob(username, tribes)
    community_data = struct.pack("<IH", community_id, len(auth_info)) + auth_info
    auth2_body = (struct.pack("<HIIH", 2, now, expires, 2) +
                  struct.pack("<HH", 0, len(auth_data)) + auth_data +
                  struct.pack("<HH", 1, len(community_data)) + community_data)
    auth2_certificate = auth2_body + sign(certificate_signer, auth2_body)

    # Clear blocks are emitted in Auth1 enum order: certificate, current key
    # block, and Auth2 certificate. Re-sending the signed block lets the client
    # verify the certificates against the same auth key used for this login.
    current_key_block = key_block(keys, now)
    clear = (b"\x03" + b"\x01" + struct.pack("<H", len(auth1_certificate)) + auth1_certificate +
             b"\x03" + struct.pack("<H", len(current_key_block)) + current_key_block +
             b"\x06" + struct.pack("<H", len(auth2_certificate)) + auth2_certificate)
    # The native reply carries five DER INTEGERs (without the enclosing
    # SEQUENCE) in its encrypted ClientPrivateKey block.
    private_block = private_key_components(client_key)

    secret_confirmation = secrets.token_bytes(2) + session_key
    encrypted_entries = (b"\x02\x02" + struct.pack("<H", len(private_block)) + private_block +
                         b"\x04" + struct.pack("<H", len(secret_confirmation)) + secret_confirmation)
    encrypted = encrypt_won_crypt_transform(session_key, encrypted_entries)
    reply = struct.pack("<BHHhB", 5, 12, 6, 0, 0) + clear + encrypted
    return (reply, client_key) if return_client_key else reply


def decrypt_elgamal(key, ciphertext):
    width = (key["p"].bit_length() + 7) // 8
    if len(ciphertext) < 4:
        raise ValueError("Truncated ElGamal block count")
    count, = struct.unpack_from("<I", ciphertext)
    if not count or len(ciphertext) != 4 + count * 2 * width:
        raise ValueError("Invalid ElGamal ciphertext size")
    plain = bytearray()
    for index in range(count):
        offset = 4 + index * 2 * width
        a = int.from_bytes(ciphertext[offset:offset + width], "big")
        b = int.from_bytes(ciphertext[offset + width:offset + 2 * width], "big")
        if not 0 < a < key["p"]:
            raise ValueError("ElGamal first component out of range")
        if pow(a, key["q"], key["p"]) != 1:
            raise ValueError("ElGamal first component is outside the q-order subgroup")
        # WON's positive BigInteger serializer can emit the second component
        # with the modulus-width top bit set; reduction is part of ElGamal math.
        value = (b % key["p"]) * pow(pow(a, key["x"], key["p"]), -1, key["p"]) % key["p"]
        # Preserve the full-width integer. The login wrapper decodes its
        # length-framed session key from the least-significant bytes.
        plain += value.to_bytes(width, "big")
    return bytes(plain)


def encrypt_elgamal_challenge(key, plaintext):
    """Encrypt native IRC challenge chunks for the client's ElGamal key.

    The client expects a little-endian block count followed by fixed-width
    ElGamal pairs. Each decrypted integer ends with a one-byte chunk length.
    """
    plaintext = bytes(plaintext)
    width = (key["p"].bit_length() + 7) // 8
    max_chunk = min(width - 3, 0xff)
    if not plaintext or max_chunk < 1:
        raise ValueError("Invalid ElGamal challenge plaintext")
    chunks = [plaintext[offset:offset + max_chunk]
              for offset in range(0, len(plaintext), max_chunk)]
    ciphertext = bytearray(struct.pack("<I", len(chunks)))
    for chunk in chunks:
        message = int.from_bytes(chunk + bytes([len(chunk)]), "big")
        if message >= key["p"]:
            raise ValueError("ElGamal challenge chunk exceeds modulus")
        exponent = secrets.randbelow(key["q"] - 1) + 1
        first = pow(key["g"], exponent, key["p"])
        second = message * pow(key["y"], exponent, key["p"]) % key["p"]
        ciphertext.extend(first.to_bytes(width, "big"))
        ciphertext.extend(second.to_bytes(width, "big"))
    return bytes(ciphertext)


def auth1_login_material(reply):
    """Extract the client public key and Auth2 certificate from an Auth1 reply."""
    if len(reply) < 9:
        raise ValueError("Truncated Auth1 reply")
    message_type, service, message, status, mask = struct.unpack_from("<BHHhB", reply)
    if (message_type, service, message, status, mask) != (5, 12, 6, 0, 0):
        raise ValueError("Unexpected Auth1 reply header")

    block_count = reply[8]
    offset = 9
    client_key = None
    auth2_certificate = None
    for _ in range(block_count):
        if offset + 3 > len(reply):
            raise ValueError("Truncated Auth1 certificate block")
        block_type = reply[offset]
        block_length, = struct.unpack_from("<H", reply, offset + 1)
        offset += 3
        if offset + block_length > len(reply):
            raise ValueError("Truncated Auth1 certificate data")
        block = reply[offset:offset + block_length]
        offset += block_length
        if block_type == 1:
            fields = struct.unpack_from("<HIIIIHH", block)
            key_length = fields[-1]
            if key_length > len(block) - 22:
                raise ValueError("Truncated Auth1 client public key")
            client_key = read_der_public(block[22:22 + key_length])
        elif block_type == 6:
            auth2_certificate = block
    if client_key is None or auth2_certificate is None:
        raise ValueError("Auth1 reply omitted identity certificate material")
    return {"client_key": client_key, "auth2_certificate": auth2_certificate}


def won_peer_challenge(client_key, challenge, certificate):
    """Peer-auth 0x33: a challenge encrypted to the client, plus our certificate."""
    challenge = bytes(challenge)
    if len(challenge) > 0xffff:
        raise ValueError("WON peer challenge is too long")
    encrypted = encrypt_elgamal_challenge(
        client_key, struct.pack("<H", len(challenge)) + challenge)
    return (struct.pack("<IIH", 0xcb, 0x33, len(encrypted)) + encrypted +
            struct.pack("<H", len(certificate)) + certificate)


def won_peer_complete(client_key, secret):
    """Peer-auth 0x35: return the client's secret, encrypted to the client."""
    secret = bytes(secret)
    if not secret or len(secret) > 0xffff:
        raise ValueError("Invalid WON peer secret")
    encrypted = encrypt_elgamal_challenge(client_key, struct.pack("<H", len(secret)) + secret)
    return struct.pack("<IIHH", 0xcb, 0x35, 0, len(encrypted)) + encrypted + struct.pack("<H", 0)


def won_peer_response_secret(message, server_key, challenge):
    """Peer-auth 0x34: the client's secret, if it echoes our challenge."""
    if len(message) < 10 or struct.unpack_from("<II", message) != (0xcb, 0x34):
        raise ValueError("Expected a WON peer-auth 0x34 response")
    length, = struct.unpack_from("<H", message, 8)
    if length != len(message) - 10:
        raise ValueError("Invalid WON peer-auth 0x34 ciphertext length")
    plaintext = decrypt_elgamal_challenge_chunks(server_key, message[10:])
    if len(plaintext) < 2:
        raise ValueError("Truncated WON peer-auth 0x34 proof")
    echoed_length, = struct.unpack_from("<H", plaintext)
    echoed = plaintext[2:2 + echoed_length]
    secret = plaintext[2 + echoed_length:]
    if echoed != bytes(challenge) or not 1 <= len(secret) <= 56:
        raise ValueError("WON peer-auth 0x34 does not answer the challenge")
    return secret


# After peer auth, each message travels as 0x0C | u16 client session id |
# CryptTransform(u16 sequence | message), keyed by the 0x33 challenge. Both
# sides number their own messages from 1.
WON_PEER_ENCRYPTED = 0x0C


def decrypt_won_peer_message(key, payload):
    if len(payload) < 3 or payload[0] != WON_PEER_ENCRYPTED:
        raise ValueError("Not a WON encrypted peer message")
    session_id, = struct.unpack_from("<H", payload, 1)
    plaintext = decrypt_won_crypt_transform(key, payload[3:])
    if len(plaintext) < 2:
        raise ValueError("Truncated WON encrypted peer message")
    sequence, = struct.unpack_from("<H", plaintext)
    return session_id, sequence, plaintext[2:]


def encrypt_won_peer_message(key, session_id, sequence, message):
    return (struct.pack("<BH", WON_PEER_ENCRYPTED, session_id) +
            encrypt_won_crypt_transform(key, struct.pack("<H", sequence) + message))


def build_won_irc_cb33(reply, challenge):
    """Build a CB33 IRC challenge encrypted to the Auth1 client key."""
    material = auth1_login_material(reply)
    return encode_won_token(won_peer_challenge(
        material["client_key"], challenge, material["auth2_certificate"]))


def build_won_irc_cb35(reply, session_key):
    """Build the CB35 final reply by returning the Auth1 session-key challenge."""
    return encode_won_token(won_peer_complete(auth1_login_material(reply)["client_key"], session_key))


class _BlowfishKey(ctypes.Structure):
    _fields_ = [("P", ctypes.c_uint32 * 18), ("S", (ctypes.c_uint32 * 256) * 4)]


def _blowfish_cbc(key, data, iv, encrypt):
    if not 4 <= len(key) <= 56:
        raise ValueError("Invalid WON Blowfish key length")
    if len(data) == 0 or len(data) % 8:
        raise ValueError("Invalid WON Blowfish ciphertext length")
    if len(iv) != 8:
        raise ValueError("WON Blowfish IV must be 8 bytes")
    library = ctypes.util.find_library("crypto")
    if not library:
        raise ValueError("OpenSSL libcrypto is unavailable for WON Blowfish decryption")
    crypto = ctypes.CDLL(library)
    try:
        set_key = crypto.BF_set_key
        cbc = crypto.BF_cbc_encrypt
    except AttributeError as exc:
        raise ValueError("OpenSSL libcrypto does not expose Blowfish support") from exc
    set_key.argtypes = [ctypes.POINTER(_BlowfishKey), ctypes.c_int, ctypes.c_void_p]
    cbc.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long,
                    ctypes.POINTER(_BlowfishKey), ctypes.c_void_p, ctypes.c_int]
    schedule = _BlowfishKey()
    key_buffer = ctypes.create_string_buffer(key, len(key))
    input_buffer = ctypes.create_string_buffer(data, len(data))
    output_buffer = ctypes.create_string_buffer(len(data))
    iv_buffer = ctypes.create_string_buffer(iv, len(iv))
    set_key(ctypes.byref(schedule), len(key), key_buffer)
    cbc(input_buffer, output_buffer, len(data), ctypes.byref(schedule), iv_buffer, int(encrypt))
    return output_buffer.raw


def decrypt_blowfish_cbc(key, ciphertext, iv=b"\xff" * 8):
    return _blowfish_cbc(key, ciphertext, iv, False)


def decrypt_won_crypt_transform(key, ciphertext):
    transformed = decrypt_blowfish_cbc(key, ciphertext)
    if len(transformed) < 4:
        raise ValueError("Truncated WON CryptTransform data")
    plaintext_length, = struct.unpack_from("<I", transformed)
    if plaintext_length > len(transformed) - 4:
        raise ValueError("Invalid WON CryptTransform plaintext length")
    return transformed[4:4 + plaintext_length]


def encrypt_won_crypt_transform(key, plaintext, iv=b"\xff" * 8):
    data = struct.pack("<I", len(plaintext)) + plaintext
    data += b"\0" * (-len(data) % 8)
    return _blowfish_cbc(key, data, iv, True)


def decrypt_won_login_data(auth_key, encrypted_key, encrypted_data):
    material = decrypt_elgamal(auth_key, encrypted_key)
    if len(material) < 2:
        raise ValueError("Truncated wrapped WON session key")
    key_length = material[-1]
    if not 4 <= key_length <= 56 or len(material) < key_length + 1:
        raise ValueError("Invalid wrapped WON session-key length")
    session_key = material[-key_length - 1:-1]
    return session_key, decrypt_won_crypt_transform(session_key, encrypted_data)


def _rc4_drop2048(key, data):
    state = list(range(256))
    j = 0
    for i in range(256):
        j = (j + state[i] + key[i % len(key)]) & 0xff
        state[i], state[j] = state[j], state[i]
    i = j = 0
    for _ in range(2048):
        i = (i + 1) & 0xff
        j = (j + state[i]) & 0xff
        state[i], state[j] = state[j], state[i]
    output = bytearray(len(data))
    for offset, value in enumerate(data):
        i = (i + 1) & 0xff
        j = (j + state[i]) & 0xff
        state[i], state[j] = state[j], state[i]
        output[offset] = value ^ state[(state[i] + state[j]) & 0xff]
    return bytes(output)


def verify_private_store_password(path, username, password):
    """Verify a password against TribesNext private.store without exposing key material.

    Returns one of: ``valid``, ``invalid``, ``account_missing``,
    ``store_missing``, or ``unsupported_password``.
    """
    path = Path(path) if path is not None else None
    if path is None or not path.is_file():
        return "store_missing"
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError):
        return "invalid"
    encrypted_value = None
    for line in lines:
        if "\t" not in line:
            continue
        stored_user, value = line.split("\t", 1)
        if stored_user.casefold() == username.casefold():
            encrypted_value = value.strip()
            break
    if encrypted_value is None:
        return "account_missing"
    if ":" not in encrypted_value:
        try:
            encrypted_value = base64.b64decode(encrypted_value, validate=True).decode("ascii")
        except (ValueError, UnicodeError):
            return "invalid"
    if encrypted_value.count(":") != 1:
        return "invalid"
    nonce, ciphertext_hex = encrypted_value.split(":", 1)
    if not nonce or not ciphertext_hex or len(ciphertext_hex) % 2:
        return "invalid"
    try:
        ciphertext = bytes.fromhex(ciphertext_hex)
        key = hashlib.sha1((password + nonce).encode("ascii")).hexdigest().encode("ascii")
    except (ValueError, UnicodeError):
        return "unsupported_password"
    decrypted_hex = _rc4_drop2048(key, ciphertext).hex()
    actual_nonce = hashlib.sha1(decrypted_hex.encode("ascii")).hexdigest()
    return "valid" if actual_nonce.casefold() == nonce.casefold() else "invalid"
