#!/usr/bin/env python3
"""Desktop launcher for the native Tribes 2 Linux client."""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import sys
import tempfile
import time
from pathlib import Path

try:
    from PySide6.QtCore import QProcess, QProcessEnvironment, QTimer, Signal  # noqa: I001 (grid-wrapped names)
    from PySide6.QtGui import QAction
    from PySide6.QtNetwork import QLocalServer, QLocalSocket
    from PySide6.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox,
        QFileDialog, QFormLayout, QGridLayout, QHBoxLayout, QLabel, QLineEdit, QListWidget,
        QMenu, QMessageBox, QPushButton, QScrollArea, QSpinBox,
        QStyle, QSystemTrayIcon, QTabWidget, QTextEdit, QVBoxLayout, QWidget,
    )
except ImportError as exc:
    raise SystemExit("PySide6 is required. Install it with: python3 -m pip install -r requirements.txt") from exc

from shiboken6 import isValid as is_qobject_valid

import t2options

ROOT = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "t2launch"
CONFIG_FILE = CONFIG_DIR / "settings.json"
SENTRY_SESSION_OVERRIDE_KEYS = frozenset({
    "hook.console_commands",
    "video.full_redraw",
    "telemetry.shazbot",
    "telemetry.shazbot_socket",
    "telemetry.shazbot_packet_capture",
    "telemetry.shazbot_notification_socket",
    "telemetry.shazbot_actions",
    "telemetry.shazbot_command_labels",
    "telemetry.shazbot_auto_weapon_slot_test",
    "telemetry.shazbot_weapon_slot_test_script",
})
SENTRY_DEMO_RECORDING_RE = re.compile(r"recordings/[A-Za-z0-9][A-Za-z0-9_.-]*\.rec")
SENTRY_CONTROL_API_FEATURES = ["session_demo_recording"]


def normalize_sentry_demo_recording(value):
    """Accept only a simple demo filename under Torque's recordings directory."""
    if value in (None, ""):
        return None
    if not isinstance(value, str) or not SENTRY_DEMO_RECORDING_RE.fullmatch(value):
        raise ValueError("demo_recording must be recordings/<filename>.rec")
    return value


def configure_sentry_demo_environment(environment, demo_recording):
    """Remove inherited autoplay state, then apply this capture's explicit demo."""
    environment.remove("T2LAUNCH_AUTOPLAY_DEMO")
    if demo_recording:
        environment.insert("T2LAUNCH_AUTOPLAY_DEMO", demo_recording)
    return environment


def sentry_control_socket_path() -> Path:
    """Per-user local control endpoint for Sentry; never carries credentials."""
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    directory = Path(runtime) if runtime and Path(runtime).is_dir() else Path(tempfile.gettempdir())
    uid = os.getuid() if hasattr(os, "getuid") else os.environ.get("USER", "user")
    return directory / f"t2launch-sentry-{uid}.sock"

# Tribes 2 command-line options, as console_start.cs parses them. Each argument
# that takes values has an enable checkbox and one field per value, hinted with
# what that value is. Advanced options stay hidden unless asked for or set.
BOOL_OPTIONS = [  # (flag, label, advanced)
    ("-dedicated", "Dedicated server", False), ("-nonpure", "Non-pure server", True),
    ("-host", "Host game", False), ("-demo", "Demo mode", True), ("-show", "TSShow", True),
    ("-con", "Console mode", True), ("-prepbuild", "Prepare/build scripts", True),
    ("-quit", "Quit after startup", True), ("-nologin", "Offline (skip login)", False),
    ("-online", "Launcher / TribesNext mode", False),
    ("--nosound", "Disable sound", False), ("--fullscreen", "Fullscreen", False),
    ("--windowed", "Windowed", False),
    ("-s", "Disable sound (short form)", True), ("-f", "Fullscreen (short form)", True),
    ("-w", "Windowed (short form)", True),
]
VALUE_OPTIONS = [  # (option, label, a hint per value, advanced)
    ("-mod", "Mod", ("mod name, e.g. Classic",), False),
    ("-clientprefs", "Client prefs file", ("prefs file, e.g. prefs/ClientPrefs.cs",), True),
    ("-serverprefs", "Server prefs file", ("prefs file, e.g. prefs/ServerPrefs.cs",), True),
    ("-mission", "Mission", ("mission name", "mission type, e.g. CTF"), False),
    ("-telnetParams", "Telnet parameters", ("port", "password", "listen-only password"), True),
    ("-connect", "Connect address", ("server address, e.g. 1.2.3.4:28000",), False),
    ("-password", "Server password", ("server password",), False),
    ("-jload", "Load journal", ("journal file",), True),
    ("-jsave", "Save journal", ("journal file",), True),
    ("-jplay", "Play journal", ("journal file",), True),
    ("-navBuild", "Navigation build", ("mission name", "mission type"), True),
    ("-spnBuild", "SPN build", ("mission name", "mission type"), True),
    ("-login", "Login credentials", ("login name", "password"), False),
    ("-bot", "Bot count", ("number of bots",), False),
    ("-light", "Light mission", ("mission name",), True),
    ("--gllibrary", "OpenGL library", ("library path, e.g. libGL.so.1",), True),
    ("-g", "OpenGL library (short form)", ("library path, e.g. libGL.so.1",), True),
]


def default_profile():
    return {"name": "Online", "flags": {"-online": True}, "values": {}, "positional": "", "custom": "",
            "load_hook": True}


def server_profile():
    """A dedicated server on Katabatic CTF with local WON emulation."""
    return {"name": "Dedicated server", "flags": {}, "values": {"-mission": ["Katabatic", "CTF"]},
            "positional": "", "custom": "", "load_hook": True,
            "options": {"game.executable": "dedicated", "won.mode": "local"}}


def read_settings():
    try:
        settings = json.loads(CONFIG_FILE.read_text())
    except (OSError, ValueError):
        settings = {}
    profiles = settings.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        profiles = [default_profile()]
    # load_hook used to be a single launcher-wide toggle; it is now per profile.
    # Seed any profile that predates the move from the old global value.
    legacy_hook = bool(settings.get("load_hook", True))
    for profile in profiles:
        if isinstance(profile, dict) and "load_hook" not in profile:
            profile["load_hook"] = legacy_hook
    return {
        "game_directory": settings.get("game_directory", str(Path.home() / "t2-linux")),
        "profiles": profiles,
        "load_hook": legacy_hook,  # default for new profiles only
        "monitor": settings.get("monitor", ""),
        "autolaunch_profile": settings.get("autolaunch_profile", ""),
        "autolaunch_delay": int(settings.get("autolaunch_delay", 10)),
    }


