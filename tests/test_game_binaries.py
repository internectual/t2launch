"""Installed-binary patch anchors and synthetic native review regressions.

Binary anchors require the Loki #25034 client/server in ~/t2-linux. Native
review fixtures require Linux and a working i386 C toolchain/runtime. The
native descriptor-parser and resource-lookup fixtures also need the client.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

GAME = Path.home() / "t2-linux"
PROLOGUE = bytes.fromhex("55 89 e5")
CLOCK = bytes.fromhex("0f 31 55 89 e5 83 ec 14")
TARGETS = {
    "tribes2.dynamic": {
        "console": (0x080E85C0, 0x080E88F4), "clock": 0x081CFA44,
        "won": (0x084846B1, 0x084846CD, 0x084846E9), "verifier": 0x0849A860,
        "rate": 0x08495CEC, "size": 0x08495CF0, "net": (0x081F8EAE, 0x081FB382, 0x081FB683),
    },
    "tribes2d.dynamic": {
        "console": (0x080E74B0, 0x080E77E4), "clock": 0x081CD014,
        "won": (0x08479751, 0x0847976D, 0x08479789), "verifier": 0x0848F520,
        "rate": 0x0848ADCC, "size": 0x0848ADD0, "net": (0x081EDB4E, 0x081F0022, 0x081F0323),
    },
}


class HudBarDemoAssetTests(unittest.TestCase):
    def test_hudbot_art_and_demo_paths_match(self):
        tests = Path(__file__).parent
        script = (tests / "hudBarDemo.cs").read_text()
        samples = {
            "hudbot_horizontal": {
                "health_bar.png": (128, 16), "health_frame.png": (128, 16),
                "energy_bar.png": (128, 8), "energy_frame.png": (128, 8),
            },
            "hudbot_vertical": {
                "health_bar.png": (16, 128), "health_frame.png": (16, 128),
                "energy_bar.png": (8, 128), "energy_frame.png": (8, 128),
            },
        }
        for variant, files in samples.items():
            for name, expected in files.items():
                data = (tests / "hudBarDemo_assets" / variant / name).read_bytes()
                self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                self.assertEqual(data[12:16], b"IHDR")
                self.assertEqual(struct.unpack_from(">II", data, 16), expected)
                self.assertIn(f"hudBarDemo_assets/{variant}/{name}", script)
        self.assertIn('$T2Launch::HudDemo::Radar = 1;', script)
        self.assertIn('$pref::skyRotationSpeed = 0.2;', script)
        self.assertIn('$T2Launch::HudDemo::CompassMode = "both";', script)
        self.assertIn("hook_hud_demo_heading()", script)
        self.assertNotIn("getEyeVector()", script)


def reader(path):
    data = path.read_bytes()
    phoff, = struct.unpack_from("<I", data, 28)
    size, count = struct.unpack_from("<HH", data, 42)
    segments = [struct.unpack_from("<8I", data, phoff + size * i) for i in range(count)]

    def read(address, length):
        for kind, offset, vaddr, _, filesz, _, _, _ in segments:
            if kind == 1 and vaddr <= address and address + length <= vaddr + filesz:
                return data[offset + address - vaddr:offset + address - vaddr + length]
        raise AssertionError(f"0x{address:08x} is not in a loaded segment")
    return read


@unittest.skipUnless(sys.platform.startswith("linux"), "native fixtures use Linux memory mappings")
class NativeReviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("gcc")
        if not cls.compiler:
            raise unittest.SkipTest("gcc is unavailable for native review fixtures")
        directory = tempfile.TemporaryDirectory(prefix="t2-native-review-")
        cls.addClassCleanup(directory.cleanup)
        cls.directory = Path(directory.name)
        # Fixtures must use their synthetic options, not an inherited launch
        # profile or the game's preload/audit/compatibility libraries.
        cls.environment = {key: value for key, value in os.environ.items()
                           if not key.startswith("T2LAUNCH_") and
                           key not in ("LD_PRELOAD", "LD_AUDIT", "LD_LIBRARY_PATH")}
        cls.environment.setdefault("HOME", str(Path.home()))
        import resource
        cls.limit_core_dumps = staticmethod(lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        probe_path = cls.directory / "probe"
        probe = subprocess.run([cls.compiler, "-m32", "-x", "c", "-", "-o", str(probe_path)],
                               input="#include <stdio.h>\nint main(void) { return 0; }\n",
                               env=cls.environment, text=True, capture_output=True, timeout=30)
        if probe.returncode:
            raise unittest.SkipTest("a working 32-bit C development toolchain is required")
        try:
            probe = subprocess.run([str(probe_path)], env=cls.environment, text=True,
                                   capture_output=True, timeout=15, preexec_fn=cls.limit_core_dumps)
        except OSError as error:
            raise unittest.SkipTest("native i386 executables cannot run on this host") from error
        if probe.returncode:
            raise unittest.SkipTest("a working native i386 runtime is required")

    def run_fixture(self, name, expected_output):
        fixture = Path(__file__).resolve().with_name(name)
        output = self.directory / fixture.stem
        build = subprocess.run([self.compiler, "-m32", "-O2", "-std=c99", "-mstackrealign",
                                "-fPIE", "-pie", str(fixture), "-o", str(output), "-pthread", "-ldl"],
                               env=self.environment, text=True, capture_output=True, timeout=60)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        result = subprocess.run([str(output)], env=self.environment, text=True, capture_output=True,
                                timeout=30, preexec_fn=self.limit_core_dumps)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(expected_output, result.stdout, result.stdout + result.stderr)

    def test_review_patch_combinations_and_restoration(self):
        self.run_fixture("review_patch_test.c", "32 independent feature combinations")

    def test_mem_patch_checksum_modes_and_page_restoration(self):
        self.run_fixture("mem_patch_test.c", "memPatch modes, startup compatibility, SHA-256 identity, legacy warning, and page restoration passed")

    def test_horizon_compass_projects_world_bearings(self):
        self.run_fixture("horizon_compass_projection_test.c",
                         "horizon compass world-bearing, counter-clockwise west, pitch, and FOV projection passed")

    def test_render_modifier_rule_syntax_and_wildcards(self):
        self.run_fixture("render_modifier_test.c",
                         "render modifier phases, scope, legacy syntax, and glob matching passed")

    def test_autobeacon_assignment_and_restoration(self):
        self.run_fixture("autobeacon_test.c", "draw-time restoration and gates passed")

    def test_review_model_resource_diagnostic(self):
        self.run_fixture("review_models_test.c", "read-only traversal and malformed-memory gates passed")

    def test_review_hud_presentation_before_native_wake(self):
        self.run_fixture("review_hud_test.c", "reference fields precede native wake and fallback gates passed")

    def test_review_shortcut_descriptors_with_native_parser(self):
        client = Path(self.environment["HOME"]) / "t2-linux" / "tribes2.dynamic"
        if not client.is_file():
            self.skipTest("installed Loki client required for the real descriptor-parser fixture")
        self.run_fixture("review_shortcut_test.c", "native parser rejects old descriptors")

    def test_build_identifier_patches_in_client_and_dedicated(self):
        self.run_fixture("hook_won_patch_test.c", "RadarHUD LOS filtering, and Shazbot telemetry passed")

    def test_native_resource_collision_precedence(self):
        client = Path(self.environment["HOME"]) / "t2-linux" / "tribes2.dynamic"
        if not client.is_file():
            self.skipTest("installed Loki client required for the native resource lookup fixture")
        self.run_fixture("review_resource_lookup_test.c", "loose preference and gates passed")


@unittest.skipUnless((GAME / "tribes2d.dynamic").is_file() and (GAME / "tribes2.dynamic").is_file(),
                     "the Loki #25034 game is not installed in ~/t2-linux")
class GameBinaryTests(unittest.TestCase):
    def test_console_evaluate_early_mem_patch_gate_prologues(self):
        for binary, address in (("tribes2.dynamic", 0x080E88F4),
                                ("tribes2d.dynamic", 0x080E77E4)):
            with self.subTest(binary=binary):
                self.assertEqual(reader(GAME / binary)(address, 8),
                                 bytes.fromhex("55 89 e5 83 ec 0c 57 56"))

    def test_sky_rotation_patch_anchor(self):
        self.assertEqual(reader(GAME / "tribes2.dynamic")(0x08222390, 9),
                         bytes.fromhex("55 89 e5 81 ec ac 00 00 00"))

    def test_client_model_resource_layout(self):
        read = reader(GAME / "tribes2.dynamic")
        # Resource manager global, dictionary base, chain link and backing filename.
        for address, expected in (
            (0x0825D15C, "a1 e4 3d 49 08"),
            (0x08107A57, "81 c3 84 08 00 00"),
            (0x081061A2, "8b 5b 08"),
            (0x0810785E, "8b 47 20 8b 57 24"),
            (0x0810616A, "f7 71 08 8b 01 31 f6 8b 1c 90"),
            (0x081076E9, "89 43 34"),
            (0x0825D18E, "89 b0 b8 00 00 00"),
            (0x0825D11F, "8b 41 44"),
            (0x0810766B, "e8 54 10 00 00"),
            (0x081085A3, "e8 1c 01 00 00"),
            (0x0811513B, "e8 48 34 ff ff"),
            (0x08114EAC, "a1 94 41 49 08"),
            (0x08114EB1, "89 73 14"),
            (0x08114F26, "89 53 24"),
            (0x08114F35, "89 43 28"),
            (0x082FB9C0, "89 87 f8 01 00 00"),
            (0x082FBA17, "89 87 f4 01 00 00"),
            (0x082FCD32, "68 fc 01 00 00"),
            (0x082FCD4D, "68 00 02 00 00"),
            (0x082FB974, "e8 7f 85 e4 ff"),
            (0x082FCE40, "68 6c 02 00 00"),
            (0x082FCE5B, "68 70 02 00 00"),
            (0x082FCE76, "68 74 02 00 00"),
        ):
            data = bytes.fromhex(expected)
            self.assertEqual(read(address, len(data)), data)

    def test_shared_sites_match_in_each_executable(self):
        for name, target in TARGETS.items():
            with self.subTest(executable=name):
                read = reader(GAME / name)
                for address in target["console"]:
                    self.assertEqual(read(address, 3), PROLOGUE)
                self.assertEqual(read(target["clock"], 8), CLOCK)
                for index, address in enumerate(target["won"], 1):
                    self.assertEqual(read(address, 28), f"tribes2.m{index}.sierra.com:15104\0".encode())
                self.assertEqual(read(target["verifier"], 4), bytes.fromhex("30 82 03 2c"))
                rate, size = struct.pack("<I", target["rate"]), struct.pack("<I", target["size"])
                for site in target["net"]:
                    self.assertEqual(read(site, 7), b"\x83\x3d" + rate + b"\x20")
                    self.assertEqual(read(site + 0x09, 10), b"\xc7\x05" + rate + b"\x20\0\0\0")
                    self.assertEqual(read(site + 0x26, 10), b"\x81\x3d" + size + b"\xc2\x01\0\0")
                    self.assertEqual(read(site + 0x32, 10), b"\xc7\x05" + size + b"\xc2\x01\0\0")

    def test_client_interface_sites(self):
        read = reader(GAME / "tribes2.dynamic")
        # HudBarBaseCtrl::onRender asks the bar for its fill (vtable +0x118) and lays out the rect.
        stretch = read(0x0818EA5E, 0x73)
        self.assertEqual(stretch[:14], bytes.fromhex("8b 06 83 c4 f4 56 8b 80 18 01 00 00 ff d0"))
        self.assertEqual(stretch[-12:], bytes.fromhex("83 f8 01 75 07 c7 45 f8 02 00 00 00"))
        # The observer IFF camera check: je after dynamic_cast<Camera>.
        self.assertEqual(read(0x081936BF, 8), bytes.fromhex("74 4e 8b 88 bc 08 00 00"))
        # The LAN class-B join check: je 0x08237190 after the no-match flag test.
        self.assertEqual(read(0x082370CE, 6), bytes.fromhex("0f 84 bc 00 00 00"))
        # The visibleDistanceMod upper clamp: and $0x45,%ah ; je (to force 1.0).
        self.assertEqual(read(0x081DC7EF, 5), bytes.fromhex("80 e4 45 74 1a"))
        self.assertEqual(struct.unpack("<f", read(0x084492D4, 4))[0], 0.5)
        # GameConnection::readPacket whiteout: readFloat(7) * 1.5 -> +0x844c.
        self.assertEqual(read(0x08275B30, 2), bytes.fromhex("6a 07"))
        self.assertEqual(read(0x08275B38, 6), bytes.fromhex("dd 05 a8 c8 45 08"))
        self.assertEqual(read(0x08275B46, 6), bytes.fromhex("d9 99 4c 84 00 00"))
        self.assertEqual(struct.unpack("<d", read(0x0845C8A8, 8))[0], 1.5)
        # setFov's 179-degree maximum and fld1 minimum.
        self.assertEqual(read(0x08234171, 6), bytes.fromhex("d9 05 28 20 45 08"))
        self.assertEqual(struct.unpack("<f", read(0x08452028, 4))[0], 179.0)
        self.assertEqual(read(0x08234186, 4), bytes.fromhex("d9 e8 d8 d1"))
        # getDataBlock, setDataBlock and setTargetId client-object checks.
        self.assertEqual(read(0x0825C995, 5), bytes.fromhex("74 11 83 c4 f4"))
        self.assertEqual(read(0x0825BB17, 5), bytes.fromhex("74 17 83 c4 f4"))
        self.assertEqual(read(0x0825BBE2, 6), bytes.fromhex("0f 85 db 00 00 00"))
        # The alxGetWaveLen and alxCreateSource callbacks the safe-soundpacks
        # wrappers pass through to; both start with the function prologue.
        self.assertEqual(read(0x080CE9B4, 3), PROLOGUE)
        self.assertEqual(read(0x080CDD10, 3), PROLOGUE)

    def test_build_identifier_sites(self):
        for binary, address in (("tribes2.dynamic", 0x08309168),
                                ("tribes2d.dynamic", 0x082FDE08)):
            with self.subTest(binary=binary):
                read = reader(GAME / binary)
                self.assertEqual(read(address, 12), bytes.fromhex(
                    "55 89 e5 b8 ca 61 00 00 89 ec 5d c3"))
        # Clear only Torque's master-side version-match request bit; retain the
        # client's separate local comparison against getT2VersionNumber().
        read = reader(GAME / "tribes2.dynamic")
        self.assertEqual(read(0x0823E9B3, 3), bytes.fromhex("80 ca 80"))
        # Dedicated master-info serializer obtains the server build ID from
        # getT2VersionNumber at 0x082338e4 and writes it as a U32 at 0x08233904.
        read = reader(GAME / "tribes2d.dynamic")
        self.assertEqual(read(0x082338E4, 8), bytes.fromhex(
            "e8 1f a5 0c 00 eb 07 90"))
        self.assertEqual(read(0x08233904, 9), bytes.fromhex(
            "52 6a 04 56 8b 40 10 ff d0"))

    def test_demo_protocol_tag_sites(self):
        read = reader(GAME / "tribes2.dynamic")
        self.assertEqual(read(0x081F9AF8, 7), bytes.fromhex(
            "c7 45 d4 04 00 33 00"))
        self.assertEqual(read(0x081F9EEC, 5), bytes.fromhex("3d 04 00 33 00"))
        self.assertEqual(read(0x081FB974, 9), bytes.fromhex(
            "55 89 e5 b8 04 00 33 00 89"))
        self.assertEqual(read(0x081FB978, 4), bytes.fromhex("04 00 33 00"))

    def test_server_auth_sites(self):
        read = reader(GAME / "tribes2d.dynamic")
        self.assertEqual(read(0x0822B604, 10), bytes.fromhex("83 3d 34 60 6b 08 00 0f 95 c0"))
        self.assertEqual(read(0x0822BC85, 13), bytes.fromhex("83 3d 34 60 6b 08 00 0f 85 42 03 00 00"))
        # The flag replacement reads the pending connection's certificate, which
        # the challenge reply loads from the same field.
        self.assertEqual(read(0x0822B625, 6), bytes.fromhex("8b 86 54 21 00 00"))
        # Ping and info replies skipped for clients without a WON certificate.
        self.assertEqual(read(0x0823531E, 11), bytes.fromhex("8a 45 c4 a8 01 0f 85 30 02 00 00"))
        self.assertEqual(read(0x0823601B, 9), bytes.fromhex("f6 c3 01 0f 85 fb 05 00 00"))
        # usleep(1000) ends Platform::process in the server's main loop.
        self.assertEqual(read(0x081CF44C, 5), bytes.fromhex("68 e8 03 00 00"))

    def test_review_sites_and_semantic_anchors(self):
        read = reader(GAME / "tribes2.dynamic")
        # Native isPlayingDemo returns connection && connection->playbackStream.
        self.assertEqual(read(0x082395EC, 29), bytes.fromhex(
            "55 8b 15 dc 5c 49 08 89 e5 b0 00 85 d2 74 0a "
            "83 ba 20 82 00 00 00 0f 95 c0 89 ec 5d c3"))
        # Mesh renderer: instance alpha -> active alpha, not a cloak setter.
        self.assertEqual(read(0x0831EE13, 12), bytes.fromhex(
            "d9 05 94 f8 6b 08 d9 1d 10 f9 6b 08"))
        # FFI mask bit zero and remaining bits have separate visibility checks.
        self.assertEqual(read(0x08191036, 9), bytes.fromhex(
            "80 bb 04 02 00 00 00 75 19"))
        self.assertEqual(read(0x0819104E, 10), bytes.fromhex(
            "80 bc 1a 04 02 00 00 00 74 e8"))
        # Camera query and HUD independently load the scene's far distance.
        self.assertEqual(read(0x08232A68, 6), bytes.fromhex("d9 80 2c 0e 00 00"))
        self.assertEqual(read(0x081939D3, 6), bytes.fromhex("d9 80 2c 0e 00 00"))
        # Beacon sensor-group comparison; projection and near-zero guards stay.
        self.assertEqual(read(0x082FC6B7, 5), bytes.fromhex("3b 45 98 75 43"))
        self.assertEqual(read(0x082FC844, 6), bytes.fromhex("0f 84 7a 01 00 00"))
        self.assertEqual(struct.unpack("<d", read(0x08478710, 8))[0], 0.0001)
        # Autobeacon wraps the line renderer; the following pass keeps copied XYZ.
        self.assertEqual(read(0x082FD00D, 6), bytes.fromhex("53 e8 89 f4 ff ff"))
        self.assertEqual(read(0x082FC49C, 12), bytes.fromhex(
            "55 89 e5 81 ec 8c 00 00 00 8b 45 08"))
        self.assertEqual(read(0x082FD025, 6), bytes.fromhex("53 e8 fd f6 ff ff"))
        # Independent class-name/ID/field anchors used by the snapshot reader.
        self.assertEqual(read(0x080FBC77, 8), bytes.fromhex("8b 42 08 ff d0 8b 40 18"))
        self.assertEqual(read(0x080FD5AE, 8), bytes.fromhex("8b 41 20 25 ff 0f 00 00"))
        self.assertEqual(read(0x0831D365, 5), bytes.fromhex("68 88 08 00 00"))
        # SceneObject::onAdd copies its world transform (+0x9c) to render transform (+0xdc).
        self.assertEqual(read(0x08203130, 20), bytes.fromhex(
            "8d bb dc 00 00 00 8d b3 9c 00 00 00 fc b9 10 00 00 00 f3 a5"))
        # GameConnection::getControlObject returns the root's +0x825c Player pointer.
        self.assertEqual(read(0x0827916C, 37), bytes.fromhex(
            "55 89 e5 8b 45 08 31 d2 85 c0 74 06 8d 90 60 ff ff ff "
            "8b 92 5c 82 00 00 31 c0 85 d2 74 03 8b 42 20 89 ec 5d c3"))

    def test_shazbot_spatial_geometry_anchors(self):
        read = reader(GAME / "tribes2.dynamic")
        # getEyeTransform serializes the virtual MatrixF result into a Torque string;
        # its wrapper calls the object's virtual slot +0x160 with MatrixF* output.
        self.assertEqual(read(0x0826CD7C, 20), bytes.fromhex(
            "55 89 e5 83 ec 74 53 8b 55 08 8d 5d c0 83 c4 f8 8b 02 53 52"))
        # cGetCameraFov delegates to virtual slot +0x1d8 on server objects. The hook
        # reads that getter directly for its validated local Player ghost.
        self.assertEqual(read(0x0826D4C8, 29), bytes.fromhex(
            "55 89 e5 83 ec 08 8b 55 08 f6 42 40 02 75 10 8b 02 83 c4 f4 "
            "52 8b 80 d8 01 00 00 ff d0"))
        # SensorData target-manager record: ID selects base + 0x1a58 + ID*0x44;
        # the SensorData pointer is +0x40 and its datablock ID is +0x20.
        self.assertEqual(read(0x082E205F, 41), bytes.fromhex(
            "89 d0 c1 e0 04 01 d0 8d 04 85 58 1a 00 00 03 05 a0 7d 49 08 "
            "8b 40 40 85 c0 75 07 b8 ff ff ff ff eb 03 8b 40 20 89 ec 5d c3"))
        # SensorData::initPersistFields registers detectRadius at +0x4c.
        self.assertEqual(read(0x082D768E, 18), bytes.fromhex(
            "6a 00 6a 01 6a 4c 6a 05 68 a2 f7 46 08 e8 6c e8 e0 ff"))
        # Mounted-image getters confirm the eight-slot array base and 0x74 stride;
        # native muzzle-point/vector accessors are the wrappers used by telemetry.
        self.assertEqual(read(0x082733AC, 22), bytes.fromhex(
            "55 89 e5 6b 55 0c 74 8b 45 08 05 84 03 00 00 89 ec 5d 8b 04 02 c3"))
        self.assertEqual(read(0x0826C96C, 25), bytes.fromhex(
            "55 89 e5 83 ec 24 53 8b 45 10 8b 5d 08 83 c4 f4 8b 40 08 50 e8 07 2d f6 ff"))
        self.assertEqual(read(0x0826C9A4, 19), bytes.fromhex(
            "8b 13 83 c4 fc 8d 45 f4 50 51 53 8b 82 54 01 00 00 ff d0"))
        self.assertEqual(read(0x0826CA04, 22), bytes.fromhex(
            "55 89 e5 83 ec 24 53 8b 45 10 83 c4 f4 8b 40 08 50 e8 72 2c f6 ff"))
        self.assertEqual(read(0x08273758, 17), bytes.fromhex(
            "55 89 e5 83 ec 50 8b 4d 08 56 53 8b 75 0c 8b 5d 10"))
        # Aim-target billboard point is a world-space solver result; its renderer
        # passes that Point3F to dglDrawBillboard at this unique call site.
        self.assertEqual(read(0x082FC32F, 5), bytes.fromhex("e8 0c 2f e1 ff"))
        self.assertEqual(read(0x0810F240, 12), bytes.fromhex(
            "55 89 e5 81 ec fc 00 00 00 57 56 53"))

    def test_shazbot_observation_source_anchors(self):
        read = reader(GAME / "tribes2.dynamic")
        binary = (GAME / "tribes2.dynamic").read_bytes()
        # ShapeBase getVelocity callback confirms the virtual +0xec aggregate-
        # return ABI mirrored by the class-whitelisted direct getter call.
        self.assertEqual(read(0x0826CB50, 0x5C), bytes.fromhex(
            "55 89 e5 83 ec 20 56 53 8b 45 08 8d 5d f4 83 c4 f8 "
            "8b 10 50 53 8b 82 ec 00 00 00 ff d0 6a 64 e8 ac 50 e8 ff "
            "d9 43 08 83 c4 f4 83 ec 08 dd 1c 24 d9 43 04 83 ec 08 dd "
            "1c 24 d9 45 f4 89 c6 83 ec 08 dd 1c 24 68 9e 9b 45 08 6a "
            "64 56 e8 4f 27 f6 ff 89 f0 8d 65 d8 5b 5e 89 ec 5d c3"))
        for class_name in (b"Item", b"Vehicle", b"WheeledVehicle", b"FlyingVehicle", b"HoverVehicle"):
            self.assertIn(class_name + b"\0", binary)
        self.assertEqual(read(0x08248D98, 10), bytes.fromhex("c7 82 58 09 00 00 00 00 00 00"))
        self.assertEqual(read(0x08248DA2, 10), bytes.fromhex("c7 82 5c 09 00 00 00 00 00 00"))
        self.assertEqual(read(0x08248DAC, 10), bytes.fromhex("c7 82 60 09 00 00 00 00 00 00"))
        # getEnergyPercent confirms PlayerData inherit flag/maxEnergy and Player mEnergy fields.
        self.assertEqual(read(0x0826CEA8, 51), bytes.fromhex(
            "55 89 e5 8b 4d 08 8b 91 d0 02 00 00 80 ba 2b 03 00 00 00 75 23 "
            "d9 ee d9 82 80 00 00 00 d8 d1 df e0 80 e4 45 fe cc 80 fc 40 "
            "dd d9 72 5f d9 81 7c 07 00 00"))
        # ShapeBase::getDamageValue confirms mDamage at Player+0x7f0 and
        # maxDamage at PlayerData+0x84 for normalized health fraction.
        self.assertEqual(read(0x0826D000, 28), bytes.fromhex(
            "55 89 e5 8b 45 08 89 ec 8b 90 d0 02 00 00 5d d9 80 f0 07 00 00 "
            "d8 b2 84 00 00 00 c3"))
        # PlayerData::initPersistFields registers maxForwardSpeed at +0x380.
        self.assertEqual(read(0x082465B6, 12), bytes.fromhex(
            "68 80 03 00 00 6a 05 68 34 67 45 08"))
        self.assertEqual(read(0x0826CEE0, 64), bytes.fromhex(
            "8b 91 e4 07 00 00 85 d2 74 4c 8b 82 d0 02 00 00 d9 ee "
            "d9 80 80 00 00 00 d8 d1 df e0 80 e4 45 fe cc 80 fc 40 "
            "dd d9 72 2c 8b 82 d0 02 00 00 80 b8 2b 03 00 00 00 75 09 "
            "d9 82 7c 07 00 00 eb bc 90"))
        self.assertEqual(read(0x0826CF20, 28), bytes.fromhex(
            "8b 82 e4 07 00 00 85 c0 74 06 89 c2 eb da 89 f6 d9 ee "
            "eb a7 dd d8 d9 ee 89 ec 5d c3"))
        # Player mJumpSurfaceNormal (+0x990) is initialized to (0,0,1).
        self.assertEqual(read(0x08248DB6, 10), bytes.fromhex("c7 82 90 09 00 00 00 00 00 00"))
        self.assertEqual(read(0x08248DC0, 10), bytes.fromhex("c7 82 94 09 00 00 00 00 00 00"))
        self.assertEqual(read(0x08248DCA, 10), bytes.fromhex("c7 82 98 09 00 00 00 00 80 3f"))
        # ContactInfo at +0xa3c: contacted/jump/run bytes, then contact normal at +0xa40.
        self.assertEqual(read(0x08248A23, 45), bytes.fromhex(
            "c7 86 40 0a 00 00 00 00 80 3f c6 86 3e 0a 00 00 00 "
            "c6 86 3d 0a 00 00 00 c6 86 3c 0a 00 00 00 "
            "c7 40 04 00 00 80 3f c7 40 08 00 00 80 3f"))
        # updateMove copies a fresh findContact normal to +0x990 only if its current jump flag is set.
        self.assertEqual(read(0x0824B27C, 36), bytes.fromhex(
            "80 bd 37 ff ff ff 00 74 1b 8b 45 88 89 83 90 09 00 00 "
            "8b 45 8c 89 83 94 09 00 00 8b 45 90 89 83 98 09 00 00"))
        self.assertEqual(read(0x0824B258, 36), bytes.fromhex(
            "83 bb e4 07 00 00 00 75 1b 8d 45 88 50 8d 85 37 ff ff ff "
            "50 8d 85 36 ff ff ff 50 53 e8 97 52 00 00 83 c4 10"))
        self.assertEqual(read(0x08250DC6, 6), bytes.fromhex("88 87 3c 0a 00 00"))
        self.assertEqual(read(0x08250DF6, 11), bytes.fromhex(
            "8b 55 10 8a 02 88 87 3d 0a 00 00"))
        self.assertEqual(read(0x08250DC6, 6), bytes.fromhex("88 87 3c 0a 00 00"))
        self.assertEqual(read(0x08250DF6, 11), bytes.fromhex(
            "8b 55 10 8a 02 88 87 3d 0a 00 00"))
        # ShapeBase::setSensorDataBlockVisibility adds/removes ghosted objects
        # from the current-visible SimSet at this exact global.
        self.assertEqual(read(0x0825C8F8, 46), bytes.fromhex(
            "55 89 e5 83 ec 10 56 53 8b 5d 08 8b 75 0c f6 43 40 02 "
            "74 29 85 f6 74 11 a1 ec 32 4e 08 83 c4 f8 8b 10 53 50 "
            "8b 42 4c eb 0f a1 ec 32 4e 08"))
        # ClientTargetList indexes 16-byte records at +0x10; its count is +0x210.
        self.assertEqual(read(0x082E2850, 24), bytes.fromhex(
            "55 89 e5 8b 55 08 8b 45 0c 3b 82 10 02 00 00 73 0f "
            "c1 e0 04 8d 44 02 10"))
        self.assertEqual(read(0x082E2878, 66), bytes.fromhex(
            "55 89 e5 83 ec 0c 57 8b 7d 08 56 53 8b 75 0c 8b 5d 14 "
            "83 c4 f4 57 e8 61 91 e1 ff c7 47 38 00 00 00 00 c7 07 "
            "a0 d1 4a 08 8d 47 38 c7 40 04 00 00 00 00 c7 40 08 00 "
            "00 00 00 8b 45 10 89 77 30 89 47 34"))
        # TargetToEvent writes type 0/1 from its assigned/potential flag; the
        # local cCreateWaypoint path uses type 2.
        self.assertEqual(read(0x0827B066, 15), bytes.fromhex(
            "80 7b 20 00 0f 94 c0 89 c2 83 e2 01 89 57 30"))
        self.assertEqual(read(0x082E2A94, 7), bytes.fromhex(
            "c7 43 30 02 00 00 00"))
        # ClientTarget refresh follows its target ID into manager+0xa258, only
        # when record+0x3c bit 0 and the current target object (+0x24) are set.
        self.assertEqual(read(0x082DD8FE, 13), bytes.fromhex(
            "f6 40 3c 01 74 49 8b 50 24 85 d2 74 42"))
        # The #25034 initializer allocates a 0x2a0 ClientTargetList and stores it
        # in global 0x08497da4.
        self.assertEqual(read(0x082DC13A, 21), bytes.fromhex(
            "68 a0 02 00 00 e8 ec 18 d7 ff 50 e8 ee 12 00 00 "
            "a3 a4 7d 49 08"))
        # The FFI visibility/HUD collection reads GameConnection sensor group +0x8434.
        self.assertEqual(read(0x0819401A, 12), bytes.fromhex(
            "8b 81 34 84 00 00 89 86 d8 00 00 00"))


if __name__ == "__main__":
    unittest.main()
