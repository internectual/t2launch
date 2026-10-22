#!/usr/bin/env python3
"""Desktop launcher for the native Tribes 2 Linux client."""

from __future__ import annotations

import json
import os
import shlex
import sys
import time
from pathlib import Path

try:
    from PySide6.QtCore import QProcess, QProcessEnvironment, QTimer, Signal  # noqa: I001 (grid-wrapped names)
    from PySide6.QtGui import QAction
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
    return {"name": "Online", "flags": {"-online": True}, "values": {}, "positional": "", "custom": ""}


def read_settings():
    try:
        settings = json.loads(CONFIG_FILE.read_text())
    except (OSError, ValueError):
        settings = {}
    profiles = settings.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        profiles = [default_profile()]
    return {
        "game_directory": settings.get("game_directory", str(Path.home() / "t2-linux")),
        "profiles": profiles,
        "load_hook": bool(settings.get("load_hook", True)),
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


def sensitive_profile_arguments(args):
    sensitive = []
    option_counts = {"-login": 2, "-password": 1}
    index = 0
    while index < len(args):
        count = option_counts.get(args[index])
        if count is None:
            index += 1
            continue
        sensitive.extend(args[index + 1:index + 1 + count])
        index += count + 1
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
        tabs = QTabWidget()
        layout.addWidget(tabs, 1)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        body = QWidget()
        form = QFormLayout(body)
        self.game_form = form
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

        # WON emulator and Hook tabs are generated from the t2options registry.
        saved_options = t2options.chosen(t2options.migrate(profile))
        self.option_widgets = {}
        self.option_rows = {}
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
                row = QWidget()
                row_layout = QVBoxLayout(row)
                row_layout.setContentsMargins(12, 0, 0, 4)
                widget = self._option_widget(option, saved_options[option.key])
                widget.setToolTip(option.help)
                if option.kind == "bool":
                    row_layout.addWidget(widget)
                else:
                    line = QHBoxLayout()
                    line.addWidget(QLabel(option.label))
                    line.addWidget(widget, 1)
                    row_layout.addLayout(line)
                note = QLabel()
                note.setStyleSheet("color: gray")
                note.setWordWrap(True)
                row_layout.addWidget(note)
                column.addWidget(row)
                self.option_widgets[option.key] = widget
                self.option_rows[option.key] = (row, note)
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
        self.refresh_options()
        self.refresh_game_rows()

    def refresh_game_rows(self, *_args):
        """Show advanced Game options when asked, and any that are already set."""
        show_advanced = self.show_advanced.isChecked()
        for row, is_set in self.advanced_game_rows:
            self.game_form.setRowVisible(row, show_advanced or is_set())

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
        values = t2options.chosen(self.option_values())
        context = self.context()
        effective = t2options.effective(values, context)
        show_advanced = self.show_advanced.isChecked()
        for option in t2options.OPTIONS:
            row, note = self.option_rows[option.key]
            row.setVisible(show_advanced or not option.advanced)
            # Requirements are judged against effective values, so a chain
            # (TribesNext -> console commands -> hook) disables in one step.
            missing = t2options.unmet(option, effective, context)
            self.option_widgets[option.key].setEnabled(not missing)
            note.setText("Requires: " + ", ".join(requirement_text(key, allowed)
                                                  for key, allowed in missing) if missing else "")
            note.setVisible(bool(missing))
        self.changed()

    def save_to(self):
        result = {
            "name": self.name.text().strip() or "Profile",
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
        self.load_hook = QCheckBox("Load t2_hook.so (LD_PRELOAD)")
        self.load_hook.setChecked(app.settings["load_hook"])
        self.load_hook.toggled.connect(
            lambda _checked: self.profile_editor and self.profile_editor.refresh_options())
        general_form.addRow(self.load_hook)
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
        remove = QPushButton("Remove profile")
        add.clicked.connect(self.add_profile)
        remove.clicked.connect(self.remove_profile)
        profile_buttons = QHBoxLayout()
        profile_buttons.addWidget(add)
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
                self.profiles[row], self.update_profile_name,
                lambda: {"launcher.load_hook": self.load_hook.isChecked()})
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

    def add_profile(self):
        self.commit_editor()
        self.profiles.append({"name": "New profile", "flags": {}, "values": {}, "positional": "", "custom": ""})
        self.profile_list.addItem("New profile")
        self.profile_list.setCurrentRow(len(self.profiles) - 1)
        self.refresh_autolaunch_choices()

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
                try:
                    t2options.validate(t2options.migrate(profile),
                                       {"launcher.load_hook": self.load_hook.isChecked()})
                except ValueError as exc:
                    raise ValueError(f"{profile.get('name', 'Profile')}: {exc}") from exc
            if any(t2options.migrate(profile)["won.mode"] != "off" for profile in self.profiles) \
                    and not self.load_hook.isChecked():
                raise ValueError("Local WON emulator profiles require Load t2_hook.so to be enabled.")
            names = [p["name"].strip() for p in self.profiles]
            if len({name.casefold() for name in names}) != len(names):
                raise ValueError("Profile names must be unique.")
            selected_auto = self.autolaunch.currentData()
            if selected_auto and selected_auto not in names:
                selected_auto = ""
            self.app.settings.update({
                "game_directory": self.game_path.text().strip(),
                "profiles": sorted(self.profiles, key=lambda p: p["name"].casefold()),
                "load_hook": self.load_hook.isChecked(),
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
        self.launch_output_states = {}
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

    def launch_profile(self, name):
        self.autolaunch_timer.stop()
        profile = next((p for p in self.settings["profiles"] if p.get("name") == name), None)
        if not profile:
            return
        game_path = Path(self.settings["game_directory"]).expanduser()
        if not game_path.is_dir():
            self.tray.showMessage("Game folder needed", "Choose the Tribes 2 folder in Settings.", QSystemTrayIcon.Warning)
            self.open_settings()
            return
        try:
            args = profile_arguments(profile)
            options = t2options.migrate(profile)
            t2options.validate(options, {"launcher.load_hook": self.settings["load_hook"]})
            if options["won.mode"] != "off" and not self.settings["load_hook"]:
                raise ValueError("Local WON emulation requires the hook.")
        except ValueError as exc:
            self.tray.showMessage("Invalid profile", str(exc), QSystemTrayIcon.Warning)
            self.open_settings()
            return
        process = QProcess(self)
        env = QProcessEnvironment.systemEnvironment()
        env.insert("T2LAUNCH_LOAD_HOOK", "1" if self.settings["load_hook"] else "0")
        context = {"launcher.load_hook": self.settings["load_hook"]}
        for key, value in t2options.launch_env(options, context).items():
            env.insert(key, value)
        if self.settings["load_hook"]:
            env.insert("T2LAUNCH_HOOK_CONFIG", str(t2options.write_hook_config(name, options, context)))
        monitor = self.settings.get("monitor", "")
        env.insert("T2LAUNCH_MONITOR", monitor)
        screen_index = next((i for i, screen in enumerate(QApplication.screens()) if screen.name() == monitor), 0)
        env.insert("T2LAUNCH_MONITOR_INDEX", str(screen_index))
        process.setProcessEnvironment(env)
        process.setProgram("bash")
        process.setArguments([str(RUNTIME / "run-game"), "tribes2", str(game_path), *args])
        process.setProcessChannelMode(QProcess.MergedChannels)
        self.launch_output_states[id(process)] = {
            "buffer": "", "secrets": sensitive_profile_arguments(args)}
        append_launch_diagnostic(
            f"launch requested; won={options['won.mode']} "
            f"hook={int(bool(self.settings['load_hook']))}")
        process.readyReadStandardOutput.connect(lambda p=process, n=name: self.read_launch_output(p, n))
        process.finished.connect(lambda code, status, n=name, p=process: self.launch_finished(n, p, code))
        process.errorOccurred.connect(lambda error, p=process: self.launch_error(p, error))
        process.start()
        self.processes[id(process)] = (name, process)
        self.update_tooltip()

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