def write_settings(settings):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_DIR.chmod(0o700)
    temporary = CONFIG_FILE.with_suffix(".tmp")
    temporary.write_text(json.dumps(settings, indent=2) + "\n")
    temporary.chmod(0o600)
    temporary.replace(CONFIG_FILE)


def launch_diagnostics_path():
    state_dir = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "t2launch"
    state_dir.mkdir(parents=True, exist_ok=True)
    state_dir.chmod(0o700)
    return state_dir / "launch-diagnostics.log"


def append_launch_diagnostic(message):
    message = redact_launch_secrets(str(message), ())
    path = launch_diagnostics_path()
    if path.exists() and path.stat().st_size > 2 * 1024 * 1024:
        previous = path.with_name(path.name + ".1")
        previous.unlink(missing_ok=True)
        path.replace(previous)
        previous.chmod(0o600)
    fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
    with os.fdopen(fd, "a", encoding="utf-8") as stream:
        stream.write(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} {message}\n")
    path.chmod(0o600)


def redact_launch_secrets(text, secrets_to_redact):
    for secret in secrets_to_redact:
        if secret:
            text = text.replace(secret, "[redacted]")
    argument = r'(?:"[^"]*"|\'[^\']*\'|[^\s,;]+)'
    text = re.sub(
        rf'(?i)(?<!\S)(--?login)(\s+)({argument})(\s+)({argument})',
        r'\1\2[redacted]\4[redacted]', text,
    )
    text = re.sub(
        rf'(?i)(?<!\S)(--?password)(\s+)({argument})',
        r'\1\2[redacted]', text,
    )
    text = re.sub(
        rf'(?i)(?<!\S)(-telnetParams)(\s+)({argument})(\s+)({argument})(\s+)({argument})',
        r'\1\2\3\4[redacted]\6[redacted]', text,
    )
    text = re.sub(
        r'(?i)(\b(?:username|user|login|password|passwd)\b\s*[:=]\s*)(?:"[^"]*"|\'[^\']*\'|[^,\r\n]+)',
        r'\1[redacted]', text,
    )
    return text


def profile_arguments(profile):
    args = []
    flags = profile.get("flags", {})
    for flag, _label, _advanced in BOOL_OPTIONS:
        if flags.get(flag):
            args.append(flag)
    for option, _label, hints, _advanced in VALUE_OPTIONS:
        count = len(hints)
        values = profile.get("values", {}).get(option, [])
        if values:
            if len(values) < count or any(not value.strip() for value in values[:count]):
                raise ValueError(f"{option} needs {count} value(s).")
            args.append(option)
            args.extend(value for value in values[:count])
    positional = profile.get("positional", "").strip()
    if positional:
        args.extend(shlex.split(positional))
    custom = profile.get("custom", "").strip()
    if custom:
        args.extend(shlex.split(custom))
    # The game processes these as competing video settings; windowed wins only
    # when explicitly selected without fullscreen.
    if any(option in args for option in ("--fullscreen", "-f")) and any(option in args for option in ("--windowed", "-w")):
        raise ValueError("Choose either Fullscreen or Windowed, not both.")
    return args


# Terminal emulators and how each takes the command to run: $TERMINAL first,
# then common ones. gnome-terminal hands the window to a server process that
# does not inherit our environment, so the command carries its own.
TERMINALS = (("x-terminal-emulator", ["-e"]), ("konsole", ["-e"]), ("gnome-terminal", ["--"]),
             ("xfce4-terminal", ["-x"]), ("alacritty", ["-e"]), ("kitty", []), ("foot", []),
             ("xterm", ["-e"]))
# Keep the window open after the server stops so its last messages stay readable.
HOLD_OPEN = ('"$@"; status=$?; echo; read -r -p "The server stopped (exit $status). '
             'Press Enter to close. " _')


def terminal_command(command, environment):
    """The argv that runs command in a terminal window with environment, or None."""
    wrapped = ["env", *(f"{key}={value}" for key, value in environment.items()),
               "bash", "-c", HOLD_OPEN, "t2launch-server", *command]
    configured = os.environ.get("TERMINAL", "").strip()
    if configured and shutil.which(shlex.split(configured)[0]):
        return [*shlex.split(configured), "-e", *wrapped]
    for name, prefix in TERMINALS:
        if shutil.which(name):
            return [name, *prefix, *wrapped]
    return None


def sensitive_profile_arguments(args):
    sensitive = []
    option_positions = {
        "-login": (0, 1), "--login": (0, 1),
        "-password": (0,), "--password": (0,),
        "-telnetParams": (1, 2),
    }
    index = 0
    while index < len(args):
        positions = option_positions.get(args[index])
        if positions is None:
            index += 1
            continue
        arity = max(positions) + 1
        values = args[index + 1:index + 1 + arity]
        sensitive.extend(values[position] for position in positions if position < len(values))
        index += arity + 1
    return tuple(value for value in sensitive if value)


def is_safe_launch_diagnostic(line):
    lower = line.casefold()
    return (line.startswith(("[wonemu]", "[t2_hook]", "[t2launch]", "ERROR:", "WARNING:",
                             "OpenGL", "Activating the OpenGL", "Setting video mode",
                             "RSA proxy")) or
            any(marker in lower for marker in
                ("segmentation fault", "fatal error", "xio:", "core dumped",
                 "syntax error", "unable to find function")))


class EndpointsEdit(QLineEdit):
    """One line of host:port entries for the hook's WON bootstrap slots."""
    changed = Signal()

    def __init__(self, value, placeholder=""):
        super().__init__(", ".join(value or ()))
        self.setPlaceholderText(placeholder)
        self.textChanged.connect(self.changed)

    def value(self):
        return tuple(item.strip() for item in self.text().split(",") if item.strip())


