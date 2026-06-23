#!/usr/bin/env python3
"""
rsa_file_daemon.py — File-based RSA decryption daemon for t2launch.
Watches for /tmp/rsa_request.txt, computes RSA decryption, writes result to /tmp/rsa_result.txt.

Usage:
    python3 rsa_file_daemon.py
"""

import os
import time
import hashlib

REQUEST_FILE = "/tmp/rsa_challenge.txt"
RESULT_FILE = "/tmp/rsa_result.txt"

def rsa_mod_exp(base_hex, exp_hex, mod_hex):
    """Fast RSA modular exponentiation"""
    base = int(base_hex, 16)
    exp = int(exp_hex, 16)
    mod = int(mod_hex, 16)
    result = pow(base, exp, mod)
    return format(result, 'x')

def main():
    print("RSA file daemon started", flush=True)
    while True:
        if os.path.exists(REQUEST_FILE):
            try:
                with open(REQUEST_FILE, 'r') as f:
                    line = f.read().strip()
                parts = line.split()
                if len(parts) == 3:
                    result = rsa_mod_exp(parts[0], parts[1], parts[2])
                    with open(RESULT_FILE, 'w') as f:
                        f.write(result + '\n')
                    os.remove(REQUEST_FILE)
                    print(f"Decrypted, result length: {len(result)}", flush=True)
            except Exception as e:
                print(f"Error: {e}", flush=True)
                try:
                    os.remove(REQUEST_FILE)
                except:
                    pass
        time.sleep(0.1)

if __name__ == '__main__':
    main()
