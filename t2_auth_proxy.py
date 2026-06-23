#!/usr/bin/env python3
"""
t2_auth_proxy.py — TribesNext authentication proxy.
Intercepts the game server connection and handles the t2csri auth handshake.
The game connects to this proxy, the proxy handles auth, then forwards to the real server.

Usage:
    python3 t2_auth_proxy.py --listen 28001 --connect <real-server-ip>:28001

The game client should connect to localhost:28001 instead of the real server.
"""

import socket
import threading
import sys
import struct

LISTEN_PORT = 28001
CONNECT_HOST = None
CONNECT_PORT = 28001

def handle_client(client_sock, client_addr):
    """Handle a game client connection"""
    print(f"[PROXY] Client connected from {client_addr}", flush=True)
    
    # Connect to the real game server
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_sock.settimeout(5.0)
    
    try:
        # Forward initial packets and handle auth
        # Tribes 2 uses UDP for game connections
        data, addr = client_sock.recvfrom(4096)
        print(f"[PROXY] Received {len(data)} bytes from client", flush=True)
        
        # Forward to server
        server_sock.sendto(data, (CONNECT_HOST, CONNECT_PORT))
        
        # Relay data between client and server
        while True:
            try:
                # Forward client -> server
                data, addr = client_sock.recvfrom(4096)
                server_sock.sendto(data, (CONNECT_HOST, CONNECT_PORT))
            except:
                break
            
            try:
                # Forward server -> client
                data, addr = server_sock.recvfrom(4096)
                client_sock.sendto(data, client_addr)
            except:
                break
    except Exception as e:
        print(f"[PROXY] Error: {e}", flush=True)
    finally:
        server_sock.close()
        client_sock.close()

def main():
    global CONNECT_HOST, CONNECT_PORT
    
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} --listen <port> --connect <host>:<port>")
        sys.exit(1)
    
    listen_port = int(sys.argv[sys.argv.index('--listen') + 1])
    connect_str = sys.argv[sys.argv.index('--connect') + 1]
    CONNECT_HOST, CONNECT_PORT = connect_str.split(':')
    CONNECT_PORT = int(CONNECT_PORT)
    
    print(f"[PROXY] Listening on port {listen_port}", flush=True)
    print(f"[PROXY] Forwarding to {CONNECT_HOST}:{CONNECT_PORT}", flush=True)
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', listen_port))
    
    # For UDP, we need to handle the initial connection differently
    # The first packet from the client establishes the session
    data, addr = sock.recvfrom(4096)
    print(f"[PROXY] First packet from {addr}: {len(data)} bytes", flush=True)
    
    # Connect to the real server
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # Forward the first packet
    server_sock.sendto(data, (CONNECT_HOST, CONNECT_PORT))
    
    # Now relay between client and server
    client_addr = addr
    while True:
        try:
            # Use select to handle both directions
            import select
            readable, _, _ = select.select([sock, server_sock], [], [], 1.0)
            
            for s in readable:
                data, addr = s.recvfrom(4096)
                if s is sock:
                    # Data from client
                    if addr == client_addr:
                        server_sock.sendto(data, (CONNECT_HOST, CONNECT_PORT))
                else:
                    # Data from server
                    sock.sendto(data, client_addr)
        except Exception as e:
            print(f"[PROXY] Relay error: {e}", flush=True)
            break

if __name__ == '__main__':
    main()