class BackendsEdit(QWidget):
    """Backends in priority order, with a checkbox (and address) per service."""
    changed = Signal()

    def __init__(self, value):
        super().__init__()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.grid = QGridLayout()
        layout.addLayout(self.grid)
        add = QPushButton("Add Torque master")
        add.setToolTip("Another Torque-protocol master server (host:port), merged into the server list.")
        add.clicked.connect(self.add_torque)
        layout.addWidget(add)
        self.backends = [dict(backend) for backend in value or ()]
        self.render()

    def add_torque(self):
        self.capture()
        self.backends.append({"kind": "torque", "master": ""})
        self.render()
        self.changed.emit()

    def move(self, index, offset):
        self.capture()
        target = index + offset
        if 0 <= target < len(self.backends):
            self.backends[index], self.backends[target] = self.backends[target], self.backends[index]
            self.render()
            self.changed.emit()

    def remove(self, index):
        self.capture()
        del self.backends[index]
        self.render()
        self.changed.emit()

    def render(self):
        while self.grid.count():
            item = self.grid.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        for column, title in enumerate(("Backend", "Auth", "Master", "IRC", "")):
            self.grid.addWidget(QLabel(f"<b>{title}</b>"), 0, column)
        self.cells = []
        for row, backend in enumerate(self.backends, start=1):
            kind = backend["kind"]
            self.grid.addWidget(QLabel(t2options.BACKEND_LABELS.get(kind, kind)), row, 0)
            cells = {}
            for column, service in enumerate(t2options.BACKEND_SERVICES, start=1):
                if service not in t2options.BACKEND_KINDS[kind]:
                    self.grid.addWidget(QLabel("—"), row, column)
                    continue
                cell = QWidget()
                line = QHBoxLayout(cell)
                line.setContentsMargins(0, 0, 0, 0)
                box = QCheckBox()
                value = backend.get(service, False)
                box.setChecked(bool(value) or (kind == "torque" and service == "master"))
                line.addWidget(box)
                field = None
                if t2options.BACKEND_KINDS[kind][service] is not None:
                    default = t2options.BACKEND_DEFAULT_ADDRESSES.get((kind, service), "")
                    field = QLineEdit(value if isinstance(value, str) else default)
                    chat = t2options.BACKEND_KINDS[kind][service] == "chat"
                    field.setPlaceholderText("chat server host:port (empty: local chat)" if chat
                                             else default or "host:port")
                    field.textChanged.connect(self.changed)
                    line.addWidget(field, 1)
                box.toggled.connect(self.changed)
                if field is not None:
                    box.toggled.connect(field.setEnabled)
                    field.setEnabled(box.isChecked())
                cells[service] = (box, field)
                self.grid.addWidget(cell, row, column)
            buttons = QWidget()
            controls = QHBoxLayout(buttons)
            controls.setContentsMargins(0, 0, 0, 0)
            for text, handler in (("↑", lambda _c=False, i=row - 1: self.move(i, -1)),
                                  ("↓", lambda _c=False, i=row - 1: self.move(i, 1))):
                button = QPushButton(text)
                button.setFixedWidth(28)
                button.clicked.connect(handler)
                controls.addWidget(button)
            if kind == "torque":
                remove = QPushButton("Remove")
                remove.clicked.connect(lambda _c=False, i=row - 1: self.remove(i))
                controls.addWidget(remove)
            self.grid.addWidget(buttons, row, 4)
            self.cells.append(cells)
        self.grid.setColumnStretch(2, 1)
        self.grid.setColumnStretch(3, 1)

    def capture(self):
        """Read the widgets back into self.backends."""
        for backend, cells in zip(self.backends, self.cells):
            for service, (box, field) in cells.items():
                if not box.isChecked():
                    backend[service] = False
                else:
                    # An empty address still means checked: the default URL,
                    # or for TribesNext IRC, community data without a chat relay.
                    backend[service] = (field.text().strip() if field is not None else "") or True

    def value(self):
        self.capture()
        return tuple(dict(backend) for backend in self.backends)


def requirement_text(key, allowed):
    """Describe one unmet option requirement for the profile editor."""
    if key == "launcher.load_hook":
        return "Load t2_hook.so (Settings)"
    option = t2options.BY_KEY[key]
    if option.kind == "bool":
        return option.label
    return f"{option.label}: {' or '.join(str(value) for value in allowed)}"


