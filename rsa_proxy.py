#!/usr/bin/env python3
"""
rsa_proxy.py — RSA decryption proxy for t2launch.
Listens on localhost:18765, receives decrypt requests, returns decrypted hex.

Protocol (line-based, newline-terminated):
    Request:  base_hex exp_hex mod_hex\n
    Response: decrypted_hex\n
    Or:      ERROR: message\n

Usage:
    python3 rsa_proxy.py &
"""

import hashlib
import json
import os
import socket
import sys
import threading

# Cache for decrypted challenges (server sends same challenge for a while)
cache = {}
cache_lock = threading.Lock()

def rsa_mod_exp(base_hex, exp_hex, mod_hex):
    """Fast RSA modular exponentiation using Python's built-in pow()"""
    base = int(base_hex, 16)
    exp = int(exp_hex, 16)
    mod = int(mod_hex, 16)
    result = pow(base, exp, mod)
    return format(result, 'x')

def handle_client(conn, addr):
    """Handle a single client connection"""
    try:
        data = b''
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
            if b'\n' in data:
                break
        
        request = data.decode('ascii').strip()
        if not request:
            return
        
        parts = request.split()
        if len(parts) != 3:
            conn.sendall(b"ERROR: expected 3 hex values\n")
            return
        
        base_hex, exp_hex, mod_hex = parts
        
        # Check cache
        cache_key = f"{base_hex}:{exp_hex}:{mod_hex}"
        with cache_lock:
            if cache_key in cache:
                conn.sendall(f"{cache[cache_key]}\n".encode('ascii'))
                return
        
        # Compute
        result = rsa_mod_exp(base_hex, exp_hex, mod_hex)
        
        # Store in cache
        with cache_lock:
            cache[cache_key] = result
        
        conn.sendall(f"{result}\n".encode('ascii'))
    except Exception as e:
        try:
            conn.sendall(f"ERROR: {e}\n".encode('ascii'))
        except:
            pass
    finally:
        conn.close()

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18765
    
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', port))
    server.listen(5)
    print(f"RSA proxy listening on 127.0.0.1:{port}", flush=True)
    
    while True:
        conn, addr = server.accept()
        t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
        t.start()

if __name__ == '__main__':
    main()
