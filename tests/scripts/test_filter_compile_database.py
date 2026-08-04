import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "filter_compile_database.py"
SPEC = importlib.util.spec_from_file_location("filter_compile_database", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class FilterCompileDatabaseTest(unittest.TestCase):
    def test_filters_by_exact_resolved_path_not_suffix(self):
        root = pathlib.Path("/workspace")
        database = [
            {
                "directory": "/workspace/build",
                "file": "/workspace/src/foo.cpp",
                "command": "c++ src/foo.cpp",
            },
            {
                "directory": "/workspace/build",
                "file": "/workspace/src/nested/src/foo.cpp",
                "command": "c++ nested/src/foo.cpp",
            },
        ]

        self.assertEqual(
            MODULE.filter_database(
                database, root, [pathlib.Path("src/foo.cpp")]
            ),
            [database[0]],
        )

    def test_missing_changed_source_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "missing compile command"):
            MODULE.filter_database(
                [], pathlib.Path("/workspace"), [pathlib.Path("src/missing.cpp")]
            )


if __name__ == "__main__":
    unittest.main()
