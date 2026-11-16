#!/usr/bin/env python3
"""Text views for the Tribes 2 sky: a terminal (a real shell on a pty) or the game's console log.

The hook starts this beside the game and reads raw RGBA frames (--width x --height, top row first) from its
stdout, whenever the screen changed (at most --fps per second). Its stdin carries small packets from the hook:

    'k' <length> <bytes>   keystrokes, already translated to terminal bytes, for the shell
    'f' 1 '0' | '1'        whether the view has the keyboard (draws a frame around itself)

Modes:
    shell    a terminal running the profile's command (default: $SHELL); profiles are "name=command" lines in
             ~/.config/t2launch/sky_terminals.cfg, never taken from the game, so a map script cannot run programs
    console  the tail of the game's console log (--tail FILE), read only

Only the emulator (Screen) is needed to test the parsing; Qt (PySide6, offscreen) is imported when drawing.
"""

from __future__ import annotations

import argparse
import codecs
import errno
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time
from pathlib import Path

DEFAULT = -1
RGB_FLAG = 0x1000000
BOLD, DIM, ITALIC, UNDERLINE, REVERSE = 1, 2, 4, 8, 16

ANSI = [
    (0x1c, 0x1c, 0x1c), (0xcc, 0x33, 0x33), (0x33, 0xbb, 0x44), (0xcc, 0xaa, 0x22),
    (0x44, 0x66, 0xdd), (0xbb, 0x44, 0xbb), (0x33, 0xaa, 0xbb), (0xcc, 0xcc, 0xcc),
    (0x66, 0x66, 0x66), (0xff, 0x55, 0x55), (0x55, 0xee, 0x66), (0xff, 0xdd, 0x55),
    (0x77, 0x99, 0xff), (0xee, 0x77, 0xee), (0x55, 0xdd, 0xee), (0xff, 0xff, 0xff),
]
DEFAULT_FG = (0xd0, 0xe8, 0xd0)
DEFAULT_BG = (0x05, 0x0c, 0x08)

LINE_DRAWING = {
    "j": "┘", "k": "┐", "l": "┌", "m": "└", "n": "┼", "q": "─", "t": "├", "u": "┤", "v": "┴", "w": "┬",
    "x": "│", "a": "▒", "`": "◆", "f": "°", "g": "±", "~": "·", "o": "⎺", "s": "⎽",
}


