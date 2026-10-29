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

    def test_native_resource_collision_precedence(self):
        client = Path(self.environment["HOME"]) / "t2-linux" / "tribes2.dynamic"
        if not client.is_file():
            self.skipTest("installed Loki client required for the native resource lookup fixture")
        self.run_fixture("review_resource_lookup_test.c", "loose preference and gates passed")


@unittest.skipUnless((GAME / "tribes2d.dynamic").is_file() and (GAME / "tribes2.dynamic").is_file(),
                     "the Loki #25034 game is not installed in ~/t2-linux")
class GameBinaryTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
