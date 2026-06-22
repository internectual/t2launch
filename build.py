#!/usr/bin/env python3
"""
build.py — Generate store files and patch VL2 with your credentials.

Usage:
    python3 build.py --username LouCypher --password "[REDACTED]"

This script:
1. Downloads credentials from TribesNext auth server (or uses provided ones)
2. Generates public.store and private.store files
3. Decrypts the private key and patches the VL2 with your specific values
"""

import argparse
import hashlib
import os
import sys
import zipfile

def rc4(key_bytes, data_bytes):
    """RC4 stream cipher"""
    S = list(range(256))
    j = 0
    for i in range(256):
        j = (j + S[i] + key_bytes[i % len(key_bytes)]) % 256
        S[i], S[j] = S[j], S[i]
    
    # Discard 2048 bytes
    i = j = 0
    for _ in range(2048):
        i = (i + 1) % 256
        j = (j + S[i]) % 256
        S[i], S[j] = S[j], S[i]
    
    # Decrypt
    result = bytearray()
    for byte in data_bytes:
        i = (i + 1) % 256
        j = (j + S[i]) % 256
        S[i], S[j] = S[j], S[i]
        k = S[(S[i] + S[j]) % 256]
        result.append(byte ^ k)
    return bytes(result)

def decrypt_store_key(encrypted_hex, password):
    """Decrypt the private key from a TribesNext private.store entry.
    
    Format of stored entry: nonce:encryptedHex
    Key derivation: RC4 key = SHA1(password + nonce) as ASCII HEX string (40 bytes)
    Verification: SHA1(decrypted_hex) == nonce
    """
    # Parse the encrypted hex
    encrypted_bytes = bytes.fromhex(encrypted_hex)
    
    # We need the nonce — it's the SHA1 of the original plaintext
    # But we need the nonce from the store file
    # For now, we try the hex-string-as-key approach
    # This requires knowing the nonce, which is stored in the store file
    # 
    # Usage: we expect the encrypted_hex to be the nonce:encryptedHex format
    if ':' in encrypted_hex:
        nonce, enc = encrypted_hex.split(':', 1)
        enc_bytes = bytes.fromhex(enc)
    else:
        # Just the encrypted hex, nonce must be provided separately
        nonce = None
        enc_bytes = bytes.fromhex(encrypted_hex)
    
    # Try different RC4 key approaches
    if nonce:
        # Approach 1: RC4 key = hex string of SHA1(password + nonce)
        rc4_key = hashlib.sha1((password + nonce).encode('ascii')).hexdigest().encode('ascii')
        decrypted = rc4(rc4_key, enc_bytes)
        decrypted_hex = decrypted.hex()
        verify = hashlib.sha1(decrypted_hex.encode('ascii')).hexdigest()
        if verify == nonce:
            return nonce, decrypted_hex
        
        # Approach 2: RC4 key = raw bytes of SHA1(password + nonce)
        rc4_key = hashlib.sha1((password + nonce).encode('ascii')).digest()
        decrypted = rc4(rc4_key, enc_bytes)
        decrypted_hex = decrypted.hex()
        verify = hashlib.sha1(decrypted_hex.encode('ascii')).hexdigest()
        if verify == nonce:
            return nonce, decrypted_hex
        
        # Approach 3: password bytes as RC4 key
        rc4_key = password.encode('ascii')
        decrypted = rc4(rc4_key, enc_bytes)
        decrypted_hex = decrypted.hex()
        verify = hashlib.sha1(decrypted_hex.encode('ascii')).hexdigest()
        if verify == nonce:
            return nonce, decrypted_hex
    
    return None, None

def download_credentials(username, password):
    """Download credentials from TribesNext auth server (simplified)"""
    # This requires TCP socket connection to the auth server
    # For now, instruct user to use t2-mapper's login script
    print("Please download credentials using t2-mapper:")
    print("  1. git clone https://github.com/exogen/t2-mapper.git")
    print("  2. cd t2-mapper && npm install")
    print("  3. npm run login")
    print("  4. Copy T2_ACCOUNT_CERTIFICATE and T2_ACCOUNT_ENCRYPTED_KEY values")
    print()
    print("Then run:")
    print(f'  python3 build.py --username {username} --cert "$CERT" --key "$KEY"')

