"""Checks the stand-alone horizontal-compass/HUDbot demo volume."""

import struct
import unittest
from pathlib import Path
from zipfile import ZipFile

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO / "scripts" / "HudDemo"
ARCHIVE = ROOT / "HudDemo.vl2"


class HudDemoPackTests(unittest.TestCase):
    def test_volume_contains_script_hudbot_variants_and_compass_assets(self):
        script = (REPO / "tests" / "hudBarDemo.cs").read_bytes()
        with ZipFile(ARCHIVE) as volume:
            names = set(volume.namelist())
            self.assertIsNone(volume.testzip())
            self.assertEqual(volume.read("scripts/autoexec/hudBarDemo.cs"), script)
            for mode, health_size, energy_size in (
                ("hudbot_horizontal", (128, 16), (128, 8)),
                ("hudbot_vertical", (16, 128), (8, 128)),
            ):
                for filename, expected in (
                    ("health_bar.png", health_size),
                    ("health_frame.png", health_size),
                    ("energy_bar.png", energy_size),
                    ("energy_frame.png", energy_size),
                ):
                    path = f"hudBarDemo_assets/{mode}/{filename}"
                    data = volume.read(path)
                    self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                    self.assertEqual(struct.unpack_from(">II", data, 16), expected)
            self.assertIn("textures/gui/uicompass.jpg", names)
            self.assertIn("textures/gui/uibackgroundcompass.png", names)
            self.assertIn("gui/uicompass.jpg", names)
            self.assertIn("gui/uibackgroundcompass.png", names)
            strip = volume.read("hudBarDemo_assets/compass_strip.png")
            self.assertEqual(strip[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack_from(">II", strip, 16), (630, 14))
            self.assertEqual(strip[25], 6)  # RGBA alpha mask derived from the beta JPG.
            # The compass markers come from the stock textures.vl2.
            for direction in ("north", "south", "east", "west"):
                self.assertNotIn(f"textures/gui/hud_{direction}.png", names)
                self.assertNotIn(f"schemes/DemoPack3/hud_{direction}.png", names)

    def test_demo_exposes_three_modes_and_uses_t2_camera_heading(self):
        script = (REPO / "tests" / "hudBarDemo.cs").read_text()
        self.assertIn('if (%mode !$= "circular"', script)
        self.assertIn('%mode !$= "horizontal"', script)
        self.assertIn('%mode !$= "both"', script)
        self.assertIn('$T2Launch::HudDemo::CompassMode = "both"', script)
        self.assertIn("hook_hud_demo_heading()", script)
        self.assertNotIn("getEyeVector()", script)
        self.assertIn("hook_hud_demo_compass_markers(HudDemoCompassNorth.getId()", script)
        for direction in ("north", "south", "east", "west"):
            self.assertIn(f'"gui/hud_{direction}"', script)
        self.assertIn('"hudBarDemo_assets/compass_strip.png"', script)
        self.assertIn('HudDemoCompassNorth.visible = "0"', script)
        self.assertIn('HudDemoCompassStrip.visible = "0"', script)
        self.assertIn('opacity = "0.9"', script)
        self.assertNotIn(".getClassName()", script)
        self.assertIn("compassHud.setVisible(%circle)", script)
        self.assertIn("hudCompassBack.setVisible(%circle)", script)
        self.assertIn("hudBarDemoSetCompassMode(%mode)", script)
        self.assertIn("$T2Launch::HudDemo::Radar = 1;", script)
        readme = (ROOT / "README.md").read_text()
        for credit in ("PenguinOfDoom", "Andrew", "NoFix"):
            self.assertIn(credit, readme)

    def test_demo_updates_art_masked_fills_without_polling_get_value(self):
        script = (REPO / "tests" / "hudBarDemo.cs").read_text()
        self.assertIn("function hudBarDemoBitmapFill", script)
        self.assertIn("hudBarDemoUpdateSkinFill(HudBarDemoSkinHealthFill", script)
        self.assertIn("hudBarDemoUpdateSkinFill(HudBarDemoVerticalHealthFill", script)
        self.assertIn('opacity = "0"', script)
        self.assertNotIn(".getValue()", script)
        self.assertNotIn("mClampF(", script)

    def test_bar_examples_anchor_bottom_right_clear_of_stock_hud_column(self):
        script = (REPO / "tests" / "hudBarDemo.cs").read_text()
        self.assertIn('extent = "440 350"', script)
        self.assertIn("function hudBarDemoAnchor()", script)
        self.assertIn("%x = %width - 440 - 176;", script)
        self.assertIn("%y = %height - 350 - 10;", script)
        self.assertIn("HudBarDemoSkin.position = (%x + 205) SPC (%y + 190);", script)
        self.assertIn("HudBarDemoSkinVertical.position = (%x + 378) SPC (%y + 185);", script)


if __name__ == "__main__":
    unittest.main()
