import re
import struct
import unittest
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "scripts" / "Matrix"


class MatrixPackTests(unittest.TestCase):
    def test_source_tree_describes_a_terrainless_map(self):
        mission = (PACK / "missions" / "Matrix.mis").read_text()
        self.assertTrue(mission.startswith("// DisplayName = Matrix\n// MissionTypes = DM\n"))
        self.assertNotIn("TerrainBlock", mission)
        for required in ("new MissionArea(MissionArea)", "new Sky(Sky)", "new Sun()", "new SimGroup(ObserverDropPoints)",
                         'dataBlock = "Observer"', 'new SpawnSphere()', 'materialList = "Matrix.dml"',
                         'renderBottomTexture = "1"', 'interiorFile = "pplat4.dif"'):
            self.assertIn(required, mission)
        area = re.search(r'area = "(-?\d+) (-?\d+) (\d+) (\d+)"', mission)
        self.assertIsNotNone(area)
        self.assertGreaterEqual(int(area.group(3)), 2000)   # the player must never leave the mission area
        dml = (PACK / "textures" / "Matrix.dml").read_bytes()
        self.assertEqual(dml.split(b"\r\n")[:6],
                         [f"matrix/skies/Matrix_{face}".encode() for face in ("FR", "RT", "BK", "LF", "UP", "DN")])
        self.assertTrue(dml.endswith(b"\r\n"))

    def test_face_textures_are_512_square_pngs(self):
        for face in ("FR", "RT", "BK", "LF", "UP", "DN"):
            data = (PACK / "textures" / "matrix" / "skies" / f"Matrix_{face}.png").read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            width, height = struct.unpack(">II", data[16:24])
            self.assertEqual((width, height), (512, 512), face)

    def test_built_volume_matches_the_source_tree(self):
        volume = PACK / "zMatrix.vl2"
        if not volume.is_file():
            self.skipTest("run scripts/Matrix/build_vl2.py first")
        with ZipFile(volume) as archive:
            self.assertIsNone(archive.testzip())
            names = set(archive.namelist())
            expected = {path.relative_to(PACK).as_posix()
                        for directory in ("missions", "scripts", "textures")
                        for path in (PACK / directory).rglob("*") if path.is_file()}
            self.assertEqual(names, expected)
            self.assertIn("scripts/autoexec/Matrix.cs", names)
            script = archive.read("scripts/autoexec/Matrix.cs").decode()
            self.assertIn("t2launch_skyFullBox(1)", script)
            self.assertIn("$T2Launch::SkyViews", script)


if __name__ == "__main__":
    unittest.main()
