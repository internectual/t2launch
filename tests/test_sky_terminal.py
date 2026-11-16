import importlib.util
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("sky_terminal", ROOT / "sky_terminal.py")
sky_terminal = importlib.util.module_from_spec(spec)
sys.modules["sky_terminal"] = sky_terminal
spec.loader.exec_module(sky_terminal)
Screen = sky_terminal.Screen


def screen_with(text, columns=20, rows=6):
    screen = Screen(columns, rows)
    screen.feed(text.encode())
    return screen


class ScreenTests(unittest.TestCase):
    def test_text_wraps_and_scrolls(self):
        screen = screen_with("abcdefghijklmnopqrstuvwx", columns=10, rows=2)
        self.assertEqual(screen.text(), ["klmnopqrst", "uvwx"])     # the first of three lines scrolled away
        screen = screen_with("0123456789ABCDEFGHIJxyz", columns=10, rows=2)
        self.assertEqual(screen.text(), ["ABCDEFGHIJ", "xyz"])      # three lines in two rows scrolled once

    def test_cursor_movement_and_erase(self):
        screen = screen_with("hello\r\nworld\x1b[1;3H\x1b[K\x1b[2;1H\x1b[2K!")
        self.assertEqual(screen.text()[0], "he")
        self.assertEqual(screen.text()[1], "!")
        screen = screen_with("abc\x1b[2J\x1b[Hz")
        self.assertEqual(screen.text()[0], "z")
        self.assertEqual(screen.text()[1], "")

    def test_sgr_colours(self):
        screen = screen_with("\x1b[31;1mR\x1b[0mn\x1b[38;5;200mP\x1b[48;2;1;2;3mT")
        self.assertEqual(screen.grid[0][0][1:], [1, sky_terminal.DEFAULT, sky_terminal.BOLD])
        self.assertEqual(screen.grid[0][1][1:], [sky_terminal.DEFAULT, sky_terminal.DEFAULT, 0])
        self.assertEqual(screen.grid[0][2][1], 200)
        self.assertEqual(screen.grid[0][3][2], sky_terminal.RGB_FLAG | (1 << 16) | (2 << 8) | 3)
        self.assertEqual(sky_terminal.resolve(screen.grid[0][3][2], (0, 0, 0)), (1, 2, 3))
        self.assertEqual(sky_terminal.palette(16), (0, 0, 0))
        self.assertEqual(sky_terminal.palette(231), (255, 255, 255))
        self.assertEqual(sky_terminal.palette(232), (8, 8, 8))

    def test_scroll_region_and_line_insert(self):
        screen = screen_with("1\r\n2\r\n3\r\n4\r\n5\x1b[2;4r\x1b[4;1H\n", columns=5, rows=5)
        self.assertEqual(screen.text(), ["1", "3", "4", "", "5"])
        screen = screen_with("a\r\nb\r\nc\x1b[2;1H\x1b[L", columns=5, rows=4)
        self.assertEqual(screen.text(), ["a", "", "b", "c"])

    def test_alternate_screen_restores_the_main_one(self):
        screen = screen_with("main\x1b[?1049h\x1b[Halt screen\x1b[?1049l")
        self.assertEqual(screen.text()[0], "main")
        self.assertFalse(screen.alternate)

    def test_utf8_split_across_feeds_and_line_drawing(self):
        screen = Screen(10, 2)
        data = "é─".encode()
        screen.feed(data[:1])
        screen.feed(data[1:])
        self.assertEqual(screen.text()[0], "é─")
        screen = screen_with("\x1b(0qqx\x1b(B")
        self.assertEqual(screen.text()[0], "──│")

    def test_osc_and_unknown_sequences_are_skipped(self):
        screen = screen_with("\x1b]0;title\x07ok\x1b[?2004h\x1bP1$r\x1b\\x\x1b[>c")
        self.assertEqual(screen.title, "title")
        self.assertEqual(screen.text()[0], "okx")

    def test_insert_delete_characters_and_tabs(self):
        screen = screen_with("abcdef\x1b[1;3H\x1b[2P")
        self.assertEqual(screen.text()[0], "abef")
        screen = screen_with("abcdef\x1b[1;3H\x1b[2@")
        self.assertEqual(screen.text()[0], "ab  cdef")
        screen = screen_with("a\tb")
        self.assertEqual(screen.grid[0][8][0], "b")

    def test_cursor_visibility_and_save_restore(self):
        screen = screen_with("\x1b[?25lab\x1b7\x1b[3;3Hz\x1b8c")
        self.assertFalse(screen.cursor_visible)
        self.assertEqual(screen.text()[0], "abc")
        self.assertEqual(screen.text()[2], "  z")


class PacketTests(unittest.TestCase):
    def test_packets_reassemble_across_reads(self):
        packets = sky_terminal.Packets()
        data = b"k\x03abcf\x011"
        self.assertEqual(packets.feed(data[:4]), [])
        self.assertEqual(packets.feed(data[4:]), [("k", b"abc"), ("f", b"1")])


class ProfileTests(unittest.TestCase):
    def test_profiles_come_only_from_the_config_file(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "sky_terminals.cfg"
            config.write_text("# comment\nmonitor = htop -d 5\nweird\n")
            self.assertEqual(sky_terminal.profile_command("monitor", config), ["htop", "-d", "5"])
            self.assertIsNone(sky_terminal.profile_command("rm", config))
            self.assertIsNone(sky_terminal.profile_command("monitor", Path(directory) / "missing.cfg"))
        default = sky_terminal.profile_command("default")
        self.assertEqual(default[-1], "-i")


@unittest.skipUnless(importlib.util.find_spec("PySide6"), "PySide6 is only in the launcher's venv")
class RenderTests(unittest.TestCase):
    WIDTH, HEIGHT = 320, 200

    def read_frame(self, process, timeout):
        import select
        size = self.WIDTH * self.HEIGHT * 4
        data = b""
        deadline = time.monotonic() + timeout
        while len(data) < size and time.monotonic() < deadline:
            if select.select([process.stdout], [], [], 0.2)[0]:
                chunk = os.read(process.stdout.fileno(), size - len(data))
                if not chunk:
                    break
                data += chunk
        return data if len(data) == size else b""

    @staticmethod
    def lit_pixels(frame):
        background = frame[:4]
        return sum(1 for i in range(0, len(frame), 4) if frame[i:i + 4] != background)

    def test_a_shell_frame_has_the_right_size_and_shows_typed_text(self):
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", SHELL="/bin/sh")
        process = subprocess.Popen([sys.executable, str(ROOT / "sky_terminal.py"), "--width", str(self.WIDTH),
                                    "--height", str(self.HEIGHT), "--font", "14", "--fps", "20"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
        try:
            first = self.read_frame(process, 10)
            self.assertEqual(len(first), self.WIDTH * self.HEIGHT * 4)
            text = b"echo hello from the sky\r"
            process.stdin.write(b"k" + bytes([len(text)]) + text)
            process.stdin.flush()
            best = self.lit_pixels(first)
            for _ in range(40):                     # the shell's reply arrives as one or more frames
                frame = self.read_frame(process, 1)
                if frame:
                    best = max(best, self.lit_pixels(frame))
                if best > self.lit_pixels(first) + 200:
                    break
            self.assertGreater(best, self.lit_pixels(first) + 200)     # typed text and its output are on screen
        finally:
            process.kill()
            process.wait()


if __name__ == "__main__":
    unittest.main()
