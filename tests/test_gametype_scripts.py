import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAMETYPES = ROOT / "scripts" / "Gametypes"
MISSIONS = ROOT / "scripts" / "Matrix" / "missions"

# (script, mission, display name)
PACKS = [
    ("Tron.cs", "MatrixTron", "Matrix Tron"),
    ("Tetris.cs", "MatrixTetris", "Matrix Tetris"),
    ("Snake.cs", "MatrixSnake", "Matrix Snake"),
    ("Pong.cs", "MatrixPong", "Matrix Pong"),
    ("Atari.cs", "MatrixAtari", "Matrix Atari"),
]


class GametypeScriptTests(unittest.TestCase):
    def test_every_script_matches_its_mission(self):
        for script, mission, display in PACKS:
            text = (GAMETYPES / script).read_text()
            self.assertIn(f'"{mission}"', text, script)
            self.assertIn("activatePackage(", text, script)
            self.assertIn("if (!$", text, script)                 # installs only once
            mis = (MISSIONS / f"{mission}.mis").read_text()
            self.assertTrue(mis.startswith(f"// DisplayName = {display}\n// MissionTypes = DM\n"), mission)
            self.assertNotIn("TerrainBlock", mis)

    def test_braces_and_parentheses_balance(self):
        for script, _, _ in PACKS:
            text = (GAMETYPES / script).read_text()
            code = re.sub(r"//[^\n]*", "", text)
            code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)       # drop string contents
            for open_, close in ("{}", "()", "[]"):
                self.assertEqual(code.count(open_), code.count(close), f"{script}: unbalanced {open_}{close}")

    def test_no_nested_ternary_that_yields_a_negative_number(self):
        # In this engine `a ? 1 : (b ? -1 : 0)` prints -1 but multiplies as 4294967295; use if/else instead.
        pattern = re.compile(r"\?[^;:]*:\s*\([^;]*\?\s*-\d")
        for script, _, _ in PACKS:
            for number, line in enumerate((GAMETYPES / script).read_text().splitlines(), 1):
                if line.strip().startswith("//"):
                    continue
                self.assertIsNone(pattern.search(line), f"{script}:{number}: {line.strip()}")

    def test_scripts_can_be_loaded_together(self):
        # each script keeps its names under its own prefix, so a comma list of all of them cannot collide
        prefixes = {"Tron.cs": "tron", "Tetris.cs": "tet", "Snake.cs": "snk", "Pong.cs": "png", "Atari.cs": "atr"}
        for script, prefix in prefixes.items():
            text = (GAMETYPES / script).read_text()
            for name in re.findall(r"^function (\w+)\(", text, re.M):
                if "::" in name:
                    continue
                self.assertTrue(name.startswith(prefix), f"{script}: {name} lacks the {prefix} prefix")


if __name__ == "__main__":
    unittest.main()
