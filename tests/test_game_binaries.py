"""The hook's patch sites hold the expected bytes in the installed binaries.

The addresses mirror game_targets[] and the server-only sites in t2_hook.c.
Skipped when the Loki #25034 game is not installed in ~/t2-linux.
"""

import struct
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


@unittest.skipUnless((GAME / "tribes2d.dynamic").is_file() and (GAME / "tribes2.dynamic").is_file(),
                     "the Loki #25034 game is not installed in ~/t2-linux")
class GameBinaryTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