class ProfileEditor(QWidget):
    def __init__(self, profile, changed, context):
        super().__init__()
        self.profile = profile
        self.changed = changed
        self.context = context  # callable returning launcher-wide values, e.g. load_hook
        layout = QVBoxLayout(self)
        self.name = QLineEdit(profile.get("name", "New profile"))
        layout.addWidget(QLabel("Profile name"))
        layout.addWidget(self.name)
        # Per profile: whether to LD_PRELOAD the hook. Options that need the hook
        # require it against this box, so a profile with the hook off is valid as
        # long as it enables nothing that depends on it.
        self.load_hook = QCheckBox("Load t2_hook.so (LD_PRELOAD) for this profile")
        self.load_hook.setChecked(bool(profile.get("load_hook", True)))
        layout.addWidget(self.load_hook)
        tabs = QTabWidget()
        layout.addWidget(tabs, 1)

        # The Game tab starts with the registry's game options (the executable);
        # the WON emulator and Hook tabs are generated from it below.
        saved_options = t2options.chosen(t2options.migrate(profile))
        self.option_widgets = {}
        self.option_rows = {}
        self.option_default_buttons = {}
        self.section_headings = []  # (heading, keys of its options)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        body = QWidget()
        form = QFormLayout(body)
        self.game_form = form
        for option in t2options.OPTIONS:
            if option.tab == "game":
                form.addRow(self._option_row(option, saved_options[option.key]))
        self.advanced_game_rows = []  # (row widget, is-set check)
        self.flag_boxes = {}
        saved_flags = profile.get("flags", {})
        for flag, label, advanced in BOOL_OPTIONS:
            checkbox = QCheckBox(f"{label} ({flag})")
            checkbox.setChecked(bool(saved_flags.get(flag, False)))
            self.flag_boxes[flag] = checkbox
            form.addRow(checkbox)
            if advanced:
                self.advanced_game_rows.append((checkbox, checkbox.isChecked))
        self.value_boxes = {}
        saved_values = profile.get("values", {})
        for option, label, hints, advanced in VALUE_OPTIONS:
            container = QWidget()
            row = QHBoxLayout(container)
            row.setContentsMargins(0, 0, 0, 0)
            enabled = QCheckBox(f"{label} ({option})")
            fields = []
            old = saved_values.get(option, [""] * len(hints))
            for index, hint in enumerate(hints):
                field = QLineEdit(old[index] if index < len(old) else "")
                field.setPlaceholderText(hint)
                row.addWidget(field)
                fields.append(field)
            enabled.setChecked(bool(old and old[0]))
            row.insertWidget(0, enabled)
            self.value_boxes[option] = (enabled, fields)
            form.addRow(container)
            if advanced:
                self.advanced_game_rows.append((container, enabled.isChecked))
        self.positional = QLineEdit(profile.get("positional", ""))
        self.positional.setPlaceholderText("Optional .dif interior file")
        form.addRow("Interior file", self.positional)
        self.advanced_game_rows.append((self.positional, lambda: bool(self.positional.text().strip())))
        self.custom = QLineEdit(profile.get("custom", ""))
        self.custom.setPlaceholderText("Additional arguments, e.g. -mod Classic -nologin")
        form.addRow("Custom options", self.custom)
        scroll.setWidget(body)
        tabs.addTab(scroll, "Game")

        for tab, title in (("won", "WON emulator"), ("hook", "Hook")):
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            body = QWidget()
            column = QVBoxLayout(body)
            section = None
            for option in t2options.OPTIONS:
                if option.tab != tab:
                    continue
                if option.section != section:
                    section = option.section
                    heading = QLabel(f"<b>{section}</b>")
                    column.addWidget(heading)
                    self.section_headings.append((heading, []))
                self.section_headings[-1][1].append(option.key)
                column.addWidget(self._option_row(option, saved_options[option.key]))
            column.addStretch()
            scroll.setWidget(body)
            tabs.addTab(scroll, title)
        self.show_advanced = QCheckBox("Show advanced options")
        layout.addWidget(self.show_advanced)

        self.name.textChanged.connect(self.changed)
        for box in self.flag_boxes.values():
            box.stateChanged.connect(self.changed)
        for enabled, fields in self.value_boxes.values():
            enabled.stateChanged.connect(self.changed)
            for field in fields:
                field.textChanged.connect(self.changed)
        self.positional.textChanged.connect(self.changed)
        self.custom.textChanged.connect(self.changed)
        for key, widget in self.option_widgets.items():
            signal = {"bool": "toggled", "choice": "currentTextChanged", "int": "valueChanged",
                      "float": "valueChanged", "endpoints": "changed",
                      "backends": "changed"}.get(t2options.BY_KEY[key].kind, "textChanged")
            getattr(widget, signal).connect(self.refresh_options)
        self.show_advanced.toggled.connect(self.refresh_options)
        self.show_advanced.toggled.connect(self.refresh_game_rows)
        self.load_hook.toggled.connect(self.refresh_options)
        self.load_hook.toggled.connect(self.changed)
        self.refresh_options()
        self.refresh_game_rows()

    def hook_context(self):
        """This profile's requirement context (its own load_hook choice)."""
        return {**self.context(), "launcher.load_hook": self.load_hook.isChecked()}

    def refresh_game_rows(self, *_args):
        """Show advanced Game options when asked, and any that are already set."""
        show_advanced = self.show_advanced.isChecked()
        for row, is_set in self.advanced_game_rows:
            self.game_form.setRowVisible(row, show_advanced or is_set())

    def _option_row(self, option, value):
        """A registry option's widget with its label and requirement note."""
        row = QWidget()
        row_layout = QVBoxLayout(row)
        row_layout.setContentsMargins(12, 0, 0, 4)
        widget = self._option_widget(option, value)
        widget.setToolTip(option.help)
        line = QHBoxLayout()
        if option.kind == "bool":
            line.addWidget(widget, 1)
        else:
            line.addWidget(QLabel(option.label))
            line.addWidget(widget, 1)
        reset = QPushButton("Default")
        reset.setFixedWidth(70)
        reset.setToolTip(f"Reset {option.label} to its declared default")
        reset.clicked.connect(lambda _checked=False, key=option.key: self.reset_option(key))
        line.addWidget(reset)
        row_layout.addLayout(line)
        note = QLabel()
        note.setStyleSheet("color: gray")
        note.setWordWrap(True)
        row_layout.addWidget(note)
        self.option_widgets[option.key] = widget
        self.option_default_buttons[option.key] = reset
        self.option_rows[option.key] = (row, note)
        return row

    @staticmethod
    def _option_widget(option, value):
        if option.kind == "bool":
            widget = QCheckBox(option.label)
            widget.setChecked(bool(value))
        elif option.kind == "endpoints":
            widget = EndpointsEdit(value, "automatic in local mode: listen address:port "
                                          "(optional fallbacks after commas)")
        elif option.kind == "backends":
            widget = BackendsEdit(value)
        elif option.kind == "choice":
            widget = QComboBox()
            widget.addItems(list(option.choices))
            widget.setCurrentText(str(value))
        elif option.kind == "int":
            widget = QSpinBox()
            widget.setRange(int(option.minimum if option.minimum is not None else -10**9),
                            int(option.maximum if option.maximum is not None else 10**9))
            widget.setValue(int(value))
        elif option.kind == "float":
            widget = QDoubleSpinBox()
            widget.setRange(option.minimum if option.minimum is not None else -1e9,
                            option.maximum if option.maximum is not None else 1e9)
            widget.setDecimals(3)
            widget.setValue(float(value))
        else:
            widget = QLineEdit(str(value))
        return widget

    def reset_option(self, key):
        """Restore one profile option to its registry default."""
        option = t2options.BY_KEY[key]
        widget = self.option_widgets[key]
        value = t2options.reset_option(self.option_values(), key)[key]
        if option.kind == "bool":
            widget.setChecked(bool(value))
        elif option.kind == "choice":
            widget.setCurrentText(str(value))
        elif option.kind in ("int", "float"):
            widget.setValue(value)
        elif option.kind == "endpoints":
            widget.setText(", ".join(value or ()))
        elif option.kind == "backends":
            widget.backends = [dict(backend) for backend in value]
            widget.render()
            widget.changed.emit()
        else:
            widget.setText(str(value))

    def option_values(self):
        values = {}
        for key, widget in self.option_widgets.items():
            kind = t2options.BY_KEY[key].kind
            if kind == "bool":
                values[key] = widget.isChecked()
            elif kind == "choice":
                values[key] = widget.currentText()
            elif kind in ("int", "float", "endpoints", "backends"):
                values[key] = widget.value()
            else:
                values[key] = widget.text()
        return values

    def refresh_options(self, *_args):
        """Enable each option only while its requirements hold."""
        raw_values = self.option_values()
        values = t2options.chosen(raw_values)
        context = self.hook_context()
        effective = t2options.effective(values, context)
        show_advanced = self.show_advanced.isChecked()
        for option in t2options.OPTIONS:
            row, note = self.option_rows[option.key]
            # Requirements are judged against effective values, so a chain
            # (TribesNext -> console commands -> hook) disables in one step.
            missing = t2options.unmet(option, effective, context)
            # Options of the other executable are hidden rather than greyed.
            other_executable = any(key == "game.executable" for key, _allowed in missing)
            row.setVisible((show_advanced or not option.advanced) and not other_executable)
            self.option_widgets[option.key].setEnabled(not missing)
            try:
                is_default = t2options.coerce(option, raw_values[option.key]) == option.default
            except (TypeError, ValueError):
                is_default = False
            self.option_default_buttons[option.key].setVisible(not is_default)
            self.option_default_buttons[option.key].setEnabled(not missing)
            note.setText("Requires: " + ", ".join(requirement_text(key, allowed)
                                                  for key, allowed in missing) if missing else "")
            note.setVisible(bool(missing))
        for heading, keys in self.section_headings:
            heading.setVisible(any(not self.option_rows[key][0].isHidden() for key in keys))
        self.changed()

    def save_to(self):
        result = {
            "name": self.name.text().strip() or "Profile",
            "load_hook": self.load_hook.isChecked(),
            "flags": {flag: box.isChecked() for flag, box in self.flag_boxes.items()},
            "values": {}, "positional": self.positional.text(), "custom": self.custom.text(),
            # Store every choice, including disabled ones, so re-enabling a
            # dependency restores what the user picked.
            "options": {key: list(value) if isinstance(value, tuple) else value
                        for key, value in self.option_values().items()},
        }
        for option, (enabled, fields) in self.value_boxes.items():
            values = [field.text() for field in fields]
            if enabled.isChecked():
                result["values"][option] = values
        return result


