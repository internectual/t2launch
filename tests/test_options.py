import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import t2options

ROOT = Path(__file__).resolve().parents[1]
HOOKED = {"launcher.load_hook": True}


class OptionTests(unittest.TestCase):
    def test_hook_keys_match_the_c_option_table(self):
        source = (ROOT / "t2_hook.c").read_text()
        table = source[source.index("static struct hook_option hook_options[] = {"):]
        table = table[: table.index("};")]
        c_keys = re.findall(r'\{"([a-z_]+\.[a-z_]+)", "', table)
        python_keys = [option.key for option in t2options.OPTIONS if option.hook]
        self.assertEqual(sorted(c_keys), sorted(python_keys))
        # Defaults must agree too, so the hook behaves the same without a config.
        for key, default in re.findall(r'\{"([a-z_]+\.[a-z_]+)", "([^"]*)"', table):
            self.assertEqual(default, "1" if t2options.BY_KEY[key].default else "0", key)

    def test_old_wonemu_flag_migrates_to_won_mode(self):
        self.assertEqual(t2options.migrate({"wonemu": True})["won.mode"], "local")
        self.assertEqual(t2options.migrate({"wonemu": False})["won.mode"], "off")
        self.assertEqual(t2options.migrate({})["won.mode"], "off")
        self.assertEqual(t2options.migrate({"wonemu": True, "options": {"won.mode": "off"}})["won.mode"],
                         "off")

    def test_tribesnext_needs_won_emulation_and_console_commands(self):
        values = t2options.effective({"won.mode": "off"}, HOOKED)
        self.assertFalse(values["hook.tribesnext"])
        self.assertFalse(values["hook.tribes_tab"])
        self.assertTrue(values["video.full_redraw"])
        values = t2options.effective({"won.mode": "local"}, HOOKED)
        self.assertTrue(values["hook.tribesnext"])
        values = t2options.effective({"won.mode": "local", "hook.console_commands": False}, HOOKED)
        self.assertFalse(values["hook.tribesnext"])
        # Without the hook every hook-applied option is off; runtime options stay.
        values = t2options.effective({"won.mode": "local"}, {"launcher.load_hook": False})
        self.assertFalse(any(values[o.key] for o in t2options.OPTIONS if o.hook))
        self.assertTrue(values["audio.openal_volume_fix"])

    def test_unmet_reports_each_requirement(self):
        option = t2options.BY_KEY["hook.tribesnext"]
        values = t2options.chosen({"won.mode": "off", "hook.console_commands": False})
        self.assertEqual([key for key, _ in t2options.unmet(option, values, HOOKED)],
                         ["won.mode", "hook.console_commands"])

    def test_values_are_coerced_and_validated(self):
        self.assertTrue(t2options.chosen({"video.software_gl": "yes"})["video.software_gl"])
        # A bad stored value falls back to the default instead of breaking launch.
        self.assertEqual(t2options.chosen({"won.mode": "bogus"})["won.mode"], "off")
        with self.assertRaisesRegex(ValueError, "choose one of"):
            t2options.validate({"won.mode": "bogus"})
        with self.assertRaisesRegex(ValueError, "line breaks"):
            t2options.validate({"won.bind_address": "1.2.3.4\nx=y"})
        with self.assertRaisesRegex(ValueError, "at most 65531"):
            t2options.validate({"won.base_port": 65535})
        t2options.validate({"unknown.key": "kept"})

    def test_endpoints_fit_the_hooks_bootstrap_slots(self):
        option = t2options.BY_KEY["won.endpoints"]
        self.assertEqual(t2options.coerce(option, "10.0.0.2:15104, won.lan:15200"),
                         ("10.0.0.2:15104", "won.lan:15200"))
        self.assertEqual(t2options.coerce(option, ["", " "]), ())
        for bad in ("10.0.0.2", "host:0", "a-very-long-hostname.example:15104", "a:1,b:2,c:3,d:4",
                    "bad host:1"):
            with self.assertRaises(ValueError, msg=bad):
                t2options.coerce(option, bad)

    def test_master_sources_are_an_ordered_extensible_list(self):
        option = t2options.BY_KEY["won.master_sources"]
        self.assertEqual(t2options.coerce(option, "torque=192.0.2.9:28000,lan,lan"),
                         (("torque", "192.0.2.9:28000"), ("lan", "")))
        self.assertEqual(t2options.coerce(option, [["LAN", "ignored"]]), (("lan", ""),))
        for bad in ("gopher=x", "torque=nohost", "tribesnext=ftp://x"):
            with self.assertRaises(ValueError, msg=bad):
                t2options.coerce(option, bad)
        env = t2options.launch_env({"won.mode": "local"}, HOOKED)
        self.assertEqual(env["T2LAUNCH_MASTER_SOURCES"], "tribesnext=http://master.tribesnext.com,lan")

    def test_remote_mode_needs_endpoints_and_a_verifier(self):
        with self.assertRaisesRegex(ValueError, "host:port to connect to"):
            t2options.validate({"won.mode": "remote"}, HOOKED)
        with self.assertRaisesRegex(ValueError, "verifier"):
            t2options.validate({"won.mode": "remote", "won.endpoints": ["192.0.2.5:15104"]}, HOOKED)
        options = {"won.mode": "remote", "won.endpoints": ["192.0.2.5:15104"],
                   "won.verifier": "/tmp/verifier.der", "won.bind_address": "0.0.0.0"}
        t2options.validate(options, HOOKED)
        env = t2options.launch_env(options, HOOKED)
        self.assertEqual(env["T2LAUNCH_WON_MODE"], "remote")
        self.assertEqual(env["T2LAUNCH_WON_ENDPOINTS"], "192.0.2.5:15104")
        self.assertEqual(env["T2LAUNCH_WON_VERIFIER"], "/tmp/verifier.der")
        # Local-service settings do not leak into a remote launch.
        self.assertNotIn("T2LAUNCH_WON_BIND", env)
        self.assertNotIn("T2LAUNCH_MASTER_SOURCES", env)
        # TribesNext logins need the account's keys locally, so remote mode has none.
        self.assertFalse(t2options.effective(options, HOOKED)["hook.tribesnext"])
        self.assertTrue(t2options.effective(options, HOOKED)["hook.tribes_tab"])

    def test_launch_env_and_hook_config(self):
        env = t2options.launch_env({"won.mode": "local", "won.master_sources": "lan",
                                    "won.bind_address": "0.0.0.0", "won.base_port": 16000,
                                    "video.software_gl": True}, HOOKED)
        self.assertEqual(env["T2LAUNCH_WON_MODE"], "local")
        self.assertEqual(env["T2LAUNCH_MASTER_SOURCES"], "lan")
        self.assertEqual(env["T2LAUNCH_WON_BIND"], "0.0.0.0")
        self.assertEqual(env["T2LAUNCH_WON_BASE_PORT"], "16000")
        self.assertEqual(env["T2LAUNCH_WON_ENDPOINTS"], "")
        self.assertNotIn("T2LAUNCH_WON_VERIFIER", env)
        self.assertEqual(env["T2LAUNCH_SOFTWARE_GL"], "1")
        self.assertEqual(env["T2LAUNCH_OPENAL_VOLUME_FIX"], "1")
        off = t2options.launch_env({"won.mode": "off"}, HOOKED)
        self.assertEqual(off["T2LAUNCH_WON_MODE"], "off")
        self.assertNotIn("T2LAUNCH_WON_ENDPOINTS", off)
        config = t2options.hook_config({"won.mode": "off", "audio.launch_reset": False}, HOOKED)
        lines = dict(line.split("=", 1) for line in config.splitlines() if not line.startswith("#"))
        self.assertEqual(lines["hook.tribesnext"], "0")
        self.assertEqual(lines["audio.launch_reset"], "0")
        self.assertEqual(lines["video.full_redraw"], "1")
        self.assertNotIn("video.software_gl", lines)

    def test_hook_config_file_is_private(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(t2options, "HOOK_CONFIG_DIR", Path(directory) / "hook"):
            path = t2options.write_hook_config("LAN / test", {"won.mode": "off"}, HOOKED)
            self.assertEqual(path.name, "LAN_test.conf")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
            self.assertIn("hook.tribesnext=0", path.read_text())


if __name__ == "__main__":
    unittest.main()
