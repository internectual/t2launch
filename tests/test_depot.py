import hashlib
import tempfile
import unittest
import unittest.mock
from pathlib import Path, PurePosixPath

import depot

TERRAIN = b"terrain bytes"
INTERIOR = b"interior bytes"
INDEX = "\n".join([
    f"{len(TERRAIN)} {hashlib.md5(TERRAIN).hexdigest()} terrains/Keg'Ren.ter",
    f"{len(INTERIOR)} {hashlib.md5(INTERIOR).hexdigest()} interiors/lwt_Map/ssplh_tower.dif",
    f"{len(INTERIOR)} {hashlib.md5(INTERIOR).hexdigest()} interiors/Hell's Gate/ssplh_tower.dif.bak",
    f"7 {'0' * 32} terrains/POPO's Map.ter",
    f"7 {'0' * 32} ../escape.ter",
    "not an index line",
])


class FakeDepot:
    def __init__(self, files):
        self.files = files
        self.urls = []

    def __call__(self, url, limit):
        self.urls.append(url)
        if url == depot.INDEX_URL:
            return INDEX.encode()
        data = self.files[url]
        if len(data) > limit:
            raise ValueError("download is larger than expected")
        return data


class DepotTests(unittest.TestCase):
    def test_index_keeps_paths_with_spaces_and_drops_unsafe_lines(self):
        paths = [entry.path for entry in depot.parse_index(INDEX)]
        self.assertIn("terrains/POPO's Map.ter", paths)
        self.assertNotIn("../escape.ter", paths)
        self.assertEqual(len(paths), 4)

    def test_message_names_map_to_their_folders(self):
        self.assertEqual(depot.asset_path("TribalArena.ter."), PurePosixPath("terrains/TribalArena.ter"))
        self.assertEqual(depot.asset_path("2wayramp.dif"), PurePosixPath("interiors/2wayramp.dif"))
        self.assertEqual(depot.asset_path("shapes/bmisc.dts"), PurePosixPath("shapes/bmisc.dts"))
        for name in ("../x.dif", "/etc/x.ter", "a\\b.dts", "textures/x.png", "x.cs"):
            self.assertIsNone(depot.asset_path(name), name)

    def test_lookup_ignores_case_and_falls_back_to_a_map_subfolder(self):
        entries = depot.parse_index(INDEX)
        found = depot.find_entry(entries, PurePosixPath("terrains/keg'ren.ter"))
        self.assertEqual(found.path, "terrains/Keg'Ren.ter")
        found = depot.find_entry(entries, PurePosixPath("interiors/SSPLH_tower.dif"))
        self.assertEqual(found.path, "interiors/lwt_Map/ssplh_tower.dif")
        self.assertIsNone(depot.find_entry(entries, PurePosixPath("shapes/ssplh_tower.dif")))

    def test_download_checks_md5_and_saves_where_the_game_looks(self):
        fake = FakeDepot({
            depot.FILE_BASE_URL + "terrains/Keg%27Ren.ter": TERRAIN,
            depot.FILE_BASE_URL + "interiors/lwt_Map/ssplh_tower.dif": INTERIOR,
        })
        with tempfile.TemporaryDirectory() as folder:
            base, cache = Path(folder) / "base", Path(folder) / "cache" / "depot.txt"
            saved = depot.download("keg'ren.ter.", base, cache, fake)
            self.assertEqual(saved, base / "terrains" / "keg'ren.ter")
            self.assertEqual(saved.read_bytes(), TERRAIN)
            saved = depot.download("SSPLH_tower.dif", base, cache, fake)
            self.assertEqual(saved, base / "interiors" / "SSPLH_tower.dif")
            # The cached index is reused, and an installed file isn't fetched again.
            self.assertEqual(fake.urls.count(depot.INDEX_URL), 1)
            with self.assertRaises(FileExistsError):
                depot.download("SSPLH_tower.dif", base, cache, fake)

    def test_download_rejects_a_corrupt_file(self):
        fake = FakeDepot({depot.FILE_BASE_URL + "terrains/Keg%27Ren.ter": b"x" * len(TERRAIN)})
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder) / "base"
            with self.assertRaisesRegex(ValueError, "MD5"):
                depot.download("Keg'Ren.ter", base, Path(folder) / "depot.txt", fake)
            self.assertFalse((base / "terrains").exists())

    def test_main_reports_one_status_line(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = Path(folder) / "depot.txt"
            cache.write_text(INDEX)
            with unittest.mock.patch("builtins.print") as printed:
                self.assertEqual(depot.main(["missing.dif", "--dest", folder, "--cache", str(cache)]), 1)
            printed.assert_called_once_with("error missing.dif is not in the depot")


if __name__ == "__main__":
    unittest.main()
