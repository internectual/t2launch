#!/usr/bin/env python3
"""
Tribes 2 authentication proxy - intercepts t2csri_* commands.

This proxy sits between the game client and the TribesNext game server.
It handles the t2csri authentication handshake on behalf of the client,
since the vanilla Loki engine doesn't know how to process t2csri commands.

Usage:
    python3 t2_auth_proxy.py --server <game-server-ip>:<port> [--listen <port>]

    Then connect the game to localhost:<proxy-port> instead of the real server.
"""

import socket
import struct
import sys
import time
import threading

class T2AuthProxy:
    def __init__(self, server_host, server_port, listen_port=28001):
        self.server_addr = (server_host, server_port)
        self.listen_port = listen_port
        
        self.listen_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listen_sock.bind(('0.0.0.0', self.listen_port))
        self.listen_sock.settimeout(0.1)
        
        self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server_sock.settimeout(0.1)
        
        self.client_addr = None
        self.running = True
        self.auth_state = 'idle'  # idle, poking, challenging, done
        
        print(f"[PROXY] Listening on port {self.listen_port}")
        print(f"[PROXY] Forwarding to {server_host}:{server_port}")
    
    def handle_server_command(self, cmd_name, args):
        """Handle a t2csri command from the server"""
        print(f"[PROXY] Server command: {cmd_name}")
        
        if cmd_name == 't2csri_pokeClient':
            version = args.decode().strip('\x00') if args else "unknown"
            print(f"[PROXY] t2csri_pokeClient: version={version}")
            self.auth_state = 'poking'
            
            # The client should respond by sending its certificate
            # But since the vanilla engine doesn't handle this,
            # we need to send the response ourselves
            
            # For now, just log it
            print(f"[PROXY] Would send certificate here")
            
        elif cmd_name == 't2csri_getChallengeChunk':
            chunk = args.decode().strip('\x00') if args else ""
            print(f"[PROXY] t2csri_getChallengeChunk: {len(chunk)} chars")
            self.auth_state = 'challenging'
            
        elif cmd_name == 't2csri_decryptChallenge':
            print(f"[PROXY] t2csri_decryptChallenge")
            self.auth_state = 'decrypting'
    
    def process_packet_from_server(self, data):
        """Process a packet coming from the server"""
        # Look for t2csri_ commands in the data
        for cmd_name in [b't2csri_pokeClient', b't2csri_getChallengeChunk', b't2csri_decryptChallenge']:
            idx = data.find(cmd_name)
            if idx != -1:
                # Extract arguments (everything after the command name)
                args_start = idx + len(cmd_name)
                args = data[args_start:]
                self.handle_server_command(cmd_name.decode(), args)
                break
        
        return data
    
    def run(self):
        """Main proxy loop"""
        print("[PROXY] Running... Press Ctrl+C to stop")
        
        while self.running:
            try:
                # Check for data from client
                try:
                    data, addr = self.listen_sock.recvfrom(65535)
                    if self.client_addr is None:
                        self.client_addr = addr
                        print(f"[PROXY] Client connected from {addr}")
                    
                    # Forward to server (unmodified)
                    self.server_sock.sendto(data, self.server_addr)
                except socket.timeout:
                    pass
                
                # Check for data from server
                try:
                    data, addr = self.server_sock.recvfrom(65535)
                    
                    # Process and potentially modify
                    data = self.process_packet_from_server(data)
                    
                    # Forward to client
                    if self.client_addr:
                        self.listen_sock.sendto(data, self.client_addr)
                except socket.timeout:
                    pass
                
            except KeyboardInterrupt:
                print("\n[PROXY] Stopping...")
                self.running = False
            except Exception as e:
                print(f"[PROXY] Error: {e}")
        
        self.listen_sock.close()
        self.server_sock.close()
        print("[PROXY] Stopped")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} --server <host>:<port> [--listen <port>]")
        sys.exit(1)
    
    server = sys.argv[sys.argv.index('--server') + 1]
    host, port = server.rsplit(':', 1)
    port = int(port)
    
    listen_port = 28001
    if '--listen' in sys.argv:
        listen_port = int(sys.argv[sys.argv.index('--listen') + 1])
    
    proxy = T2AuthProxy(host, port, listen_port)
    proxy.run()
