import json
import os
import socket
import struct
import tempfile
import threading
import unittest
from pathlib import Path

import discord_presence


class FakeDiscord:
    """A Discord IPC socket that answers the handshake and records each frame."""

    def __init__(self, path):
        self.frames = []
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(str(path))
        self.server.listen(1)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        connection, _ = self.server.accept()
        with connection:
            while True:
                try:
                    op, payload = discord_presence.read_frame(connection)
                except ConnectionError:
                    return
                self.frames.append((op, payload))
                if op == discord_presence.OP_HANDSHAKE:
                    reply = {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1}}
                else:
                    reply = {"cmd": payload["cmd"], "data": {}, "nonce": payload["nonce"]}
                connection.sendall(discord_presence.encode_frame(discord_presence.OP_FRAME, reply))


class PresenceTests(unittest.TestCase):
    def test_frames_round_trip(self):
        left, right = socket.socketpair()
        with left, right:
            left.sendall(discord_presence.encode_frame(1, {"cmd": "X"}))
            self.assertEqual(discord_presence.read_frame(right), (1, {"cmd": "X"}))
            left.sendall(struct.pack("<II", 1, discord_presence.FRAME_MAX_BYTES + 1))
            with self.assertRaises(ConnectionError):
                discord_presence.read_frame(right)

    def test_socket_paths_start_with_the_runtime_folder(self):
        paths = discord_presence.socket_paths({"XDG_RUNTIME_DIR": "/run/user/1000"})
        self.assertEqual(paths[0], Path("/run/user/1000/discord-ipc-0"))
        self.assertIn(Path("/run/user/1000/app/com.discordapp.Discord/discord-ipc-0"), paths)
        self.assertIn(Path("/tmp/discord-ipc-9"), paths)

    def test_activity_for_each_state(self):
        game = discord_presence.activity_for("game\t\x02My \x03Server\tKatabatic\tCTF\t12", 100)
        self.assertEqual(game["details"], "Katabatic - CTF")
        self.assertEqual(game["state"], "My Server (12 players)")
        self.assertEqual(game["timestamps"], {"start": 100})
        self.assertEqual(discord_presence.activity_for("game\tX\tMap\tDM\t1", 0)["details"], "Map - DM")
        self.assertEqual(discord_presence.activity_for("game\tX\tMap\tDM\t1", 0)["state"], "X (1 player)")
        self.assertNotIn("state", discord_presence.activity_for("game\t\tM\t\t1", 0))
        self.assertEqual(discord_presence.activity_for("game\t\tM\t\t1", 0)["details"], "In a game")
        self.assertEqual(discord_presence.activity_for("menu", 0)["details"], "In the menus")
        self.assertEqual(discord_presence.activity_for("joining", 0)["details"], "Joining a game")
        self.assertEqual(discord_presence.activity_for("demo", 0)["details"], "Watching a demo")
        self.assertEqual(len(discord_presence.clean("x" * 300)), discord_presence.FIELD_MAX)

    def test_start_time_lasts_for_one_server(self):
        key = discord_presence.session_key
        self.assertEqual(key("joining"), key("joining"))
        self.assertEqual(key("game\tA\tMap1\tCTF\t3"), key("game\tA\tMap2\tCTF\t5"))
        self.assertNotEqual(key("game\tA\tMap\tCTF\t3"), key("game\tB\tMap\tCTF\t3"))
        self.assertNotEqual(key("menu"), key("joining"))

    def test_run_sends_each_state_and_clears_on_exit(self):
        with tempfile.TemporaryDirectory() as runtime:
            fake = FakeDiscord(Path(runtime) / "discord-ipc-0")
            game, helper = socket.socketpair()
            reader = helper.makefile("rb")
            result = {}
            thread = threading.Thread(target=lambda: result.setdefault("code", discord_presence.run(
                "1234", 42, reader, {"XDG_RUNTIME_DIR": runtime}, clock=lambda: 1000.0)))
            thread.start()
            game.sendall(b"joining\ngame\tServer\tMap\tCTF\t2\n")
            game.close()
            thread.join(10)
            self.assertEqual(result.get("code"), 0)
            fake.thread.join(5)
            reader.close()
            helper.close()
            fake.server.close()
        self.assertEqual(fake.frames[0], (0, {"v": 1, "client_id": "1234"}))
        activities = [payload["args"]["activity"] for op, payload in fake.frames[1:]]
        self.assertTrue(all(payload["args"]["pid"] == 42 for _, payload in fake.frames[1:]))
        self.assertEqual(activities[0]["details"], "In the menus")
        self.assertEqual(activities[-2]["state"], "Server (2 players)")
        self.assertIsNone(activities[-1])

    def test_main_rejects_a_bad_application_id(self):
        self.assertEqual(discord_presence.main(["--app-id", "abc"]), 2)


if __name__ == "__main__":
    unittest.main()
