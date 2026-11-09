import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from PySide6.QtCore import QProcessEnvironment
    from PySide6.QtNetwork import QLocalServer
except ImportError as error:
    raise unittest.SkipTest(f"PySide6 is required for T2Launch control API tests: {error}")

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / "shazbot"))

from sentry import control_client
from t2launch_app import (
    append_launch_diagnostic,
    configure_sentry_demo_environment,
    normalize_sentry_demo_recording,
    redact_launch_secrets,
    sensitive_profile_arguments,
)


SERVER_PROGRAM = r'''import sys
from PySide6.QtCore import QTimer
import t2launch_app
t2launch_app_settings = {
    "profiles": [{"name": "Sentry API Test", "options": {"game.executable": "client"}},
                 {"name": "Dedicated API Test", "options": {"game.executable": "dedicated"}}],
    "monitor": "", "autolaunch_profile": "", "autolaunch_delay": 0
}
t2launch_app.read_settings = lambda: t2launch_app_settings
t2launch_app.append_launch_diagnostic = lambda _message: None
app = t2launch_app.Launcher(["sentry-api-test"])
saved_profile = repr(app.settings["profiles"])
class FakeProcess:
    def processId(self): return 4421
def verify_launch(name, **kwargs):
    if name == "Dedicated API Test":
        assert kwargs == {"show_errors": False}, kwargs
        print("dedicated-launch-verified", flush=True)
        return FakeProcess()
    assert name == "Sentry API Test"
    assert kwargs["session_id"] == "test-session-001"
    assert kwargs["load_hook_override"] is True
    assert kwargs["show_errors"] is False
    assert kwargs["demo_recording"] == "recordings/demo.rec"
    assert kwargs["session_overrides"] == {
        "telemetry.shazbot": True,
        "telemetry.shazbot_socket": "/tmp/sentry-runtime.sock",
        "telemetry.shazbot_packet_capture": False,
        "telemetry.shazbot_notification_socket": "/tmp/sentry-notify.sock",
        "telemetry.shazbot_actions": False,
        "telemetry.shazbot_command_labels": True,
        "telemetry.shazbot_auto_weapon_slot_test": True,
        "telemetry.shazbot_weapon_slot_test_script": "prefs/ShazbotSentryWeaponSlotTest_test-session-001.cs",
        "hook.console_commands": True,
        "video.full_redraw": False,
    }
    assert repr(app.settings["profiles"]) == saved_profile
    print("launch-overrides-verified", flush=True)
    return FakeProcess()
app.launch_profile = verify_launch
QTimer.singleShot(15000, app.quit)
raise SystemExit(app.exec())
'''