class SettingsDialog(QDialog):
    def __init__(self, app):
        super().__init__()
        self.app = app
        self.setWindowTitle("T2Launch Settings")
        self.resize(900, 700)
        root_layout = QVBoxLayout(self)
        general = QWidget()
        general_form = QFormLayout(general)
        path_row = QHBoxLayout()
        self.game_path = QLineEdit(app.settings["game_directory"])
        browse = QPushButton("Browse…")
        browse.clicked.connect(self.pick_game)
        path_row.addWidget(self.game_path)
        path_row.addWidget(browse)
        general_form.addRow("Game folder", path_row)
        self.monitor = QComboBox()
        self.monitor.addItem("Default display", "")
        for screen in QApplication.screens():
            self.monitor.addItem(screen.name(), screen.name())
        selected = self.monitor.findData(app.settings.get("monitor", ""))
        if selected >= 0:
            self.monitor.setCurrentIndex(selected)
        general_form.addRow("Fullscreen display", self.monitor)
        self.autolaunch = QComboBox()
        for profile in sorted(app.settings["profiles"], key=lambda p: p.get("name", "").casefold()):
            self.autolaunch.addItem(profile.get("name", "Profile"), profile.get("name", "Profile"))
        self.autolaunch.insertItem(0, "Disabled", "")
        selected = self.autolaunch.findData(app.settings.get("autolaunch_profile", ""))
        self.autolaunch.setCurrentIndex(max(selected, 0))
        general_form.addRow("Autolaunch profile", self.autolaunch)
        self.delay = QSpinBox()
        self.delay.setRange(0, 3600)
        self.delay.setSuffix(" seconds")
        self.delay.setValue(app.settings.get("autolaunch_delay", 10))
        general_form.addRow("Autolaunch delay", self.delay)
        build_row = QHBoxLayout()
        self.build_status = QLabel("Compile the hook, RSA helper, and stack-realignment shim.")
        self.build_button = QPushButton("Rebuild")
        self.build_button.clicked.connect(app.build_support)
        build_row.addWidget(self.build_status, 1)
        build_row.addWidget(self.build_button)
        general_form.addRow("Support binaries", build_row)
        root_layout.addWidget(general)

        profile_group = QHBoxLayout()
        self.profile_list = QListWidget()
        self.profile_list.setMinimumWidth(150)
        self.profiles = [dict(profile) for profile in app.settings["profiles"]]
        self.profile_editor = None
        self.editor_row = -1
        for profile in self.profiles:
            self.profile_list.addItem(profile.get("name", "Profile"))
        self.profile_list.currentRowChanged.connect(self.show_profile)
        add = QPushButton("Add profile")
        add_server = QPushButton("Add server profile")
        remove = QPushButton("Remove profile")
        add.clicked.connect(self.add_profile)
        add_server.clicked.connect(self.add_server_profile)
        remove.clicked.connect(self.remove_profile)
        profile_buttons = QHBoxLayout()
        profile_buttons.addWidget(add)
        profile_buttons.addWidget(add_server)
        profile_buttons.addWidget(remove)
        profile_column = QVBoxLayout()
        profile_column.addWidget(self.profile_list, 1)
        profile_column.addLayout(profile_buttons)
        self.profile_widget = QWidget()
        self.profile_widget_layout = QVBoxLayout(self.profile_widget)
        profile_group.addLayout(profile_column)
        profile_group.addWidget(self.profile_widget, 1)
        root_layout.addLayout(profile_group, 1)

        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        root_layout.addWidget(buttons)
        self.profile_list.setCurrentRow(0)

    def pick_game(self):
        path = QFileDialog.getExistingDirectory(self, "Select Tribes 2 game folder", self.game_path.text())
        if path:
            self.game_path.setText(path)

    def commit_editor(self):
        row = self.editor_row
        if self.profile_editor and 0 <= row < len(self.profiles):
            self.profiles[row] = self.profile_editor.save_to()

    def show_profile(self, row):
        show_advanced = False
        if self.profile_editor:
            show_advanced = self.profile_editor.show_advanced.isChecked()
            self.commit_editor()
            self.profile_editor.deleteLater()
        while self.profile_widget_layout.count():
            item = self.profile_widget_layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        self.profile_editor = None
        self.editor_row = row
        if 0 <= row < len(self.profiles):
            self.profile_editor = ProfileEditor(
                self.profiles[row], self.update_profile_name, lambda: {})
            # Keep "Show advanced options" as it was when switching profiles.
            self.profile_editor.show_advanced.setChecked(show_advanced)
            self.profile_widget_layout.addWidget(self.profile_editor)

    def update_profile_name(self):
        if self.profile_editor:
            row = self.profile_list.currentRow()
            if row >= 0:
                name = self.profile_editor.name.text().strip() or "Profile"
                old_name = self.profiles[row].get("name", "Profile")
                self.profile_list.item(row).setText(name)
                self.profiles[row]["name"] = name
                selected = self.autolaunch.currentData()
                self.refresh_autolaunch_choices(name if selected == old_name else selected)

    def refresh_autolaunch_choices(self, selected=None):
        if not hasattr(self, "autolaunch"):
            return
        if selected is None:
            selected = self.autolaunch.currentData()
        names = sorted({profile.get("name", "Profile") for profile in self.profiles}, key=str.casefold)
        self.autolaunch.blockSignals(True)
        self.autolaunch.clear()
        self.autolaunch.addItem("Disabled", "")
        for name in names:
            self.autolaunch.addItem(name, name)
        index = self.autolaunch.findData(selected)
        self.autolaunch.setCurrentIndex(max(index, 0))
        self.autolaunch.blockSignals(False)

    def add_profile(self, profile=None):
        self.commit_editor()
        profile = profile or {"name": "New profile", "flags": {}, "values": {}, "positional": "",
                              "custom": "", "load_hook": bool(self.app.settings.get("load_hook", True))}
        self.profiles.append(profile)
        self.profile_list.addItem(profile["name"])
        self.profile_list.setCurrentRow(len(self.profiles) - 1)
        self.refresh_autolaunch_choices()

    def add_server_profile(self):
        self.add_profile(server_profile())

    def remove_profile(self):
        if len(self.profiles) <= 1:
            QMessageBox.information(self, "Profiles", "Keep at least one profile.")
            return
        row = self.profile_list.currentRow()
        if row >= 0:
            self.commit_editor()
            if self.profile_editor:
                self.profile_editor.deleteLater()
            self.profile_editor = None
            self.editor_row = -1
            self.profiles.pop(row)
            self.profile_list.blockSignals(True)
            self.profile_list.takeItem(row)
            new_row = min(row, len(self.profiles) - 1)
            self.profile_list.setCurrentRow(new_row)
            self.profile_list.blockSignals(False)
            self.show_profile(new_row)
            self.refresh_autolaunch_choices()

    def save(self):
        try:
            self.commit_editor()
            for profile in self.profiles:
                profile_arguments(profile)
            for profile in self.profiles:
                hooked = bool(profile.get("load_hook", True))
                try:
                    t2options.validate(t2options.migrate(profile),
                                       {"launcher.load_hook": hooked})
                    if t2options.migrate(profile)["won.mode"] != "off" and not hooked:
                        raise ValueError("Local WON emulation requires Load t2_hook.so for this profile.")
                except ValueError as exc:
                    raise ValueError(f"{profile.get('name', 'Profile')}: {exc}") from exc
            names = [p["name"].strip() for p in self.profiles]
            if len({name.casefold() for name in names}) != len(names):
                raise ValueError("Profile names must be unique.")
            selected_auto = self.autolaunch.currentData()
            if selected_auto and selected_auto not in names:
                selected_auto = ""
            self.app.settings.update({
                "game_directory": self.game_path.text().strip(),
                "profiles": sorted(self.profiles, key=lambda p: p["name"].casefold()),
                "monitor": self.monitor.currentData() or "",
                "autolaunch_profile": selected_auto,
                "autolaunch_delay": self.delay.value(),
            })
            write_settings(self.app.settings)
            self.app.refresh_menu()
            self.accept()
        except (ValueError, OSError) as exc:
            QMessageBox.warning(self, "Cannot save settings", str(exc))


