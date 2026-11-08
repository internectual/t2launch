"""Structural and dependency checks for the standalone DOOMHud VL2."""

import re
import unittest
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1] / "scripts" / "DOOMHud"
ARCHIVE = ROOT / "DOOMHud.vl2"

class DoomHudPackTests(unittest.TestCase):
    def test_archive_contains_the_source_assets_and_loadable_script(self):
        source_script = (ROOT / "scripts/autoexec/DOOMHud.cs").read_bytes()
        with ZipFile(ARCHIVE) as volume:
            names = set(volume.namelist())
            self.assertIsNone(volume.testzip())
            self.assertEqual(volume.read("scripts/autoexec/DOOMHud.cs"), source_script)
            self.assertIn("gui/DOOMGui.gui", names)
            self.assertIn("scripts/autoexec/DOOMHud.txt", names)
            self.assertEqual(
                {name for name in names if name.startswith("fonts/") and name.endswith(".gft")},
                {f"fonts/doom_{size}.gft" for size in (8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 32)},
            )
            gui = volume.read("gui/DOOMGui.gui").decode("utf-8")
            for bitmap in re.findall(r'bitmap\s*=\s*"(DOOM/[^"]+)"', gui):
                self.assertIn(f"textures/{bitmap}.png", names, bitmap)

    def test_updated_hud_has_no_legacy_executable_or_support_pack_dependency(self):
        script = (ROOT / "scripts/autoexec/DOOMHud.cs").read_text()
        gui = (ROOT / "gui/DOOMGui.gui").read_text()
        self.assertIn("DOOMHealthValue.value", script)
        self.assertIn("DOOMEnergyValue.value", script)
        self.assertNotIn(".getValue()", script)
        self.assertIn('DOOMHud.horizSizing = "center";', script)
        self.assertIn('DOOMHud.vertSizing = "top";', script)
        self.assertIn('horizSizing = "center";', gui)
        self.assertIn('vertSizing = "top";', gui)
        self.assertIn('position = "0 416";', gui)
        for obsolete in ("xHealth.value", "xEnergy.value", "loadout.", "hudmover::", "t2health.exe"):
            self.assertNotIn(obsolete, script)
        self.assertIn("if (isObject(PlayGui)) DOOMHuds.Setup();", script)
        demo = (Path(__file__).parent / "hudBarDemo.cs").read_text()
        self.assertIn("if (isObject(DOOMHuds))", demo)
        self.assertIn("DOOMHuds.Setup();", demo)


if __name__ == "__main__":
    unittest.main()
