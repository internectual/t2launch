import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import t2options  # noqa: E402


class AtariTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("gcc"), "needs gcc")
    def test_core_builds_and_passes_its_own_tests(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "atari_core_test"
            subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-pthread", "-o", str(exe),
                            str(ROOT / "tests" / "atari_core_test.c"), "-lm"],
                           check=True, capture_output=True, text=True)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("0 failures", run.stdout)

    def test_demo_rom_assembles_to_the_committed_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "t2demo.bin"
            subprocess.run([sys.executable, str(ROOT / "scripts" / "Atari" / "make_demo_rom.py"), str(out)],
                           check=True, capture_output=True, text=True)
            rom = out.read_bytes()
        self.assertEqual(len(rom), 4096)
        self.assertEqual(rom[0xFFC:0xFFE], bytes([0x00, 0x10]))      # reset vector -> $1000
        self.assertEqual(rom, (ROOT / "scripts" / "Atari" / "t2demo.bin").read_bytes())

    def test_options_exist_and_are_dedicated_server_only(self):
        keys = {option.key: option for option in t2options.OPTIONS}
        for key in ("server.atari_rom_dir", "server.atari_region", "server.atari_grid"):
            self.assertIn(key, keys)
            self.assertTrue(keys[key].hook)
            self.assertIn("game.executable", [r[0] for r in keys[key].requires])
        self.assertEqual(keys["server.atari_region"].choices, ("ntsc", "pal"))
        self.assertEqual(keys["server.atari_grid"].default, "40x48")

    def test_hook_registers_the_atari_functions(self):
        hook = (ROOT / "t2_hook.c").read_text()
        for name in ("atariGrid", "atariColor", "atariList", "atariLoad", "atariStart", "atariStop", "atariReset",
                     "atariInput", "atariSwitch", "atariFrame", "atariStatus"):
            self.assertIn(f'"t2launch_{name}"', hook)
        self.assertIn("if (target->dedicated) atari_register_commands();", hook)
        for option in ("server.atari_rom_dir", "server.atari_region", "server.atari_grid"):
            self.assertIn(f'{{"{option}"', hook)
        self.assertIn("t2_atari.h", (ROOT / "runtime" / "run-game").read_text())

    def test_script_and_mission_are_consistent(self):
        script = (ROOT / "scripts" / "Gametypes" / "Atari.cs").read_text()
        for needle in ('$Atr::Mission = "MatrixAtari"', "function Observer::onTrigger", "t2launch_atariFrame",
                       "t2launch_atariLoad", "datablock ForceFieldBareData(AtrCol"):
            self.assertIn(needle, script)
        mission = (ROOT / "scripts" / "Matrix" / "missions" / "MatrixAtari.mis").read_text()
        self.assertTrue(mission.startswith("// DisplayName = Matrix Atari\n// MissionTypes = DM\n"))
        self.assertNotIn("TerrainBlock", mission)


if __name__ == "__main__":
    unittest.main()
