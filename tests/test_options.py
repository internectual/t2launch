import ast
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import t2options

ROOT = Path(__file__).resolve().parents[1]
HOOKED = {"launcher.load_hook": True}


class OptionTests(unittest.TestCase):
    def test_sentry_session_override_allowlist_matches_capture_client(self):
        tree = ast.parse((ROOT / "t2launch_app.py").read_text())
        assignment = next(
            node for node in tree.body
            if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == "SENTRY_SESSION_OVERRIDE_KEYS"
                for target in node.targets
            )
        )
        allowed = set(ast.literal_eval(assignment.value.args[0]))
        self.assertEqual(allowed, {
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

    def test_hook_keys_match_the_c_option_table(self):
        source = (ROOT / "t2_hook.c").read_text()
        table = source[source.index("static struct hook_option hook_options[] = {"):]
        table = table[: table.index("};")]
        c_keys = re.findall(r'\{"([a-z_]+\.[a-z_]+)", "', table)
        python_keys = [option.key for option in t2options.OPTIONS if option.hook]
        self.assertEqual(sorted(c_keys), sorted(python_keys))
        # Defaults must agree too, so the hook behaves the same without a config.
        for key, default in re.findall(r'\{"([a-z_]+\.[a-z_]+)", "([^"]*)"', table):
            self.assertEqual(default, t2options._text(t2options.BY_KEY[key].default), key)

    def test_mem_patch_modes_default_to_warning(self):
        option = t2options.BY_KEY["hook.mem_patch"]
        self.assertEqual(option.kind, "choice")
        self.assertEqual(option.choices, ("off", "warn", "enabled"))
        self.assertEqual(option.default, "warn")
        self.assertTrue(option.advanced)
        self.assertEqual(t2options.effective({"hook.console_commands": False}, HOOKED)["hook.mem_patch"],
                         "warn")
        source = (ROOT / "t2_hook.c").read_text()
        registration = source.index('Con__addMethodS(NULL, "memPatch"')
        console_gate = source.index('if (!hook_option_bool("hook.console_commands"))', registration)
        self.assertLess(registration, console_gate)

    def test_review_command_usage_has_single_optional_parameter(self):
        source = (ROOT / "t2_hook.c").read_text()
        registrations = source[source.index('Con__addMethodS(NULL, "hook_review_range"'):
                               source.index('Con__addMethodS(NULL, "hook_sha1"')]
        for command in ("range", "global_beacons", "ffi", "no_cloak", "autobeacon"):
            with self.subTest(command=command):
                self.assertIn(f'hook_review_{command}([enabled])', registrations)
        self.assertNotIn("[0|1]", registrations)

    def test_render_modifier_path_options_are_written_to_hook_config(self):
        for key in ("render.datablock_rules", "render.object_rules", "render.sky_rotation_speed"):
            with self.subTest(key=key):
                self.assertTrue(t2options.BY_KEY[key].hook)
                self.assertIn(f"{key}=", t2options.hook_config({}, HOOKED))
        source = (ROOT / "t2_hook.c").read_text()
        self.assertIn('{"render.datablock_rules", ""', source)
        self.assertIn('{"render.object_rules", ""', source)
        self.assertIn('"t2launch_collectRenderModifiers(name, class) - internal render-rule collector", 4, 4',
                      source)
        datablocks = (ROOT / "runtime/render-modifiers.datablocks.example.cfg").read_text()
        objects = (ROOT / "runtime/render-modifiers.objects.example.cfg").read_text()
        self.assertIn('client connect class:Sky.materialList = "grid.dml"',
                      datablocks)
        self.assertIn('client frame class:Sky.materialList = "grid.dml"', datablocks)
        self.assertIn("zGridSkyBox.vl2", datablocks)
        self.assertIn("client frame navHud.position", objects)
        self.assertEqual(t2options.BY_KEY["render.sky_rotation_speed"].default, 0.0)

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
        self.assertFalse(any(values[o.key] for o in t2options.OPTIONS if o.hook and o.kind == "bool"))
        self.assertTrue(values["audio.openal_volume_fix"])

    def test_unmet_reports_each_requirement(self):
        option = t2options.BY_KEY["hook.tribesnext"]
        values = t2options.chosen({"won.mode": "off", "hook.console_commands": False})
        self.assertEqual([key for key, _ in t2options.unmet(option, values, HOOKED)],
                         ["won.mode", "hook.console_commands"])

    def test_review_range_and_beacons_are_independent_and_require_review(self):
        keys = ("review.extended_range", "review.global_beacons", "review.ffi", "review.no_cloak", "review.autobeacon")
        normal = t2options.effective({}, HOOKED)
        self.assertTrue(all(not normal[key] for key in keys))
        review = t2options.effective({"review.cheat_view": True}, HOOKED)
        self.assertTrue(all(review[key] for key in keys))
        selected = t2options.effective({"review.cheat_view": True,
                                       "review.extended_range": False}, HOOKED)
        self.assertFalse(selected["review.extended_range"])
        self.assertTrue(selected["review.global_beacons"])
        self.assertTrue(selected["review.ffi"])
        selected = t2options.effective({"review.cheat_view": True, "review.ffi": False,
                                       "review.no_cloak": False}, HOOKED)
        self.assertFalse(selected["review.ffi"])
        self.assertFalse(selected["review.no_cloak"])
        self.assertTrue(selected["review.extended_range"])
        self.assertTrue(selected["review.autobeacon"])
        self.assertTrue(all(key in t2options.CLIENT_ONLY for key in keys))
        self.assertTrue(review["review.shortcuts"])
        no_commands = t2options.effective({"review.cheat_view": True,
                                           "hook.console_commands": False}, HOOKED)
        self.assertFalse(no_commands["review.shortcuts"])

    def test_values_are_coerced_and_validated(self):
        self.assertTrue(t2options.chosen({"video.software_gl": "yes"})["video.software_gl"])
        self.assertTrue(t2options.coerce(t2options.BY_KEY["security.safe_soundpacks"], ""))
        self.assertTrue(t2options.coerce(t2options.BY_KEY["security.safe_soundpacks"], None))
        self.assertFalse(t2options.coerce(t2options.BY_KEY["security.safe_soundpacks"], "0"))
        self.assertEqual(t2options.coerce(t2options.BY_KEY["won.mode"], ""), "off")
        self.assertEqual(t2options.coerce(t2options.BY_KEY["discord.app_id"], ""),
                         t2options.BY_KEY["discord.app_id"].default)
        self.assertEqual(t2options.BY_KEY["discord.app_id"].default, "488764170460725249")
        self.assertEqual(t2options.coerce(t2options.BY_KEY["server.public_address"], ""), "")
        # A bad stored value falls back to the default instead of breaking launch.
        self.assertEqual(t2options.chosen({"won.mode": "bogus"})["won.mode"], "off")
        with self.assertRaisesRegex(ValueError, "choose one of"):
            t2options.validate({"won.mode": "bogus"})
        with self.assertRaisesRegex(ValueError, "line breaks"):
            t2options.validate({"won.bind_address": "1.2.3.4\nx=y"})
        with self.assertRaisesRegex(ValueError, "at most 65531"):
            t2options.validate({"won.base_port": 65535})
        t2options.validate({"unknown.key": "kept"})

    def test_build_identifier_is_profile_scoped_and_written_to_hook_config(self):
        option = t2options.BY_KEY["net.build_identifier"]
        self.assertEqual(option.default, 25034)
        self.assertEqual(t2options.coerce(option, "24834"), 24834)
        self.assertEqual(t2options.coerce(option, ""), option.default)
        self.assertEqual(t2options.coerce(option, "   "), option.default)
        t2options.validate({"net.build_identifier": ""}, HOOKED)
        blank_config = t2options.hook_config({"net.build_identifier": ""}, HOOKED)
        self.assertIn("net.build_identifier=25034", blank_config.splitlines())
        reset = t2options.reset_option({"net.build_identifier": 24834,
                                        "demo.protocol_tag": 3145732}, "net.build_identifier")
        self.assertEqual(reset["net.build_identifier"], option.default)
        self.assertEqual(reset["demo.protocol_tag"], 3145732)
        with self.assertRaisesRegex(ValueError, "at least 1"):
            t2options.coerce(option, 0)
        with self.assertRaisesRegex(ValueError, "at most 2147483647"):
            t2options.coerce(option, 2147483648)
        config = t2options.hook_config({"net.build_identifier": 987654}, HOOKED)
        self.assertIn("net.build_identifier=987654", config.splitlines())
        server = t2options.effective({"game.executable": "dedicated",
                                      "net.build_identifier": 24834}, HOOKED)
        self.assertEqual(server["net.build_identifier"], 24834)

    def test_demo_protocol_tag_is_client_scoped_and_profile_configurable(self):
        option = t2options.BY_KEY["demo.protocol_tag"]
        self.assertEqual(option.default, 3342340)
        self.assertEqual(t2options.coerce(option, "3145732"), 3145732)
        self.assertEqual(t2options.coerce(option, ""), option.default)
        self.assertEqual(t2options.coerce(option, "   "), option.default)
        t2options.validate({"demo.protocol_tag": ""}, HOOKED)
        blank_config = t2options.hook_config({"demo.protocol_tag": ""}, HOOKED)
        self.assertIn("demo.protocol_tag=3342340", blank_config.splitlines())
        reset = t2options.reset_option({"demo.protocol_tag": 3145732}, "demo.protocol_tag")
        self.assertEqual(reset["demo.protocol_tag"], option.default)
        with self.assertRaisesRegex(ValueError, "at least 1"):
            t2options.coerce(option, 0)
        with self.assertRaisesRegex(ValueError, "at most 2147483647"):
            t2options.coerce(option, 2147483648)
        config = t2options.hook_config({"demo.protocol_tag": 3145732}, HOOKED)
        self.assertIn("demo.protocol_tag=3145732", config.splitlines())
        dedicated = t2options.effective({"game.executable": "dedicated",
                                         "demo.protocol_tag": 3145732}, HOOKED)
        self.assertEqual(dedicated["demo.protocol_tag"], option.off_value)

    def test_blank_soundpack_limit_restores_safe_cap_but_zero_is_explicit_off(self):
        option = t2options.BY_KEY["security.max_soundpack_kbytes"]
        self.assertEqual(option.default, 8192)
        self.assertEqual(t2options.coerce(option, ""), 8192)
        self.assertEqual(t2options.coerce(option, "   "), 8192)
        self.assertEqual(t2options.coerce(option, "0"), 0)
        with self.assertRaisesRegex(ValueError, "at most 2097151"):
            t2options.coerce(option, 2097152)
        blank_config = t2options.hook_config({"security.max_soundpack_kbytes": ""}, HOOKED)
        self.assertIn("security.max_soundpack_kbytes=8192", blank_config.splitlines())

    def test_shazbot_telemetry_is_opt_in_client_only_and_configurable(self):
        enabled = t2options.BY_KEY["telemetry.shazbot"]
        socket_path = t2options.BY_KEY["telemetry.shazbot_socket"]
        notification_socket = t2options.BY_KEY["telemetry.shazbot_notification_socket"]
        packet_capture = t2options.BY_KEY["telemetry.shazbot_packet_capture"]
        actions = t2options.BY_KEY["telemetry.shazbot_actions"]
        action_socket = t2options.BY_KEY["telemetry.shazbot_action_socket"]
        command_labels = t2options.BY_KEY["telemetry.shazbot_command_labels"]
        self.assertFalse(enabled.default)
        self.assertEqual(socket_path.default, "/tmp/shazbot.sock")
        self.assertEqual(notification_socket.default, "/tmp/shazbot-notify.sock")
        self.assertFalse(packet_capture.default)
        self.assertFalse(actions.default)
        self.assertFalse(command_labels.default)
        self.assertEqual(action_socket.default, "/tmp/shazbot-action.sock")
        self.assertIn(enabled.key, t2options.CLIENT_ONLY)
        self.assertIn(socket_path.key, t2options.CLIENT_ONLY)
        self.assertIn(notification_socket.key, t2options.CLIENT_ONLY)
        self.assertIn(packet_capture.key, t2options.CLIENT_ONLY)
        self.assertIn(actions.key, t2options.CLIENT_ONLY)
        self.assertIn(action_socket.key, t2options.CLIENT_ONLY)
        self.assertIn(command_labels.key, t2options.CLIENT_ONLY)
        self.assertFalse(t2options.effective({}, HOOKED)[enabled.key])
        client = t2options.effective({enabled.key: True}, HOOKED)
        self.assertTrue(client[enabled.key])
        self.assertEqual(client[socket_path.key], "/tmp/shazbot.sock")
        packet_config = t2options.hook_config({
            packet_capture.key: True,
            socket_path.key: "/tmp/custom-shazbot.sock",
        }, HOOKED)
        self.assertIn("telemetry.shazbot_packet_capture=1", packet_config.splitlines())
        self.assertIn("telemetry.shazbot_socket=/tmp/custom-shazbot.sock", packet_config.splitlines())
        configured = t2options.hook_config({enabled.key: True,
                                             socket_path.key: "/tmp/custom-shazbot.sock"}, HOOKED)
        self.assertIn("telemetry.shazbot=1", configured.splitlines())
        self.assertIn("telemetry.shazbot_socket=/tmp/custom-shazbot.sock", configured.splitlines())
        notification_config = t2options.hook_config({
            enabled.key: True,
            notification_socket.key: "/tmp/custom-notify.sock",
        }, HOOKED)
        self.assertIn("telemetry.shazbot_notification_socket=/tmp/custom-notify.sock",
                      notification_config.splitlines())
        action_config = t2options.hook_config({enabled.key: True, actions.key: True,
                                                action_socket.key: "/tmp/custom-action.sock",
                                                command_labels.key: True}, HOOKED)
        self.assertIn("telemetry.shazbot_actions=1", action_config.splitlines())
        self.assertIn("telemetry.shazbot_action_socket=/tmp/custom-action.sock", action_config.splitlines())
        self.assertIn("telemetry.shazbot_command_labels=1", action_config.splitlines())
        self.assertNotIn("T2LAUNCH_USE_PADSP", t2options.launch_env({enabled.key: True}, HOOKED))
        self.assertNotIn("T2LAUNCH_USE_PADSP",
                         t2options.launch_env({enabled.key: True, actions.key: True}, HOOKED))
        dedicated = t2options.effective({"game.executable": "dedicated",
                                          enabled.key: True,
                                          socket_path.key: "/tmp/custom-shazbot.sock"}, HOOKED)
        self.assertFalse(dedicated[enabled.key])
        self.assertEqual(dedicated[socket_path.key], socket_path.default)
        self.assertEqual(t2options.coerce(socket_path, ""), socket_path.default)

    def test_endpoints_fit_the_hooks_bootstrap_slots(self):
        option = t2options.BY_KEY["won.endpoints"]
        self.assertEqual(t2options.coerce(option, "10.0.0.2:15104, won.lan:15200"),
                         ("10.0.0.2:15104", "won.lan:15200"))
        self.assertEqual(t2options.coerce(option, ["", " "]), ())
        for bad in ("10.0.0.2", "host:0", "a-very-long-hostname.example:15104", "a:1,b:2,c:3,d:4",
                    "bad host:1"):
            with self.assertRaises(ValueError, msg=bad):
                t2options.coerce(option, bad)

    def test_backend_kinds_match_wonemu(self):
        import wonemu
        self.assertEqual(t2options.BACKEND_KINDS, wonemu.BACKEND_KINDS)
        self.assertEqual(list(t2options.DEFAULT_BACKENDS), wonemu.default_backends())
        # What the launcher sends, wonemu accepts unchanged.
        env = t2options.launch_env({"won.mode": "local"}, HOOKED)
        self.assertEqual(wonemu.parse_backends(env["T2LAUNCH_WON_BACKENDS"]), wonemu.default_backends())

    def test_backends_are_ordered_and_validated(self):
        option = t2options.BY_KEY["won.backends"]
        self.assertEqual(t2options.coerce(option, ""), option.default)
        t2options.validate({"won.backends": ""}, HOOKED)
        backends = t2options.coerce(option, [
            {"kind": "native", "auth": True, "master": True, "irc": False},
            {"kind": "tribesnext", "auth": False, "master": True, "irc": False},
            {"kind": "torque", "master": "192.0.2.9:28000"}])
        self.assertEqual(backends, (
            {"kind": "native", "auth": True, "master": True, "irc": False},
            {"kind": "tribesnext", "auth": False, "master": "http://master.tribesnext.com", "irc": False},
            {"kind": "torque", "master": "192.0.2.9:28000"}))
        # A checked cell with an empty address (as older launchers saved it) stays checked.
        checked = t2options.coerce(option, [{"kind": "tribesnext", "auth": True, "master": "", "irc": ""}])
        self.assertEqual(checked[0]["irc"], True)
        self.assertEqual(checked[0]["master"], "http://master.tribesnext.com")
        for bad in ([{"kind": "gopher"}], [{"kind": "native", "auth": False}],
                    [{"kind": "native", "auth": True}, {"kind": "torque", "master": "nohost"}],
                    [{"kind": "tribesnext", "auth": True, "master": "ftp://x"}]):
            with self.assertRaises(ValueError, msg=bad):
                t2options.coerce(option, bad)

    def test_old_master_sources_migrate_to_backends(self):
        options = t2options.migrate({"options": {"won.mode": "local", "won.master_sources": [
            ["lan", ""], ["torque", "192.0.2.9:28000"]]}})
        self.assertNotIn("won.master_sources", options)
        backends = t2options.coerce(t2options.BY_KEY["won.backends"], options["won.backends"])
        self.assertFalse(backends[0]["master"])
        self.assertTrue(backends[0]["auth"])
        self.assertEqual(backends[2], {"kind": "torque", "master": "192.0.2.9:28000"})

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
        self.assertNotIn("T2LAUNCH_WON_BACKENDS", env)
        # TribesNext logins need the account's keys locally, so remote mode has none.
        self.assertFalse(t2options.effective(options, HOOKED)["hook.tribesnext"])
        self.assertTrue(t2options.effective(options, HOOKED)["hook.tribes_tab"])

    def test_launch_env_and_hook_config(self):
        env = t2options.launch_env({"won.mode": "local",
                                    "won.bind_address": "0.0.0.0", "won.base_port": 16000,
                                    "video.software_gl": True}, HOOKED)
        self.assertEqual(env["T2LAUNCH_WON_MODE"], "local")
        self.assertIn('"kind":"tribesnext"', env["T2LAUNCH_WON_BACKENDS"])
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
        self.assertEqual(lines["runtime.monotonic_clock"], "1")
        self.assertNotIn("video.software_gl", lines)
        models = {"review.cheat_view": True, "review.replace_models": True}
        self.assertEqual(t2options.launch_env(models, HOOKED)["T2LAUNCH_REVIEW_MODELS"], "1")
        self.assertIn("review.replace_models=1", t2options.hook_config(models, HOOKED).splitlines())
        models["review.cheat_view"] = False
        self.assertIn("review.replace_models=0", t2options.hook_config(models, HOOKED).splitlines())
        unhooked = t2options.hook_config({"won.mode": "off"}, {"launcher.load_hook": False})
        self.assertIn("runtime.monotonic_clock=0", unhooked)

    def test_dedicated_profiles_drop_client_options(self):
        server = {"game.executable": "dedicated", "won.mode": "local", "video.gamma": True,
                  "server.auto_restart": True}
        values = t2options.effective(server, HOOKED)
        for key in t2options.CLIENT_ONLY:
            self.assertEqual(values[key], t2options.BY_KEY[key].off_value, key)
        self.assertTrue(values["net.packet_limits"])
        self.assertTrue(values["runtime.monotonic_clock"])
        self.assertTrue(values["server.tribesnext_auth"])
        env = t2options.launch_env(server, HOOKED)
        self.assertEqual(env["T2LAUNCH_EXECUTABLE"], "dedicated")
        self.assertEqual(env["T2LAUNCH_SERVER_RESTART"], "1")
        self.assertEqual(env["T2LAUNCH_SERVER_CONSOLE"], "terminal")
        self.assertNotIn("T2LAUNCH_SOFTWARE_GL", env)
        lines = dict(line.split("=", 1) for line in t2options.hook_config(server, HOOKED).splitlines()
                     if not line.startswith("#"))
        self.assertEqual((lines["video.gamma"], lines["server.tribesnext_auth"]), ("0", "1"))
        # A client profile has no server options, and server-only hook options stay off.
        client = t2options.effective({"won.mode": "local"}, HOOKED)
        self.assertFalse(client["server.tribesnext_auth"])
        self.assertTrue(client["video.gamma"])
        self.assertNotIn("T2LAUNCH_SERVER_RESTART", t2options.launch_env({"won.mode": "local"}, HOOKED))
        with self.assertRaises(ValueError):
            t2options.coerce(t2options.BY_KEY["game.executable"], "server")

    def test_gamescope_options_need_gamescope(self):
        profile = {"won.mode": "off", "video.gamescope": True, "video.gamescope_filter": "fsr"}
        with mock.patch.object(t2options.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "gamescope installed"):
                t2options.validate(profile, HOOKED)
        with mock.patch.object(t2options.shutil, "which", return_value="/usr/bin/gamescope"):
            t2options.validate(profile, HOOKED)
        env = t2options.launch_env(profile, HOOKED)
        self.assertEqual((env["T2LAUNCH_GAMESCOPE"], env["T2LAUNCH_GAMESCOPE_FILTER"]), ("1", "fsr"))
        off = t2options.launch_env({"won.mode": "off"}, HOOKED)
        self.assertEqual(off["T2LAUNCH_GAMESCOPE"], "0")
        self.assertNotIn("T2LAUNCH_GAMESCOPE_FILTER", off)

    def test_hook_config_file_is_private(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(t2options, "HOOK_CONFIG_DIR", Path(directory) / "hook"):
            path = t2options.write_hook_config("LAN / test", {"won.mode": "off"}, HOOKED)
            self.assertEqual(path.name, "LAN_test.conf")
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
            self.assertIn("hook.tribesnext=0", path.read_text())
            session_path = t2options.write_hook_config(
                "LAN / test", {"won.mode": "off"}, HOOKED, session_id="sentry_abc123"
            )
            self.assertEqual(session_path.name, "LAN_test-sentry_abc123.conf")
            with self.assertRaisesRegex(ValueError, "session_id"):
                t2options.write_hook_config(
                    "LAN / test", {"won.mode": "off"}, HOOKED, session_id="../unsafe"
                )


if __name__ == "__main__":
    unittest.main()