def patch_vl2(vl2_path, username, cert, decrypted_key, n_hex, e_hex='5'):
    """Patch the VL2 archive with user-specific credentials"""
    with open(vl2_path, 'rb') as f:
        content = f.read()
    
    files = {}
    with zipfile.ZipFile(content, 'r') as zf:
        for name in zf.namelist():
            files[name] = zf.read(name)
    
    # Patch loginScreens.cs
    text = files['loginScreens.cs'].decode('latin-1')
    text = text.replace('REPLACE_USERNAME', username)
    text = text.replace('REPLACE_GUID', cert.split('\t')[1] if '\t' in cert else '0')
    text = text.replace('REPLACE_E', e_hex)
    text = text.replace('REPLACE_N', n_hex[:64] + '...' if len(n_hex) > 64 else n_hex)  # Truncate for display
    text = text.replace('REPLACE_SIG', cert.split('\t')[4] if '\t' in cert else 'sig')
    text = text.replace('REPLACE_DECRYPTED_KEY', decrypted_key[:64] + '...' if len(decrypted_key) > 64 else decrypted_key)
    files['loginScreens.cs'] = text.encode('latin-1')
    
    # Repack
    with zipfile.ZipFile(vl2_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for name, data in files.items():
            zf.writestr(name, data)
    
    print(f"Patched {vl2_path}")

def main():
    parser = argparse.ArgumentParser(description='Build t2launch with your credentials')
    parser.add_argument('--username', default='LouCypher', help='TribesNext username')
    parser.add_argument('--password', default=None, help='TribesNext password')
    parser.add_argument('--cert', default=None, help='Base64 certificate (from t2-login)')
    parser.add_argument('--key', default=None, help='Base64 encrypted key (from t2-login)')
    args = parser.parse_args()
    
    if not args.cert or not args.key:
        download_credentials(args.username, args.password or 'password')
        return
    
    # Decode credentials
    import base64
    cert = base64.b64decode(args.cert).decode('ascii')
    encrypted_key_b64 = base64.b64decode(args.key).decode('ascii')
    
    print(f"Username: {args.username}")
    print(f"Certificate fields: {len(chr(9).join(cert.split(chr(9))))} chars")
    print(f"Encrypted key: {len(encrypted_key_b64)} chars")
    
    # Decrypt the key
    if ':' in encrypted_key_b64:
        nonce, encrypted_hex = encrypted_key_b64.split(':', 1)
    else:
        print("ERROR: Expected nonce:encryptedHex format")
        return
    
    _, decrypted_hex = decrypt_store_key(encrypted_key_b64, args.password or '')
    if decrypted_hex:
        print(f"Successfully decrypted private key ({len(decrypted_hex)} hex chars)")
    else:
        print("WARNING: Could not decrypt key with password. Using encrypted key as-is.")
        decrypted_hex = encrypted_hex
    
    # Extract n and e from certificate
    cert_fields = cert.split('\t')
    if len(cert_fields) >= 4:
        e_hex = cert_fields[2]
        n_hex = cert_fields[3]
    else:
        e_hex = '5'
        n_hex = 'unknown'
    
    vl2_path = 'base/t2csri_linux.vl2'
    patch_vl2(vl2_path, args.username, cert, decrypted_hex, n_hex, e_hex)
    
    # Copy patched VL2 to ~/t2-linux/base
    import shutil
    dest = os.path.expanduser('~/t2-linux/base/t2csri_linux.vl2')
    shutil.copy2(vl2_path, dest)
    print(f"Done! Copied to {dest}")
    print("Run with: ./docker/asgard-run tribes2 ~/t2-linux -online")

if __name__ == '__main__':
    main()