class Launcher(QApplication):
    def __init__(self, argv):
        super().__init__(argv)
        self.setApplicationName("T2Launch")
        self.setQuitOnLastWindowClosed(False)
        self.settings = read_settings()
        if not self.settings.get("monitor") and QApplication.primaryScreen():
            self.settings["monitor"] = QApplication.primaryScreen().name()
        self.processes = {}
        self.process_metadata = {}
        self.launch_output_states = {}
        self._sentry_buffers = {}
        self.sentry_control_server = None
        self.sentry_control_path = None
        self.build_process = None
        self.build_dialog = None
        self.settings_dialog = None
        self.tray = QSystemTrayIcon(self)
        self.tray.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_ComputerIcon))
        self.tray.setToolTip("T2Launch — no running instances")
        self.menu = QMenu()
        self.tray.setContextMenu(self.menu)
        self.tray.activated.connect(self.tray_activated)
        self.tray.show()
        self._start_sentry_control_server()
        self.refresh_menu()
        self.poll = QTimer(self)
        self.poll.timeout.connect(self.update_tooltip)
        self.poll.start(1500)
        self.autolaunch_timer = QTimer(self)
        self.autolaunch_timer.setSingleShot(True)
        profile_name = self.settings.get("autolaunch_profile")
        if profile_name:
            delay = max(0, self.settings.get("autolaunch_delay", 10))
            self.tray.showMessage("T2Launch", f"Launching {profile_name} in {delay} seconds. Open Settings to cancel.", QSystemTrayIcon.Information, min(delay, 10) * 1000)
            self.autolaunch_timer.timeout.connect(lambda: self.launch_profile(profile_name))
            self.autolaunch_timer.start(delay * 1000)

    def _start_sentry_control_server(self):
        path = sentry_control_socket_path()
        server = QLocalServer(self)
        server.setSocketOptions(QLocalServer.UserAccessOption)
        if not server.listen(str(path)):
            probe = QLocalSocket(self)
            probe.connectToServer(str(path))
            if probe.waitForConnected(100):
                probe.disconnectFromServer()
                server.deleteLater()
                return  # a running T2Launch instance already owns the endpoint
            probe.deleteLater()
            QLocalServer.removeServer(str(path))  # remove only an unresponsive stale socket
            if not server.listen(str(path)):
                server.deleteLater()
                append_launch_diagnostic(f"Sentry control API unavailable: {server.errorString()}")
                return
        self.sentry_control_server = server
        self.sentry_control_path = str(path)
        server.newConnection.connect(self._accept_sentry_control)
        append_launch_diagnostic(f"Sentry control API ready at {path}")

    def _restart_sentry_control_server(self):
        old_path = self.sentry_control_path or str(sentry_control_socket_path())
        old_server = self.sentry_control_server
        if old_server is not None:
            old_server.close()
            old_server.deleteLater()
        QLocalServer.removeServer(old_path)
        self.sentry_control_server = None
        self.sentry_control_path = None
        self._start_sentry_control_server()
        return self.sentry_control_server is not None

    def _accept_sentry_control(self):
        server = self.sentry_control_server
        if not server:
            return
        while server.hasPendingConnections():
            client = server.nextPendingConnection()
            if not client:
                continue
            self._sentry_buffers[id(client)] = bytearray()
            client.readyRead.connect(lambda c=client: self._read_sentry_control(c))
            client.disconnected.connect(lambda c=client: self._sentry_buffers.pop(id(c), None))

    def _read_sentry_control(self, client):
        buffer = self._sentry_buffers.get(id(client))
        if buffer is None:
            return
        request = None
        buffer.extend(bytes(client.readAll()))
        if len(buffer) > 65536:
            response = {"ok": False, "error": "request exceeds 64 KiB"}
        elif b"\n" not in buffer:
            return
        else:
            line, _rest = bytes(buffer).split(b"\n", 1)
            try:
                request = json.loads(line.decode("utf-8"))
                response = self._handle_sentry_request(request)
            except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError, OSError) as error:
                response = {"ok": False, "error": str(error)}
        if isinstance(response, dict) and isinstance(request, dict):
            response.setdefault("request_id", request.get("request_id"))
        client.write((json.dumps(response, separators=(",", ":")) + "\n").encode("utf-8"))
        client.flush()
        client.waitForBytesWritten(100)
        client.disconnectFromServer()

    def _sentry_profile_summaries(self):
        summaries = []
        for profile in self.settings.get("profiles", []):
            if not isinstance(profile, dict):
                continue
            hooked = bool(profile.get("load_hook", True))
            effective = t2options.effective(t2options.migrate(profile),
                                            {"launcher.load_hook": hooked})
            summaries.append({
                "name": str(profile.get("name", "Profile")),
                "client": effective.get("game.executable") == "client",
                "hook": hooked,
                "console_commands": bool(effective.get("hook.console_commands", False)),
                "telemetry": bool(effective.get("telemetry.shazbot", False)),
                "packet_capture": bool(effective.get("telemetry.shazbot_packet_capture", False)),
                "command_labels": bool(effective.get("telemetry.shazbot_command_labels", False)),
                "actions": bool(effective.get("telemetry.shazbot_actions", False)),
            })
        return summaries

    def _sentry_session_summaries(self):
        summaries = []
        for process_key, (name, process) in list(self.processes.items()):
            metadata = self.process_metadata.get(process_key, {})
            state = process.state()
            state_name = ("starting" if state == QProcess.Starting else
                          "running" if state == QProcess.Running else "stopped")
            summaries.append({
                "profile": name,
                "session_id": metadata.get("session_id"),
                "state": state_name,
                "pid": int(process.processId()),
                "hook": bool(metadata.get("hook", False)),
                "telemetry": bool(metadata.get("telemetry", False)),
                "packet_capture": bool(metadata.get("packet_capture", False)),
                "demo_recording": metadata.get("demo_recording"),
            })
        return summaries

    def _handle_sentry_request(self, request):
        if not isinstance(request, dict):
            raise ValueError("request must be a JSON object")
        operation = request.get("op")
        if operation == "ping":
            return {"ok": True, "api": 2, "features": SENTRY_CONTROL_API_FEATURES}
        if operation == "profiles":
            return {"ok": True, "profiles": self._sentry_profile_summaries()}
        if operation == "status":
            return {"ok": True, "sessions": self._sentry_session_summaries()}
        if operation == "restart_control_api":
            restarted = self._restart_sentry_control_server()
            if not restarted:
                raise ValueError("T2Launch could not restart its Sentry control API")
            return {"ok": True, "api": 2, "features": SENTRY_CONTROL_API_FEATURES,
                    "state": "restarted"}
        if operation == "launch_profile":
            name = request.get("profile")
            session_id = request.get("session_id")
            if not isinstance(name, str) or not name:
                raise ValueError("profile name is required")
            if not isinstance(session_id, str) or not session_id:
                raise ValueError("session_id is required for a Sentry launch")
            demo_recording = normalize_sentry_demo_recording(
                request.get("demo_recording")
            )
            overrides = request.get("overrides", {})
            if not isinstance(overrides, dict):
                raise ValueError("overrides must be a JSON object")
            unknown = set(overrides) - SENTRY_SESSION_OVERRIDE_KEYS
            if unknown:
                raise ValueError(f"unsupported session override(s): {', '.join(sorted(unknown))}")
            # This channel is specifically for passive data collection. It never
            # permits the Sentry command to enable policy control of the client.
            overrides = dict(overrides)
            overrides["telemetry.shazbot"] = True
            overrides["telemetry.shazbot_actions"] = False
            overrides["hook.console_commands"] = True
            process = self.launch_profile(
                name, session_overrides=overrides, session_id=session_id,
                load_hook_override=True, show_errors=False,
                demo_recording=demo_recording,
            )
            if process is None:
                raise ValueError("profile launch was rejected; inspect the T2Launch profile and game path")
            return {"ok": True, "session_id": session_id, "profile": name,
                    "pid": int(process.processId()), "state": "starting",
                    "demo_recording": demo_recording}
        raise ValueError("unknown Sentry control operation")

    def refresh_menu(self):
        self.menu.clear()
        for profile in sorted(self.settings["profiles"], key=lambda p: p.get("name", "").casefold()):
            action = QAction(profile.get("name", "Profile"), self.menu)
            action.triggered.connect(lambda checked=False, p=profile: self.launch_profile(p.get("name", "")))
            self.menu.addAction(action)
        self.menu.addSeparator()
        settings_action = QAction("Settings", self.menu)
        settings_action.triggered.connect(self.open_settings)
        self.menu.addAction(settings_action)
        exit_action = QAction("Exit", self.menu)
        exit_action.triggered.connect(self.quit_app)
        self.menu.addAction(exit_action)

    def update_tooltip(self):
        running = [name for name, process in self.processes.values() if process.state() != QProcess.NotRunning]
        if running:
            status = "Running: " + ", ".join(sorted(running, key=str.casefold))
        else:
            status = "No running instances"
        self.tray.setToolTip("T2Launch — " + status)

    def tray_activated(self, reason):
        if reason == QSystemTrayIcon.DoubleClick:
            self.open_settings()

    def open_settings(self):
        if self.settings_dialog and self.settings_dialog.isVisible():
            self.settings_dialog.raise_()
            self.settings_dialog.activateWindow()
            return
        self.autolaunch_timer.stop()
        self.settings_dialog = SettingsDialog(self)
        self.settings_dialog.show()

    def launch_profile(self, name, *, session_overrides=None, session_id=None,
                       load_hook_override=None, show_errors=True, demo_recording=None):
        self.autolaunch_timer.stop()
        profile = next((p for p in self.settings["profiles"] if p.get("name") == name), None)
        if not profile:
            return None
        game_path = Path(self.settings["game_directory"]).expanduser()
        if not game_path.is_dir():
            if show_errors:
                self.tray.showMessage("Game folder needed", "Choose the Tribes 2 folder in Settings.", QSystemTrayIcon.Warning)
                self.open_settings()
            return None
        hooked = (bool(profile.get("load_hook", True)) if load_hook_override is None
                  else bool(load_hook_override))
        try:
            demo_recording = normalize_sentry_demo_recording(demo_recording)
            if demo_recording and session_id is None:
                raise ValueError("demo playback autoplay is only available for Sentry capture sessions")
            args = profile_arguments(profile)
            options = t2options.migrate(profile)
            overrides = session_overrides or {}
            if not isinstance(overrides, dict):
                raise ValueError("session overrides must be a mapping")
            unknown = set(overrides) - SENTRY_SESSION_OVERRIDE_KEYS
            if unknown:
                raise ValueError(f"unsupported session override(s): {', '.join(sorted(unknown))}")
            options.update(overrides)
            t2options.validate(options, {"launcher.load_hook": hooked})
            if options["won.mode"] != "off" and not hooked:
                raise ValueError("Local WON emulation requires the hook.")
            if session_id is not None and not hooked:
                raise ValueError("Sentry telemetry capture requires the hook.")
            if session_id is not None and t2options.effective(
                    options, {"launcher.load_hook": hooked}).get("game.executable") != "client":
                raise ValueError("Sentry telemetry capture requires the client executable.")
            if session_id is not None and not options.get("hook.console_commands", True):
                raise ValueError("Sentry command capture requires hook.console_commands.")
        except ValueError as exc:
            if show_errors:
                self.tray.showMessage("Invalid profile", str(exc), QSystemTrayIcon.Warning)
                self.open_settings()
            return None
        process = QProcess(self)
        env = configure_sentry_demo_environment(
            QProcessEnvironment.systemEnvironment(), demo_recording
        )
        context = {"launcher.load_hook": hooked}
        launch = {"T2LAUNCH_LOAD_HOOK": "1" if hooked else "0",
                  **t2options.launch_env(options, context)}
        hook_config_path = None
        if hooked:
            hook_config_path = t2options.write_hook_config(
                name, options, context, session_id=session_id)
            launch["T2LAUNCH_HOOK_CONFIG"] = str(hook_config_path)
        effective = t2options.effective(options, context)
        dedicated = effective["game.executable"] == "dedicated"
        if not dedicated:
            monitor = self.settings.get("monitor", "")
            launch["T2LAUNCH_MONITOR"] = monitor
            screen_index = next((i for i, screen in enumerate(QApplication.screens())
                                 if screen.name() == monitor), 0)
            launch["T2LAUNCH_MONITOR_INDEX"] = str(screen_index)
        for key, value in launch.items():
            env.insert(key, value)
        process.setProcessEnvironment(env)
        command = ["bash", str(RUNTIME / "run-game"), "tribes2", str(game_path), *args]
        terminal = None
        if dedicated and effective["server.console"] == "terminal":
            terminal = terminal_command(command, launch)
            if terminal is None:
                self.tray.showMessage("No terminal found", "Running the server headless; set $TERMINAL "
                                      "to choose a terminal.", QSystemTrayIcon.Warning)
        process.setProgram((terminal or command)[0])
        process.setArguments((terminal or command)[1:])
        process.setProcessChannelMode(QProcess.MergedChannels)
        self.launch_output_states[id(process)] = {
            "buffer": "", "secrets": sensitive_profile_arguments(args)}
        effective = t2options.effective(options, context)
        self.process_metadata[id(process)] = {
            "profile": name,
            "session_id": session_id,
            "hook": hooked,
            "telemetry": bool(effective.get("telemetry.shazbot", False)),
            "packet_capture": bool(effective.get("telemetry.shazbot_packet_capture", False)),
            "command_labels": bool(effective.get("telemetry.shazbot_command_labels", False)),
            "actions": bool(effective.get("telemetry.shazbot_actions", False)),
            "executable": effective.get("game.executable", "client"),
            "started_at": time.time(),
            "hook_config_path": str(hook_config_path) if hook_config_path else None,
            "demo_recording": demo_recording,
        }
        append_launch_diagnostic(
            f"launch requested; executable={effective['game.executable']} won={options['won.mode']} "
            f"hook={int(hooked)} terminal={int(terminal is not None)} "
            f"session={session_id or '-'} demo={demo_recording or '-'}")
        process.readyReadStandardOutput.connect(lambda p=process, n=name: self.read_launch_output(p, n))
        process.finished.connect(lambda code, status, n=name, p=process: self.launch_finished(n, p, code))
        process.errorOccurred.connect(lambda error, p=process: self.launch_error(p, error))
        process.start()
        self.processes[id(process)] = (name, process)
        self.update_tooltip()
        return process

    def read_launch_output(self, process, name):
        if not is_qobject_valid(process):
            return
        state = self.launch_output_states.get(id(process))
        if state is None:
            return
        try:
            output = process.readAllStandardOutput()
        except RuntimeError:
            return
        state["buffer"] += bytes(output).decode(errors="replace")
        while "\n" in state["buffer"]:
            line, state["buffer"] = state["buffer"].split("\n", 1)
            self._record_launch_output_line(line.rstrip("\r"), state)

    @staticmethod
    def _record_launch_output_line(line, state):
        line = redact_launch_secrets(line, state["secrets"])
        if line:
            print(f"[T2Launch] {line}", flush=True)
        if is_safe_launch_diagnostic(line):
            append_launch_diagnostic(line)

    def launch_finished(self, name, process, code):
        if is_qobject_valid(process):
            self.read_launch_output(process, name)
        state = self.launch_output_states.pop(id(process), None)
        if state and state["buffer"]:
            self._record_launch_output_line(state["buffer"].rstrip("\r"), state)
        append_launch_diagnostic(f"game exited code={code}")
        self.processes.pop(id(process), None)
        metadata = self.process_metadata.pop(id(process), None)
        if metadata and metadata.get("session_id") and metadata.get("hook_config_path"):
            try:
                Path(metadata["hook_config_path"]).unlink(missing_ok=True)
            except OSError as error:
                append_launch_diagnostic(f"Sentry session config cleanup failed: {error}")
        if code:
            self.tray.showMessage("Tribes 2 exited", f"Profile {name} exited with status {code}.", QSystemTrayIcon.Warning)
        self.update_tooltip()
        if is_qobject_valid(process):
            process.deleteLater()

    def launch_error(self, process, error):
        error_code = getattr(error, "value", error)
        append_launch_diagnostic(f"QProcess launch error={int(error_code)}")
        if is_qobject_valid(process):
            self.tray.showMessage("Launch error", process.errorString(), QSystemTrayIcon.Critical)

    def build_support(self):
        if self.build_process and self.build_process.state() != QProcess.NotRunning:
            return
        self.build_dialog = QDialog()
        self.build_dialog.setWindowTitle("Build support binaries")
        layout = QVBoxLayout(self.build_dialog)
        self.build_output = QTextEdit()
        self.build_output.setReadOnly(True)
        layout.addWidget(QLabel("Building the Tribes 2 support binaries from this checkout…"))
        layout.addWidget(self.build_output)
        self.build_dialog.resize(720, 420)
        self.build_dialog.show()
        self.build_process = QProcess(self)
        self.build_process.setProgram("bash")
        self.build_process.setArguments([str(RUNTIME / "build")])
        self.build_process.setProcessChannelMode(QProcess.MergedChannels)
        self.build_process.readyReadStandardOutput.connect(self.read_build_output)
        self.build_process.finished.connect(self.build_finished)
        self.build_process.start()

    def read_build_output(self):
        self.build_output.append(bytes(self.build_process.readAllStandardOutput()).decode(errors="replace").rstrip())

    def build_finished(self, code, status):
        self.read_build_output()
        self.build_output.append("\nBuild succeeded." if code == 0 else f"\nBuild failed (exit status {code}).")

    def quit_app(self):
        # Leave running game/launcher processes alive when the tray app exits.
        for _name, process in self.processes.values():
            process.setParent(None)
        self.quit()


def main():
    app = Launcher(sys.argv)
    if not QSystemTrayIcon.isSystemTrayAvailable():
        QMessageBox.critical(None, "T2Launch", "No system tray is available in this desktop session.")
        return 1
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