class SentryControlApiTests(unittest.TestCase):
    def test_login_and_password_arguments_are_redacted_from_launch_logs(self):
        args = [
            "-login", "user-name-secret", "account-password-secret",
            "-password", "server-password-secret",
            "-telnetParams", "28000", "telnet-password-secret", "listen-password-secret",
        ]
        secrets = sensitive_profile_arguments(args)
        self.assertEqual(secrets, (
            "user-name-secret", "account-password-secret", "server-password-secret",
            "telnet-password-secret", "listen-password-secret",
        ))
        message = ("command: " + " ".join(args) +
                   " username=reported-user-secret password=reported-pass-secret")
        redacted = redact_launch_secrets(message, secrets)
        for secret in (*secrets, "reported-user-secret", "reported-pass-secret"):
            self.assertNotIn(secret, redacted)

        with tempfile.TemporaryDirectory(prefix="t2launch-redacted-log-") as directory:
            path = Path(directory) / "launch.log"
            with mock.patch("t2launch_app.launch_diagnostics_path", return_value=path):
                append_launch_diagnostic(message)
            logged = path.read_text(encoding="utf-8")
            for secret in (*secrets, "reported-user-secret", "reported-pass-secret"):
                self.assertNotIn(secret, logged)

    def test_demo_recording_is_restricted_and_stale_environment_is_cleared(self):
        self.assertIsNone(normalize_sentry_demo_recording(None))
        self.assertEqual(
            normalize_sentry_demo_recording("recordings/tribesforever-3116.rec"),
            "recordings/tribesforever-3116.rec",
        )
        for invalid in (
            "../demo.rec", "/tmp/demo.rec", "recordings/../demo.rec",
            'recordings/demo".rec', "recordings/subdir/demo.rec",
        ):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                normalize_sentry_demo_recording(invalid)

        environment = QProcessEnvironment()
        environment.insert("T2LAUNCH_AUTOPLAY_DEMO", "recordings/stale.rec")
        configure_sentry_demo_environment(environment, None)
        self.assertEqual(environment.value("T2LAUNCH_AUTOPLAY_DEMO"), "")
        configure_sentry_demo_environment(environment, "recordings/demo.rec")
        self.assertEqual(environment.value("T2LAUNCH_AUTOPLAY_DEMO"), "recordings/demo.rec")

    def test_ping_profiles_and_status_round_trip_through_launcher_server(self):
        with tempfile.TemporaryDirectory(prefix="t2launch-sentry-api-") as directory:
            runtime_dir = Path(directory) / "runtime"
            runtime_dir.mkdir()
            env = os.environ.copy()
            env.update({
                "QT_QPA_PLATFORM": "offscreen",
                "XDG_RUNTIME_DIR": str(runtime_dir),
                "XDG_CONFIG_HOME": str(Path(directory) / "config"),
            })
            server = subprocess.Popen(
                [sys.executable, "-c", SERVER_PROGRAM], cwd=ROOT, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            endpoint = runtime_dir / f"t2launch-sentry-{os.getuid()}.sock"
            try:
                deadline = time.monotonic() + 5.0
                while not endpoint.exists() and server.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.01)
                if not endpoint.exists():
                    if server.poll() is None:
                        server.terminate()
                        server.wait(timeout=3.0)
                    self.fail(f"T2Launch did not publish its control socket: {server.stderr.read()}")
                with mock.patch.object(control_client, "socket_path", return_value=endpoint):
                    ping = control_client.request({"op": "ping"}, timeout_ms=2000)
                    profiles = control_client.request({"op": "profiles"}, timeout_ms=2000)
                    launched = control_client.request({
                        "op": "launch_profile",
                        "profile": "Sentry API Test",
                        "session_id": "test-session-001",
                        "demo_recording": "recordings/demo.rec",
                        "overrides": {
                            "telemetry.shazbot": False,
                            "telemetry.shazbot_socket": "/tmp/sentry-runtime.sock",
                            "telemetry.shazbot_packet_capture": False,
                            "telemetry.shazbot_notification_socket": "/tmp/sentry-notify.sock",
                            "telemetry.shazbot_actions": True,
                            "telemetry.shazbot_command_labels": True,
                            "telemetry.shazbot_auto_weapon_slot_test": True,
                            "telemetry.shazbot_weapon_slot_test_script":
                                "prefs/ShazbotSentryWeaponSlotTest_test-session-001.cs",
                            "hook.console_commands": False,
                            "video.full_redraw": False,
                        },
                    }, timeout_ms=2000)
                    dedicated = control_client.request(
                        {"op": "launch_dedicated", "profile": "Dedicated API Test"}, timeout_ms=2000)
                    client_as_dedicated = control_client.request(
                        {"op": "launch_dedicated", "profile": "Sentry API Test"}, timeout_ms=2000)
                    stopped = control_client.request({"op": "stop_dedicated"}, timeout_ms=2000)
                    restarted = control_client.request({"op": "restart_control_api"}, timeout_ms=2000)
                    status = control_client.request({"op": "status"}, timeout_ms=2000)
                self.assertTrue(ping["ok"])
                self.assertEqual(ping["api"], 2)
                self.assertIn("session_demo_recording", ping["features"])
                self.assertEqual(profiles["profiles"][0]["name"], "Sentry API Test")
                self.assertTrue(profiles["profiles"][0]["client"])
                self.assertTrue(launched["ok"])
                self.assertEqual(launched["pid"], 4421)
                self.assertEqual(launched["session_id"], "test-session-001")
                self.assertEqual(server.stdout.readline().strip(), "launch-overrides-verified")
                self.assertIn("dedicated_server", ping["features"])
                self.assertEqual(profiles["profiles"][1]["executable"], "dedicated")
                self.assertTrue(dedicated["ok"])
                self.assertEqual(dedicated["pid"], 4421)
                self.assertEqual(server.stdout.readline().strip(), "dedicated-launch-verified")
                self.assertFalse(client_as_dedicated["ok"])
                self.assertIn("dedicated executable", client_as_dedicated["error"])
                self.assertEqual(stopped, {"ok": True, "stopped": [], "request_id": None})
                self.assertTrue(restarted["ok"])
                self.assertEqual(restarted["api"], 2)
                self.assertEqual(restarted["state"], "restarted")
                self.assertEqual(status["sessions"], [])
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=3.0)
                error_output = server.stderr.read()
                server.stdout.close()
                server.stderr.close()
                if server.returncode not in (0, -15):
                    self.fail(f"T2Launch test server exited {server.returncode}: {error_output}")


if __name__ == "__main__":
    unittest.main()
