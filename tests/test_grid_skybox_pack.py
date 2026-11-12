"""Checks the minimal grid sky VL2 and its datablock modifier reference."""

import unittest
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "zGridSkyBox.vl2"
FACE_NAMES = ("FR", "RT", "BK", "LF", "UP", "DN")


class GridSkyboxPackTests(unittest.TestCase):
    def test_archive_contains_only_grid_faces_and_one_dml(self):
        expected = {f"textures/koko/skies/Grid_{face}.png" for face in FACE_NAMES}
        expected.add("textures/grid.dml")
        with ZipFile(ARCHIVE) as volume:
            self.assertIsNone(volume.testzip())
            self.assertEqual(set(volume.namelist()), expected)
            dml = volume.read("textures/grid.dml").decode("ascii").splitlines()
            self.assertEqual(dml, [f"koko/skies/Grid_{face}" for face in FACE_NAMES])

    def test_datablock_example_points_at_the_pack_dml(self):
        config = (ROOT / "runtime/render-modifiers.datablocks.example.cfg").read_text()
        self.assertIn('client connect class:Sky.materialList = "textures/grid.dml"', config)
        with ZipFile(ARCHIVE) as volume:
            self.assertIn("textures/grid.dml", volume.namelist())


if __name__ == "__main__":
    unittest.main()