def palette(index: int) -> tuple[int, int, int]:
    if index < 16:
        return ANSI[index]
    if index < 232:
        index -= 16
        steps = (0, 95, 135, 175, 215, 255)
        return steps[index // 36], steps[(index // 6) % 6], steps[index % 6]
    level = 8 + (index - 232) * 10
    return level, level, level


def resolve(color: int, default: tuple[int, int, int]) -> tuple[int, int, int]:
    if color == DEFAULT:
        return default
    if color & RGB_FLAG:
        return (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF
    return palette(color)


class Screen:
    """A small VT100/xterm emulator: enough for shells, less, vim, htop and friends."""

    def __init__(self, columns: int, rows: int) -> None:
        self.columns = max(1, columns)
        self.rows = max(1, rows)
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.state = "ground"
        self.params = ""
        self.charset_slot = 0
        self.charsets = ["B", "B"]
        self.shifted = False
        self.alternate = False
        self.saved_main = None
        self.title = ""
        self.reset()

    # -- state ------------------------------------------------------------------------------------------

    def reset(self) -> None:
        self.fg = self.bg = DEFAULT
        self.flags = 0
        self.grid = [self.blank_row() for _ in range(self.rows)]
        self.x = self.y = 0
        self.pending_wrap = False
        self.top, self.bottom = 0, self.rows - 1
        self.cursor_visible = True
        self.autowrap = True
        self.origin = False
        self.insert_mode = False
        self.saved_cursor = (0, 0, DEFAULT, DEFAULT, 0)
        self.tabs = set(range(8, self.columns, 8))
        self.dirty = True

    def blank_row(self) -> list:
        return [[" ", DEFAULT, self.bg, 0] for _ in range(self.columns)]

    def text(self) -> list[str]:
        return ["".join(cell[0] for cell in row).rstrip() for row in self.grid]

    # -- output -----------------------------------------------------------------------------------------

    def feed(self, data: bytes) -> None:
        for char in self.decoder.decode(data):
            self.handle(char)
        self.dirty = True

    def handle(self, char: str) -> None:
        state = self.state
        code = ord(char)
        if state == "ground":
            self.ground(char, code)
        elif state == "esc":
            self.escape(char)
        elif state == "csi":
            if char in "0123456789;:<=>?" or (char in " !\"#$%&'()*+,-./" and code < 0x40):
                self.params += char
            elif code == 0x1B:
                self.state = "esc"
            elif 0x40 <= code <= 0x7E:
                self.state = "ground"
                self.csi(char, self.params)
            elif code < 0x20:
                self.ground(char, code)     # controls inside a sequence still act
            else:
                self.state = "ground"
        elif state == "osc":
            if code == 0x07:
                self.finish_osc()
            elif code == 0x1B:
                self.state = "osc_esc"
            else:
                self.params += char
        elif state == "osc_esc":
            if char == "\\":
                self.finish_osc()
            else:
                self.state = "ground"
        elif state == "ignore":                  # DCS/APC/PM/SOS: skipped up to ST
            if code == 0x07 or code == 0x9C:
                self.state = "ground"
            elif code == 0x1B:
                self.state = "ignore_esc"
        elif state == "ignore_esc":
            self.state = "ground" if char == "\\" else "ignore"
        elif state == "charset":
            self.charsets[self.charset_slot] = char
            self.state = "ground"
        elif state == "hash":
            if char == "8":                      # DECALN: fill with E
                for row in self.grid:
                    for cell in row:
                        cell[0] = "E"
            self.state = "ground"

    def finish_osc(self) -> None:
        number, _, text = self.params.partition(";")
        if number in ("0", "2"):
            self.title = text
        self.state = "ground"

    def ground(self, char: str, code: int) -> None:
        if code == 0x1B:
            self.state = "esc"
        elif code == 0x07:
            pass
        elif code == 0x08:
            self.pending_wrap = False
            self.x = max(0, self.x - 1)
        elif code == 0x09:
            self.pending_wrap = False
            later = [tab for tab in sorted(self.tabs) if tab > self.x]
            self.x = later[0] if later else self.columns - 1
        elif code in (0x0A, 0x0B, 0x0C):
            self.linefeed()
        elif code == 0x0D:
            self.pending_wrap = False
            self.x = 0
        elif code == 0x0E:
            self.shifted = True
        elif code == 0x0F:
            self.shifted = False
        elif code < 0x20 or 0x7F <= code < 0xA0:
            pass
        else:
            if self.charsets[1 if self.shifted else 0] == "0":
                char = LINE_DRAWING.get(char, char)
            self.put(char)

    def put(self, char: str) -> None:
        if self.pending_wrap and self.autowrap:
            self.x = 0
            self.linefeed()
        self.pending_wrap = False
        if self.insert_mode:
            self.insert_chars(1)
        self.grid[self.y][self.x] = [char, self.fg, self.bg, self.flags]
        if self.x == self.columns - 1:
            self.pending_wrap = True
        else:
            self.x += 1

    def linefeed(self) -> None:
        self.pending_wrap = False
        if self.y == self.bottom:
            self.scroll_up(1)
        elif self.y < self.rows - 1:
            self.y += 1

    def reverse_index(self) -> None:
        self.pending_wrap = False
        if self.y == self.top:
            self.scroll_down(1)
        elif self.y > 0:
            self.y -= 1

    def scroll_up(self, count: int, top: int | None = None) -> None:
        top = self.top if top is None else top
        for _ in range(min(count, self.bottom - top + 1)):
            del self.grid[top]
            self.grid.insert(self.bottom, self.blank_row())

    def scroll_down(self, count: int, top: int | None = None) -> None:
        top = self.top if top is None else top
        for _ in range(min(count, self.bottom - top + 1)):
            del self.grid[self.bottom]
            self.grid.insert(top, self.blank_row())

    # -- escapes ----------------------------------------------------------------------------------------

    def escape(self, char: str) -> None:
        self.state = "ground"
        if char == "[":
            self.state, self.params = "csi", ""
        elif char == "]":
            self.state, self.params = "osc", ""
        elif char in "PX^_":
            self.state = "ignore"
        elif char in "()":
            self.state, self.charset_slot = "charset", 0 if char == "(" else 1
        elif char == "#":
            self.state = "hash"
        elif char == "7":
            self.save_cursor()
        elif char == "8":
            self.restore_cursor()
        elif char == "D":
            self.linefeed()
        elif char == "E":
            self.x = 0
            self.linefeed()
        elif char == "M":
            self.reverse_index()
        elif char == "H":
            self.tabs.add(self.x)
        elif char == "c":
            self.reset()
        # "=", ">" (keypad modes) and everything else: ignored

    def save_cursor(self) -> None:
        self.saved_cursor = (self.x, self.y, self.fg, self.bg, self.flags)

    def restore_cursor(self) -> None:
        self.x, self.y, self.fg, self.bg, self.flags = self.saved_cursor
        self.x = min(self.x, self.columns - 1)
        self.y = min(self.y, self.rows - 1)
        self.pending_wrap = False

    @staticmethod
    def numbers(text: str, default: int = 0) -> list[int]:
        values = []
        for part in text.lstrip("<=>?").replace(":", ";").split(";"):
            digits = "".join(c for c in part if c.isdigit())
            values.append(int(digits) if digits else default)
        return values or [default]

    def move(self, x: int | None = None, y: int | None = None) -> None:
        self.pending_wrap = False
        if x is not None:
            self.x = min(max(0, x), self.columns - 1)
        if y is not None:
            low, high = (self.top, self.bottom) if self.origin else (0, self.rows - 1)
            self.y = min(max(low, y), high)

    def csi(self, final: str, raw: str) -> None:
        private = raw[:1] in "<=>?"
        numbers = self.numbers(raw)
        first = numbers[0]
        count = max(1, first)
        if final == "m" and not private:
            self.sgr(numbers)
        elif final == "A":
            self.move(y=max(self.top if self.y >= self.top else 0, self.y - count))
        elif final == "B" or final == "e":
            self.move(y=min(self.bottom if self.y <= self.bottom else self.rows - 1, self.y + count))
        elif final == "C" or final == "a":
            self.move(x=self.x + count)
        elif final == "D":
            self.move(x=self.x - count)
        elif final == "E":
            self.move(x=0, y=self.y + count)
        elif final == "F":
            self.move(x=0, y=self.y - count)
        elif final == "G" or final == "`":
            self.move(x=count - 1)
        elif final in "Hf":
            row = max(1, numbers[0]) - 1
            column = max(1, numbers[1] if len(numbers) > 1 else 1) - 1
            self.move(x=column, y=row + (self.top if self.origin else 0))
        elif final == "d":
            self.move(y=count - 1 + (self.top if self.origin else 0))
        elif final == "J":
            self.erase_display(first)
        elif final == "K":
            self.erase_line(first)
        elif final == "L":
            if self.top <= self.y <= self.bottom:
                self.scroll_down(count, self.y)
        elif final == "M":
            if self.top <= self.y <= self.bottom:
                self.scroll_up(count, self.y)
        elif final == "@":
            self.insert_chars(count)
        elif final == "P":
            self.delete_chars(count)
        elif final == "X":
            for column in range(self.x, min(self.columns, self.x + count)):
                self.grid[self.y][column] = [" ", DEFAULT, self.bg, 0]
        elif final == "S":
            self.scroll_up(count)
        elif final == "T" and not private:
            self.scroll_down(count)
        elif final == "r":
            top = max(1, numbers[0]) - 1
            bottom = (numbers[1] if len(numbers) > 1 and numbers[1] else self.rows) - 1
            if 0 <= top < bottom < self.rows:
                self.top, self.bottom = top, bottom
                self.move(x=0, y=self.top if self.origin else 0)
        elif final == "s" and not private:
            self.save_cursor()
        elif final == "u" and not private:
            self.restore_cursor()
        elif final in "hl":
            self.set_modes(numbers, final == "h", private)
        # device status, window ops and the rest: ignored

    def set_modes(self, numbers: list[int], on: bool, private: bool) -> None:
        for number in numbers:
            if not private:
                if number == 4:
                    self.insert_mode = on
            elif number == 25:
                self.cursor_visible = on
            elif number == 7:
                self.autowrap = on
            elif number == 6:
                self.origin = on
                self.move(x=0, y=self.top if on else 0)
            elif number in (47, 1047, 1049):
                self.set_alternate(on, number == 1049)

    def set_alternate(self, on: bool, save: bool) -> None:
        if on and not self.alternate:
            if save:
                self.save_cursor()
            self.saved_main = (self.grid, self.top, self.bottom)
            self.grid = [self.blank_row() for _ in range(self.rows)]
            self.top, self.bottom = 0, self.rows - 1
            self.alternate = True
        elif not on and self.alternate:
            self.grid, self.top, self.bottom = self.saved_main
            self.saved_main = None
            self.alternate = False
            if save:
                self.restore_cursor()

    def erase_display(self, mode: int) -> None:
        blank = lambda: [" ", DEFAULT, self.bg, 0]  # noqa: E731
        if mode == 0:
            self.erase_line(0)
            for row in range(self.y + 1, self.rows):
                self.grid[row] = [blank() for _ in range(self.columns)]
        elif mode == 1:
            self.erase_line(1)
            for row in range(0, self.y):
                self.grid[row] = [blank() for _ in range(self.columns)]
        elif mode in (2, 3):
            for row in range(self.rows):
                self.grid[row] = [blank() for _ in range(self.columns)]

    def erase_line(self, mode: int) -> None:
        if mode == 0:
            span = range(self.x, self.columns)
        elif mode == 1:
            span = range(0, self.x + 1)
        else:
            span = range(self.columns)
        for column in span:
            self.grid[self.y][column] = [" ", DEFAULT, self.bg, 0]

    def insert_chars(self, count: int) -> None:
        row = self.grid[self.y]
        count = min(count, self.columns - self.x)
        for _ in range(count):
            row.insert(self.x, [" ", DEFAULT, self.bg, 0])
            row.pop()

    def delete_chars(self, count: int) -> None:
        row = self.grid[self.y]
        count = min(count, self.columns - self.x)
        for _ in range(count):
            del row[self.x]
            row.append([" ", DEFAULT, self.bg, 0])

    def sgr(self, numbers: list[int]) -> None:
        index = 0
        while index < len(numbers):
            n = numbers[index]
            if n == 0:
                self.fg = self.bg = DEFAULT
                self.flags = 0
            elif n == 1:
                self.flags |= BOLD
            elif n == 2:
                self.flags |= DIM
            elif n == 3:
                self.flags |= ITALIC
            elif n == 4:
                self.flags |= UNDERLINE
            elif n == 7:
                self.flags |= REVERSE
            elif n == 22:
                self.flags &= ~(BOLD | DIM)
            elif n == 23:
                self.flags &= ~ITALIC
            elif n == 24:
                self.flags &= ~UNDERLINE
            elif n == 27:
                self.flags &= ~REVERSE
            elif 30 <= n <= 37:
                self.fg = n - 30
            elif 40 <= n <= 47:
                self.bg = n - 40
            elif 90 <= n <= 97:
                self.fg = n - 90 + 8
            elif 100 <= n <= 107:
                self.bg = n - 100 + 8
            elif n == 39:
                self.fg = DEFAULT
            elif n == 49:
                self.bg = DEFAULT
            elif n in (38, 48):
                color = None
                if index + 2 < len(numbers) and numbers[index + 1] == 5:
                    color = min(255, numbers[index + 2])
                    index += 2
                elif index + 4 < len(numbers) and numbers[index + 1] == 2:
                    r, g, b = (min(255, v) for v in numbers[index + 2:index + 5])
                    color = RGB_FLAG | (r << 16) | (g << 8) | b
                    index += 4
                if color is not None:
                    if n == 38:
                        self.fg = color
                    else:
                        self.bg = color
            index += 1

    def resize(self, columns: int, rows: int) -> None:
        self.columns, self.rows = max(1, columns), max(1, rows)
        self.reset()


# ── Key packets from the hook ──────────────────────────────────────────────────────────────────────────


class Packets:
    """Reassembles the hook's stdin packets: a type byte, a length byte and the payload."""

    def __init__(self) -> None:
        self.buffer = b""

    def feed(self, data: bytes) -> list[tuple[str, bytes]]:
        self.buffer += data
        out = []
        while len(self.buffer) >= 2:
            length = self.buffer[1]
            if len(self.buffer) < 2 + length:
                break
            out.append((chr(self.buffer[0]), self.buffer[2:2 + length]))
            self.buffer = self.buffer[2 + length:]
        return out


# ── Profiles ───────────────────────────────────────────────────────────────────────────────────────────


def profile_command(name: str, config: Path | None = None) -> list[str] | None:
    """The argv for a terminal profile, or None when the profile is not defined."""
    import shlex
    if name in ("", "default"):
        return [os.environ.get("SHELL") or "/bin/bash", "-i"]
    config = config or Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "t2launch" / "sky_terminals.cfg"
    try:
        for line in config.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            if key.strip() == name:
                return shlex.split(value)
    except OSError:
        pass
    return None


# ── Drawing (Qt, offscreen) ────────────────────────────────────────────────────────────────────────────


class Renderer:
    def __init__(self, width: int, height: int, font_pixels: int) -> None:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        os.environ.setdefault("QT_LOGGING_RULES", "*.debug=false;qt.qpa.*=false")
        from PySide6.QtGui import QFont, QFontDatabase, QFontMetricsF, QGuiApplication, QImage
        self.QImage = QImage
        self.app = QGuiApplication.instance() or QGuiApplication(["sky_terminal"])
        families = set(QFontDatabase.families())
        font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        for family in ("DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "Hack"):
            if family in families:
                font = QFont(family)
                break
        font.setPixelSize(font_pixels)
        font.setStyleStrategy(QFont.StyleStrategy.PreferAntialias)
        font.setHintingPreference(QFont.HintingPreference.PreferFullHinting)
        self.font = font
        metrics = QFontMetricsF(font)
        advance = metrics.horizontalAdvance("M")
        self.cell_w = max(1, round(advance))
        self.cell_h = max(1, int(metrics.height() + 0.999))
        self.ascent = int(metrics.ascent() + 0.5)
        font.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, self.cell_w - advance)
        self.width, self.height = width, height
        self.columns = max(1, width // self.cell_w)
        self.rows = max(1, height // self.cell_h)

    def draw(self, screen: Screen, focused: bool, notice: str = "") -> bytes:
        from PySide6.QtCore import QRect, Qt
        from PySide6.QtGui import QColor, QPainter, QPen
        image = self.QImage(self.width, self.height, self.QImage.Format.Format_RGBA8888)
        image.fill(QColor(*DEFAULT_BG))
        painter = QPainter(image)
        painter.setFont(self.font)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
        cw, ch = self.cell_w, self.cell_h
        for y, row in enumerate(screen.grid[:self.rows]):
            x = 0
            while x < screen.columns and x < self.columns:
                char, fg, bg, flags = row[x]
                end = x + 1
                while end < min(screen.columns, self.columns) and row[end][1:] == [fg, bg, flags]:
                    end += 1
                if flags & REVERSE:
                    fg_rgb, bg_rgb = resolve(bg, DEFAULT_BG), resolve(fg, DEFAULT_FG)
                else:
                    fg_rgb, bg_rgb = resolve(fg, DEFAULT_FG), resolve(bg, DEFAULT_BG)
                if flags & BOLD and not flags & REVERSE and 0 <= fg < 8:
                    fg_rgb = palette(fg + 8)
                if flags & DIM:
                    fg_rgb = tuple(int(c * 0.6) for c in fg_rgb)
                if bg_rgb != DEFAULT_BG:
                    painter.fillRect(QRect(x * cw, y * ch, (end - x) * cw, ch), QColor(*bg_rgb))
                text = "".join(cell[0] for cell in row[x:end])
                if text.strip():
                    painter.setPen(QColor(*fg_rgb))
                    painter.drawText(x * cw, y * ch + self.ascent, text)
                if flags & UNDERLINE:
                    painter.fillRect(QRect(x * cw, y * ch + ch - 2, (end - x) * cw, 1), QColor(*fg_rgb))
                x = end
        if screen.cursor_visible and screen.y < self.rows and screen.x < self.columns:
            painter.fillRect(QRect(screen.x * cw, screen.y * ch, cw, ch), QColor(0xd0, 0xff, 0xd0, 170 if focused else 90))
        if notice:
            painter.setPen(QColor(255, 220, 120))
            painter.drawText(4, self.height - 6, notice)
        if focused:
            pen = QPen(QColor(0x40, 0xe0, 0xff))
            pen.setWidth(4)
            painter.setPen(pen)
            painter.drawRect(2, 2, self.width - 4, self.height - 4)
        painter.end()
        data = bytes(image.constBits())
        return data[:self.width * self.height * 4]


# ── Main loop ──────────────────────────────────────────────────────────────────────────────────────────


def spawn_shell(argv: list[str], columns: int, rows: int) -> tuple[int, int]:
    pid, master = pty.fork()
    if pid == 0:
        try:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
            env = dict(os.environ, TERM="xterm-256color", COLORTERM="truecolor")
            for name in ("LD_PRELOAD", "LD_AUDIT", "LD_LIBRARY_PATH", "QT_QPA_PLATFORM"):
                env.pop(name, None)
            os.execvpe(argv[0], argv, env)
        except OSError:
            pass
        os._exit(127)
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    return pid, master


def tail_start(path: Path, limit: int = 16384) -> tuple[bytes, int]:
    try:
        with path.open("rb") as handle:
            size = handle.seek(0, os.SEEK_END)
            handle.seek(max(0, size - limit))
            data = handle.read()
            if size > limit and b"\n" in data:
                data = data.split(b"\n", 1)[1]
            return data, size
    except OSError:
        return b"", 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--font", type=int, default=16, help="font size in pixels")
    parser.add_argument("--fps", type=int, default=12)
    parser.add_argument("--mode", choices=("shell", "console"), default="shell")
    parser.add_argument("--profile", default="default")
    parser.add_argument("--tail", default="")
    args = parser.parse_args(argv)
    args.width = min(max(args.width, 64), 4096)
    args.height = min(max(args.height, 64), 4096)

    try:
        renderer = Renderer(args.width, args.height, min(max(args.font, 6), 64))
    except Exception as exc:           # Qt missing: say so on the helper's stderr (the hook shows it)
        print(f"sky_terminal: cannot draw text ({exc}); is PySide6 installed?", file=sys.stderr)
        return 2
    screen = Screen(renderer.columns, renderer.rows)
    out = sys.stdout.buffer
    notice = ""
    child = master = None
    log_path = Path(args.tail) if args.tail else None
    log_offset = 0

    if args.mode == "shell":
        command = profile_command(args.profile)
        if command is None:
            notice = f"no terminal profile '{args.profile}' in ~/.config/t2launch/sky_terminals.cfg"
        else:
            try:
                child, master = spawn_shell(command, screen.columns, screen.rows)
            except OSError as exc:
                notice = f"cannot start {command[0]}: {exc}"
    elif log_path:
        data, log_offset = tail_start(log_path)
        screen.feed(data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        if not data:
            screen.feed(b"(waiting for console output)\r\n")

    packets = Packets()
    focused = False
    interval = 1.0 / max(1, args.fps)
    next_frame = 0.0
    stdin_fd = sys.stdin.fileno()
    running = True
    while running:
        watched = [stdin_fd] + ([master] if master is not None else [])
        try:
            ready, _, _ = select.select(watched, [], [], interval / 2)
        except InterruptedError:
            continue
        if stdin_fd in ready:
            data = os.read(stdin_fd, 4096)
            if not data:
                break                                   # the game closed its end: finish
            for kind, payload in packets.feed(data):
                if kind == "k" and master is not None:
                    try:
                        os.write(master, payload)
                    except OSError:
                        pass
                elif kind == "f":
                    focused = payload == b"1"
                    screen.dirty = True
        if master is not None and master in ready:
            try:
                data = os.read(master, 65536)
            except OSError as exc:
                data = b"" if exc.errno == errno.EIO else None
            if data:
                screen.feed(data)
            elif data is not None:
                break                                   # the shell exited
        if log_path and args.mode == "console":
            try:
                size = log_path.stat().st_size
            except OSError:
                size = log_offset
            if size < log_offset:
                log_offset = 0                          # the log was restarted
            if size > log_offset:
                with log_path.open("rb") as handle:
                    handle.seek(log_offset)
                    chunk = handle.read(1 << 16)
                log_offset += len(chunk)
                screen.feed(chunk.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        now = time.monotonic()
        if (screen.dirty or next_frame == 0.0) and now >= next_frame:
            try:
                out.write(renderer.draw(screen, focused, notice))
                out.flush()
            except (BrokenPipeError, OSError):
                break
            screen.dirty = False
            next_frame = now + interval
    if child:
        try:
            os.kill(child, signal.SIGHUP)
            os.waitpid(child, 0)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
